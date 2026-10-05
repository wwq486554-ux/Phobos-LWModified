#include "ViewZoom.h"

#include <CCINIClass.h>
#include <DisplayClass.h>
#include <MapClass.h>
#include <Memory.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <Utilities/Debug.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
	// Rejected experiment, kept so it is not retried (2026-10-04):
	//
	// 006DA943 is the "list is full" branch of the visible-cell insert. The
	// native code falls through to 006DA961 and clears the cell's pending flag
	// even though the insert was skipped, which looks like a permanently lost
	// update. Patching only the branch displacement (7D 18 -> 7D 23) so the
	// full case jumps to 006DA968 -- keeping the flag set -- was tried in game:
	// it crashed twice (2560x1600 k=1.5, and 800x600 with the probe on) with a
	// wild rep stosd (write to 0x1DB28000, count 0x3F4A1D28) at 007BD03F, i.e.
	// the engine ends up in an illegal state. Do NOT re-enable it.
	//
	// The same runs also showed the 800 entry cap never firing (list peak
	// 357-365 / 800, dropped 0), so the cap is not what leaves the enlarged
	// viewport unpainted -- see the hand-over manual.

	// Defaults measured in stage A: the 2K tactical view already burns 45-52%
	// of one core, so the first release only shrinks down to 1.5x.
	constexpr double DefaultLevels[] = { 0.25, 0.33, 0.5, 0.67, 1.0, 1.25, 1.5, 2.0, 2.5 };
	constexpr int DefaultLevelCount = static_cast<int>(sizeof(DefaultLevels) / sizeof(DefaultLevels[0]));

	// Window messages. The window procedure dispatches messages above 0x205
	// through the site this class is fed from, which covers the middle button.
	constexpr unsigned int MiddleButtonDownMessage = 0x0207u;
	constexpr unsigned int MiddleButtonUpMessage = 0x0208u;
	constexpr unsigned int MiddleButtonDoubleClickMessage = 0x0209u;

	// Same drag threshold the game client uses for its own click/drag split.
	constexpr int MiddleButtonDragThreshold = 4;

	// The engine's tactical surfaces are 16 bit; if a build ever runs with a
	// different depth the zoom simply stays off instead of corrupting pixels.
	constexpr int RequiredBytesPerPixel = 2;

	// Upper bound on the scratch allocation (pixels), mirrors the sanity check
	// the reference implementation uses.
	constexpr unsigned int MaxScratchPixels = 0x20000000u;

	// The whole-map level has no fixed factor: its viewport is the loaded map's
	// own pixel bounds. The table stores this sentinel and the real value is
	// derived at runtime from MapClass.
	constexpr double WholeMapFactor = -1.0;

	char LowerAscii(char value)
	{
		return (value >= 'A' && value <= 'Z') ? static_cast<char>(value + 32) : value;
	}

	bool IsWholeMapToken(const char* pText)
	{
		if (!pText)
			return false;

		const char* pKeywords[] = { "full", "whole", "wholemap", "all" };

		for (const char* pKeyword : pKeywords)
		{
			const char* pLeft = pText;
			const char* pRight = pKeyword;

			while (*pLeft && LowerAscii(*pLeft) == *pRight)
			{
				++pLeft;
				++pRight;
			}

			if (!*pLeft && !*pRight)
				return true;
		}

		return false;
	}

	// DSurface is sizeof(0x24) and is created with
	// `operator new(0x24)` + ctor 0x4BA5A0 for every engine surface.
	constexpr unsigned int DSurfaceSize = 0x24u;
	constexpr unsigned int DSurfaceCtor = 0x4BA5A0u;

	// TacticalClass::view_bound, written by TacticalClass::SetViewDimensions
	// (0x6D5F60) and read by the renderer for the visible cell range.
	constexpr unsigned int TacticalViewBound = 0xB0CE28u;

	// TacticalClass::SetViewDimensions and the helper it calls at its end. The
	// latter recomputes the internal view origin (+0xB0/+0xB4) and rebuilds the
	// isometric matrix (+0xDE4) from view_bound and the camera centre, which is
	// what object and building projection uses.
	constexpr unsigned int TacticalSetViewDimensions = 0x6D5F60u;
	constexpr unsigned int TacticalRebuildView = 0x6D8B30u;

	// Two engine objects are plain `width * height * 2` byte scratch buffers
	// built from the view rectangle, and they are rebuilt only by the engine's
	// own view routine at 0x4A8960 (the one that logs Set_View_Dimensions):
	//
	//   0x887644 - the tactical translucency/shadow scratch, ctor 0x7BC970
	//   0x87E8A4 - the player visibility (ABuffer) scratch, ctor 0x410CE0
	//
	// Changing the viewport without rebuilding them means the renderer writes a
	// viewport's worth of pixels into a native-sized buffer. A 1.25x viewport
	// already overruns them by 2.25x and the whole map by 12.7x, which corrupts
	// the heap and crashes much later inside an unrelated blit loop (observed
	// at gamemd+0x94BF5 with an index of 0xFFFF).
	//
	// 0x4A8960 cannot simply be called for us: it also lays the sidebar and the
	// message bar out from the rectangle it is given, which would break the
	// "UI never scales" contract. So the two buffers are built here, at their
	// own size, and the globals are swapped for the duration of the render
	// window exactly like Composite/Tile are.
	constexpr unsigned int TacticalShadowBuffer = 0x887644u;
	constexpr unsigned int TacticalVisibilityBuffer = 0x87E8A4u;
	constexpr unsigned int ShadowBufferCtor = 0x7BC970u;
	constexpr unsigned int ShadowBufferDtor = 0x7BCAE0u;
	constexpr unsigned int VisibilityBufferCtor = 0x410CE0u;
	constexpr unsigned int VisibilityBufferDtor = 0x410E50u;
	constexpr unsigned int BufferObjectSize = 0x30u;
	constexpr unsigned int BufferObjectMagicOffset = 0x24u;
	constexpr int BufferObjectMagic = 0x8000;
	constexpr unsigned int BufferObjectWidthOffset = 0x28u;
	constexpr unsigned int BufferObjectHeightOffset = 0x2Cu;

	// The engine's operator new / operator delete, as used by its own surfaces
	// and by these two buffers.
	constexpr unsigned int EngineAllocate = 0x7C8E17u;
	constexpr unsigned int EngineDeallocate = 0x7C8B3Du;

	using SetViewDimensions_t = void(__thiscall*)(void*, RectangleStruct*);
	using RebuildView_t = void(__thiscall*)(void*);
	using Allocate_t = void* (__cdecl*)(unsigned int);
	using Deallocate_t = void(__cdecl*)(void*);
	// Both constructors take the rectangle by value on the stack.
	using RectBufferCtor_t = void(__thiscall*)(void*, RectangleStruct);
	using BufferDtor_t = void(__thiscall*)(void*);

	void WriteViewBound(const RectangleStruct& rect)
	{
		int* pViewBound = reinterpret_cast<int*>(TacticalViewBound);

		pViewBound[0] = rect.X;
		pViewBound[1] = rect.Y;
		pViewBound[2] = rect.Width;
		pViewBound[3] = rect.Height;
	}

	// Mirrors TacticalClass::SetViewDimensions: view_bound, camera clamp against
	// the new viewport, internal origin and isometric matrix.
	void ApplyTacticalViewport(const RectangleStruct& rect)
	{
		WriteViewBound(rect);

		TacticalClass* pTactical = TacticalClass::Instance;

		if (pTactical)
			reinterpret_cast<SetViewDimensions_t>(TacticalSetViewDimensions)(pTactical, const_cast<RectangleStruct*>(&rect));
	}

	// Rebuilds the derived state (internal view origin + isometric matrix) from
	// view_bound and the current camera centre, without touching the camera.
	void RebuildTacticalView()
	{
		TacticalClass* pTactical = TacticalClass::Instance;

		if (pTactical)
			reinterpret_cast<RebuildView_t>(TacticalRebuildView)(pTactical);
	}

	// The pixel storage of one of the two view-size scratch objects lives in a
	// small buffer object reached through +0x14.
	constexpr unsigned int BufferObjectStorageOffset = 0x14u;

	// The engine's visible-cell queue holds at most 799 entries per pass
	// (0x6DA93E rejects at 0x31F) and is cleared at the end of every pass
	// (0x6D4571). A cell is only drawn when something marked it as changed, so
	// a freshly enlarged viewport stays unpainted - measured "cells total 0" for
	// a whole-map view. The banded fill below pushes the map through the
	// engine's own enqueue entry, a slice per frame, until every cell has been
	// drawn once; afterwards the engine's incremental redraw keeps it current.
	constexpr int VisibleCellCapacity = 799;
	// The list is filled twice per frame: once before the first pass and once
	// after the visibility pass, which clears it. Only the injection after that
	// clear is certainly drawn, but which pass consumes the list is not obvious
	// from the disassembly, so both are fed and the budget is split.
	constexpr int BandCellsPerPass = 200;
	constexpr unsigned int CellPendingFlagOffset = 0x138u;
	constexpr unsigned int CellFrameStampOffset = 0x5Cu;
	constexpr unsigned int CellFlagsOffset = 0x12Cu;

	// TacticalClass::TacticalPos, the internal view origin the renderer subtracts
	// from a projected point to get viewport-relative coordinates.
	constexpr unsigned int TacticalViewOrigin = 0xB0u;

	// TacticalClass::RenderMode (+0x0C of the display object): RegisterCellAsVisible
	// bails out while it is set, and the engine clears it at the top of every
	// Render. Read here only so the diagnostic can prove which guard fired.
	constexpr unsigned int DisplayRenderMode = 0x87F7F4u;
	constexpr unsigned int CurrentFrameIndex = 0xA8ED84u;
	constexpr unsigned int TacticalRedrawFlagOffset = 0xD7Du;

	// Camera centre (+0xD74/+0xD78): a camera move invalidates the whole
	// enlarged frame, so the fill is restarted after one. The engine parks the
	// camera on a clamp boundary in the whole-map view and jitters by a pixel or
	// two, hence the tolerance.
	constexpr unsigned int CameraXOffset = 0xD74u;
	constexpr unsigned int CameraYOffset = 0xD78u;
	constexpr int BandCameraTolerance = 16;

	void DestroyViewBuffer(void* pBuffer, unsigned int dtor)
	{
		if (!pBuffer)
			return;

		reinterpret_cast<BufferDtor_t>(dtor)(pBuffer);
		reinterpret_cast<Deallocate_t>(EngineDeallocate)(pBuffer);
	}

	// Builds one `width * height * 2` byte scratch object the same way the
	// engine's own view routine (0x4A8960) does, then verifies that it really
	// carries the size the renderer is about to use.
	void* CreateViewBuffer(unsigned int ctor, unsigned int dtor, const RectangleStruct& rect)
	{
		void* pMemory = reinterpret_cast<Allocate_t>(EngineAllocate)(BufferObjectSize);

		if (!pMemory)
			return nullptr;

		reinterpret_cast<RectBufferCtor_t>(ctor)(pMemory, rect);

		const unsigned char* pBytes = static_cast<const unsigned char*>(pMemory);
		const int width = *reinterpret_cast<const int*>(pBytes + BufferObjectWidthOffset);
		const int height = *reinterpret_cast<const int*>(pBytes + BufferObjectHeightOffset);
		const void* pStorage = *reinterpret_cast<void* const*>(pBytes + BufferObjectStorageOffset);

		if (width != rect.Width || height != rect.Height || !pStorage)
		{
			Debug::Log("[ViewZoom] view buffer %dx%d came back as %dx%d (storage %p), refusing it\n",
				rect.Width, rect.Height, width, height, pStorage);

			DestroyViewBuffer(pMemory, dtor);

			return nullptr;
		}

		*reinterpret_cast<int*>(static_cast<unsigned char*>(pMemory) + BufferObjectMagicOffset) = BufferObjectMagic;

		return pMemory;
	}

	int Clamp(int value, int low, int high)
	{
		if (value < low)
			return low;

		if (value > high)
			return high;

		return value;
	}

	void CopyRow(unsigned short* pDestination, const unsigned short* pSource, int pixels)
	{
		if (!pDestination || !pSource || pixels <= 0)
			return;

		const int pairs = pixels / 2;
		unsigned int* pDestinationPairs = reinterpret_cast<unsigned int*>(pDestination);
		const unsigned int* pSourcePairs = reinterpret_cast<const unsigned int*>(pSource);

		for (int pair = 0; pair < pairs; ++pair)
			pDestinationPairs[pair] = pSourcePairs[pair];

		if (pixels & 1)
			pDestination[pixels - 1] = pSource[pixels - 1];
	}

	bool SameRect(const RectangleStruct& lhs, const RectangleStruct& rhs)
	{
		return lhs.X == rhs.X && lhs.Y == rhs.Y
			&& lhs.Width == rhs.Width && lhs.Height == rhs.Height;
	}

	bool PointInsideRect(const Point2D& point, const RectangleStruct& rect)
	{
		return point.X >= rect.X && point.Y >= rect.Y
			&& point.X < rect.X + rect.Width
			&& point.Y < rect.Y + rect.Height;
	}

	// ---- diagnostics (ViewZoom.Diagnostics), reported every few seconds ----
	constexpr unsigned int DiagnosticIntervalMs = 5000;
	constexpr int SampleStep = 16;

	unsigned int DiagWideFrames = 0;
	unsigned int DiagWideRoleA = 0;
	unsigned int DiagWideRoleB = 0;
	unsigned int DiagWideRoleLost = 0;
	unsigned int DiagDownscaleOk = 0;
	unsigned int DiagDownscaleFail = 0;
	unsigned int DiagMagnifyOk = 0;
	unsigned int DiagMagnifyFail = 0;
	int DiagSourceNonBlack = -1;
	int DiagSourceSamples = 0;
	int DiagDestNonBlack = -1;
	int DiagDestSamples = 0;
	unsigned int DiagWindowTick = 0;
	unsigned int DiagGeomTick = 0;
	unsigned int DiagSidebarHash = 0;
	unsigned int DiagSidebarChanges = 0;
	unsigned int DiagSidebarShownHash = 0;
	unsigned int DiagSidebarShownChanges = 0;
	unsigned int DiagSidebarHiddenHash = 0;
	unsigned int DiagSidebarHiddenChanges = 0;
	// Sidebar presents handed the native viewport instead of the enlarged one,
	// plus the last enlarged width that was logged (see PresentSidebarNatively).
	unsigned int DiagSidebarNativePresents = 0;
	int DiagSidebarLoggedWidth = 0;

	// Invalidation produced while the enlarged viewport is not installed. The
	// engine hands the renderer native-viewport geometry there, so the fork has
	// to correct it: 0x6D2790 rectangles have to move into the enlarged space,
	// and 0x6DA7D0 has to enqueue cells the native cull would drop. These are
	// the two numbers that tell whether the outer band is being serviced.
	unsigned int DiagDirtySeen = 0;
	unsigned int DiagDirtyCorrected = 0;
	unsigned int DiagDirtySamples = 0;
	// Rectangles registered *inside* the render window: those already describe
	// the enlarged view, so they must not be moved. Their count and first
	// samples tell whether the object/animation erase path is registering at
	// all while zoomed out.
	unsigned int DiagDirtyInWindow = 0;
	unsigned int DiagDirtyInWindowSamples = 0;
	unsigned int DiagCellSeen = 0;
	unsigned int DiagCellCalls = 0;
	unsigned int DiagCellWideOnly = 0;
	unsigned int DiagCellEnqueued = 0;
	unsigned int DiagCellCapped = 0;
	unsigned int DiagCellSamples = 0;

	// Cost of the banded fill: a restart walks the map's whole cell vector, and
	// every pushed cell is drawn by all twelve per-cell passes.
	unsigned int DiagBandRestarts = 0;
	unsigned int DiagBandPushed = 0;

	// Whole-viewport repaints forced by ViewZoom.FullRedrawEvery.
	unsigned int DiagForcedRedraws = 0;
	int FullRedrawCountdown = 0;
	int DiagFullRedrawInterval = 0;

	// Every dirty rectangle the engine registers, including the ones made while
	// rendering natively. Comparing the native rate with the zoomed one is what
	// tells whether the enlarged view suppresses registrations.
	unsigned int DiagDirtyAll = 0;

	// Frame cost while a wide level is live, split by whether the frame was a
	// forced whole-viewport one. Drives the automatic FullRedrawEvery.
	bool FrameTimed = false;
	bool FrameWasForced = false;
	unsigned int FrameStartTick = 0;
	double NormalFrameCost = 0.0;
	double ForcedFrameCost = 0.0;

	void SampleRegion(const unsigned char* pPixels, int pitch, const RectangleStruct& rect,
		int stepX, int stepY, int& nonBlack, int& samples)
	{
		nonBlack = 0;
		samples = 0;

		for (int y = 0; y < rect.Height; y += stepY)
		{
			const unsigned short* pRow = reinterpret_cast<const unsigned short*>(
				pPixels + (rect.Y + y) * pitch + rect.X * 2);

			for (int x = 0; x < rect.Width; x += stepX)
			{
				++samples;

				if (pRow[x] != 0)
					++nonBlack;
			}
		}
	}

	// Geometry self check: the wide render is only correct when view_bound,
	// DSurface::ViewBounds, the camera centre and the tactical's internal view
	// origin all describe the same rectangle.
	bool DiagGeomSample = false;

	// Decides once per interval; every site of that one frame then logs.
	void BeginGeometrySample()
	{
		if (!ViewZoomClass::Diagnostics)
		{
			DiagGeomSample = false;
			return;
		}

		const unsigned int now = GetTickCount();

		if (DiagGeomTick && now - DiagGeomTick < DiagnosticIntervalMs)
		{
			DiagGeomSample = false;
			return;
		}

		DiagGeomTick = now;
		DiagGeomSample = true;
	}

	// Cheap fingerprint of a rectangle, used to tell which surface the engine
	// actually presents while a wide level is active.
	unsigned int FingerprintRect(DSurface* pSurface, const RectangleStruct& rect)
	{
		if (!pSurface || pSurface->GetBytesPerPixel() != RequiredBytesPerPixel)
			return 0;

		const int pitch = pSurface->GetPitch();

		if (pitch <= 0 || rect.Width <= 0 || rect.Height <= 0
			|| rect.X < 0 || rect.Y < 0
			|| rect.X + rect.Width > pSurface->Width
			|| rect.Y + rect.Height > pSurface->Height)
		{
			return 0;
		}

		unsigned char* pPixels = static_cast<unsigned char*>(pSurface->Lock(0, 0));

		if (!pPixels)
			return 0;

		unsigned int hash = 2166136261u;

		for (int y = 0; y < rect.Height; y += 24)
		{
			const unsigned short* pRow = reinterpret_cast<const unsigned short*>(
				pPixels + (rect.Y + y) * pitch + rect.X * 2);

			for (int x = 0; x < rect.Width; x += 24)
			{
				hash ^= pRow[x];
				hash *= 16777619u;
			}
		}

		pSurface->Unlock();

		return hash;
	}

	void LogPresentation(const char* pWhere);

	void LogGeometry(const char* pWhere)
	{
		if (!ViewZoomClass::Diagnostics || !DiagGeomSample)
			return;

		TacticalClass* pTactical = TacticalClass::Instance;

		if (!pTactical)
			return;

		const unsigned char* pBytes = reinterpret_cast<const unsigned char*>(pTactical);
		const int* pViewBound = reinterpret_cast<const int*>(TacticalViewBound);
		const auto& view = DSurface::ViewBounds;

		Debug::Log("[ViewZoom] geom %-7s vb %d,%d,%d,%d | vB %d,%d,%d,%d | cam %d,%d | org %d,%d\n",
			pWhere,
			pViewBound[0], pViewBound[1], pViewBound[2], pViewBound[3],
			view.X, view.Y, view.Width, view.Height,
			*reinterpret_cast<const int*>(pBytes + 0xD74u),
			*reinterpret_cast<const int*>(pBytes + 0xD78u),
			*reinterpret_cast<const int*>(pBytes + 0xB0u),
			*reinterpret_cast<const int*>(pBytes + 0xB4u));

		// Which surface is on screen, and by how much is it offset from the
		// frame we produced? Search a small offset range on every candidate.
		LogPresentation("begin");
	}


	// Measures which surface the engine shows and how far it is from the frame
	// we produced. pReference is the native surface holding our downscaled
	// frame; the candidates are the buffers the engine could be presenting.
	void LogPresentation(const char* pWhere)
	{
		if (!ViewZoomClass::Diagnostics || !DiagGeomSample)
			return;

		DSurface* pReference = DSurface::Composite;

		if (!pReference || pReference->GetBytesPerPixel() != RequiredBytesPerPixel)
			return;

		const auto& view = DSurface::ViewBounds;
		const int sampleStep = 32;
		const int offsetLimit = 600;
		const int offsetStep = 30;

		const int referencePitch = pReference->GetPitch();

		if (referencePitch <= 0)
			return;

		unsigned char* pReferencePixels = static_cast<unsigned char*>(pReference->Lock(0, 0));

		if (!pReferencePixels)
			return;

		struct Candidate
		{
			const char* pName;
			DSurface* pSurface;
		};

		const Candidate candidates[] =
		{
			{ "Primary", DSurface::Primary },
			{ "Hidden", DSurface::Hidden },
			{ "Alternate", DSurface::Alternate },
			{ "Tile", DSurface::Tile },
		};

		for (const auto& candidate : candidates)
		{
			DSurface* pSurface = candidate.pSurface;

			if (!pSurface || pSurface->GetBytesPerPixel() != RequiredBytesPerPixel)
				continue;

			const int pitch = pSurface->GetPitch();

			if (pitch <= 0 || pSurface->Width < view.Width || pSurface->Height < view.Height)
				continue;

			unsigned char* pPixels = static_cast<unsigned char*>(pSurface->Lock(0, 0));

			if (!pPixels)
				continue;

			long long bestError = -1;
			int bestX = 0;
			int bestY = 0;

			for (int offsetY = -offsetLimit; offsetY <= offsetLimit; offsetY += offsetStep)
			{
				for (int offsetX = -offsetLimit; offsetX <= offsetLimit; offsetX += offsetStep)
				{
					long long error = 0;
					int samples = 0;

					for (int y = 0; y < view.Height; y += sampleStep)
					{
						const int sourceY = y + offsetY;

						if (sourceY < 0 || sourceY >= pSurface->Height)
							continue;

						const unsigned short* pSurfaceRow = reinterpret_cast<const unsigned short*>(
							pPixels + sourceY * pitch);
						const unsigned short* pReferenceRow = reinterpret_cast<const unsigned short*>(
							pReferencePixels + y * referencePitch);

						for (int x = 0; x < view.Width; x += sampleStep)
						{
							const int sourceX = x + offsetX;

							if (sourceX < 0 || sourceX >= pSurface->Width)
								continue;

							const int difference = static_cast<int>(pSurfaceRow[sourceX])
								- static_cast<int>(pReferenceRow[x]);
							error += difference < 0 ? -difference : difference;
							++samples;
						}
					}

					if (samples < 200)
						continue;

					const long long average = error / samples;

					if (bestError < 0 || average < bestError)
					{
						bestError = average;
						bestX = offsetX;
						bestY = offsetY;
					}
				}
			}

			pSurface->Unlock();

			Debug::Log("[ViewZoom] present %-7s %-9s %dx%d best %d,%d meanerr %.1f/255\n",
				pWhere, candidate.pName, pSurface->Width, pSurface->Height,
				bestX, bestY, static_cast<double>(bestError < 0 ? 0 : bestError) / 257.0);
		}

		pReference->Unlock();
	}

	void ReportDiagnostics()
	{
		const unsigned int now = GetTickCount();

		if (!DiagWindowTick)
			DiagWindowTick = now;

		if (now - DiagWindowTick < DiagnosticIntervalMs)
			return;

		DiagWindowTick = now;

		Debug::Log("[ViewZoom] diag: sidebar surface %u, hidden region %u, screen region %u changes in %.1fs | wide frames %u (roleA %u roleB %u lost %u), downscale ok %u fail %u, magnify ok %u fail %u, sidebar native %u\n",
			DiagSidebarChanges, DiagSidebarHiddenChanges, DiagSidebarShownChanges,
			DiagnosticIntervalMs / 1000.0,
			DiagWideFrames, DiagWideRoleA, DiagWideRoleB, DiagWideRoleLost,
			DiagDownscaleOk, DiagDownscaleFail, DiagMagnifyOk, DiagMagnifyFail,
			DiagSidebarNativePresents);

		if (DiagSourceSamples > 0)
		{
			Debug::Log("[ViewZoom] diag: last frame source non-black %d/%d, destination non-black %d/%d\n",
				DiagSourceNonBlack, DiagSourceSamples, DiagDestNonBlack, DiagDestSamples);
		}

		// The two invalidation paths. A wide-only enqueue is one the engine's
		// own cull would have dropped while zoomed out, i.e. exactly the bug
		// this counter exists to prove or disprove.
		Debug::Log("[ViewZoom] diag: dirty rects all %u | wide: logic %u (moved %u), in-window %u | visible-cell calls %u (candidates %u, wide-only %u, enqueued %u, left to the cap %u) | band fill restarts %u, cells pushed %u | forced full redraws %u (interval %d)\n",
			DiagDirtyAll,
			DiagDirtySeen, DiagDirtyCorrected, DiagDirtyInWindow,
			DiagCellCalls, DiagCellSeen, DiagCellWideOnly, DiagCellEnqueued, DiagCellCapped,
			DiagBandRestarts, DiagBandPushed, DiagForcedRedraws, DiagFullRedrawInterval);

		DiagDirtyAll = 0;
		DiagForcedRedraws = 0;
		DiagBandRestarts = 0;
		DiagBandPushed = 0;
		DiagDirtyInWindow = 0;
		DiagDirtyInWindowSamples = 0;

		DiagDirtySeen = 0;
		DiagDirtyCorrected = 0;
		DiagDirtySamples = 0;
		DiagCellSeen = 0;
		DiagCellCalls = 0;
		DiagCellWideOnly = 0;
		DiagCellEnqueued = 0;
		DiagCellCapped = 0;
		DiagCellSamples = 0;

		DiagWideFrames = 0;
		DiagWideRoleA = 0;
		DiagWideRoleB = 0;
		DiagWideRoleLost = 0;
		DiagDownscaleOk = 0;
		DiagDownscaleFail = 0;
		DiagMagnifyOk = 0;
		DiagMagnifyFail = 0;
		DiagSidebarChanges = 0;
		DiagSidebarShownChanges = 0;
		DiagSidebarHiddenChanges = 0;
		DiagSidebarNativePresents = 0;
	}
}

bool ViewZoomClass::Enabled = true;
bool ViewZoomClass::MiddleClickReset = true;
bool ViewZoomClass::ShowHint = true;
bool ViewZoomClass::WideView = false;
bool ViewZoomClass::WholeMap = true;
bool ViewZoomClass::DebugFill = false;
bool ViewZoomClass::Diagnostics = false;
bool ViewZoomClass::NoBandFill = false;
bool ViewZoomClass::SidebarNativeView = true;
bool ViewZoomClass::TranslateDirtyAreas = true;
// The wide-aware enqueue looked like the fix for the outer band, but in game it
// makes things worse: the enlarged view has far more candidate cells than the
// engine would ever queue, so the extra inserts fill the 799 entry list and the
// engine's own registrations -- the ones that erase objects and animations --
// are dropped by the cap (which also clears their pending flag). Measured
// 2026-10-04: with it on the animation residue is dense, with it off only the
// current frame is left. The dirty area translation alone keeps ore and cells
// current, so this stays off by default and is kept only as an A/B switch.
bool ViewZoomClass::WideCellEnqueue = false;
bool ViewZoomClass::BandFillOnScroll = false;
int ViewZoomClass::FullRedrawEvery = 0;

int ViewZoomClass::LevelCount = DefaultLevelCount;
int ViewZoomClass::CurrentLevel = 4;
double ViewZoomClass::Levels[ViewZoomClass::MaxLevels] = { 0.25, 0.33, 0.5, 0.67, 1.0, 1.25, 1.5 };
bool ViewZoomClass::WholeMapLevels[ViewZoomClass::MaxLevels] = {};

bool ViewZoomClass::Active = false;
bool ViewZoomClass::VisualReady = false;
bool ViewZoomClass::NativeFrameValid = false;
RectangleStruct ViewZoomClass::SourceRect { 0, 0, 0, 0 };
RectangleStruct ViewZoomClass::NativeView { 0, 0, 0, 0 };
unsigned short* ViewZoomClass::Scratch = nullptr;
unsigned int ViewZoomClass::ScratchPixels = 0;

bool ViewZoomClass::WideReady = false;
DSurface* ViewZoomClass::pBigComposite = nullptr;
DSurface* ViewZoomClass::pBigTile = nullptr;
int ViewZoomClass::BigWidth = 0;
int ViewZoomClass::BigHeight = 0;
RectangleStruct ViewZoomClass::WideDestRect { 0, 0, 0, 0 };

Point2D ViewZoomClass::AdjustedInput { 0, 0 };
Point2D ViewZoomClass::AdjustedDragStart { 0, 0 };
Point2D ViewZoomClass::AdjustedDragEnd { 0, 0 };

bool ViewZoomClass::MiddleButtonDown = false;
unsigned int ViewZoomClass::MiddleButtonDownPoint = 0;

RectangleStruct ViewZoomClass::NativeViewSize { 0, 0, 0, 0 };

DSurface* ViewZoomClass::pSavedComposite = nullptr;
DSurface* ViewZoomClass::pSavedTile = nullptr;
DSurface* ViewZoomClass::pSavedTemp = nullptr;
RectangleStruct ViewZoomClass::SavedViewBounds { 0, 0, 0, 0 };
int ViewZoomClass::SavedViewBound[4] = { 0, 0, 0, 0 };
bool ViewZoomClass::WideInstalled = false;
bool ViewZoomClass::WideLevelLive = false;
bool ViewZoomClass::SidebarViewActive = false;
RectangleStruct ViewZoomClass::SidebarViewSaved { 0, 0, 0, 0 };
bool ViewZoomClass::UiStepActive = false;
DSurface* ViewZoomClass::UiStepSavedComposite = nullptr;
DSurface* ViewZoomClass::UiStepSavedTile = nullptr;
DSurface* ViewZoomClass::UiStepSavedTemp = nullptr;
RectangleStruct ViewZoomClass::UiStepSavedViewBounds { 0, 0, 0, 0 };
int ViewZoomClass::UiStepSavedViewBound[4] = { 0, 0, 0, 0 };

void* ViewZoomClass::pBigShadowBuffer = nullptr;
void* ViewZoomClass::pBigVisibilityBuffer = nullptr;
void* ViewZoomClass::pSavedShadowBuffer = nullptr;
void* ViewZoomClass::pSavedVisibilityBuffer = nullptr;
int ViewZoomClass::BigBufferWidth = 0;
int ViewZoomClass::BigBufferHeight = 0;
bool ViewZoomClass::BuffersInstalled = false;

bool ViewZoomClass::InputViewportInstalled = false;
int ViewZoomClass::SavedInputViewBound[4] = { 0, 0, 0, 0 };
int ViewZoomClass::SavedInputTacticalPos[2] = { 0, 0 };
Point2D ViewZoomClass::LastTraceInput { 0, 0 };
Point2D ViewZoomClass::LastTraceOutput { 0, 0 };
double ViewZoomClass::LastTraceFactor = 1.0;
int ViewZoomClass::TracePreviousOrigin[2] = { 0, 0 };
int ViewZoomClass::TraceCamera[2] = { 0, 0 };
int ViewZoomClass::InputTraceCount = 0;

bool ViewZoomClass::BandFillActive = false;
int ViewZoomClass::BandCursorIndex = 0;
int ViewZoomClass::BandFrames = 0;
int ViewZoomClass::BandPushed = 0;
int ViewZoomClass::BandLastCameraX = 0;
int ViewZoomClass::BandLastCameraY = 0;

void ViewZoomClass::LoadFromINI(CCINIClass* pINI, const char* pSection)
{
	if (!pINI || !pSection)
		return;

	Enabled = pINI->ReadBool(pSection, "ViewZoom.Enable", true);
	MiddleClickReset = pINI->ReadBool(pSection, "ViewZoom.MiddleClickReset", true);
	ShowHint = pINI->ReadBool(pSection, "ViewZoom.ShowHint", true);
	WideView = pINI->ReadBool(pSection, "ViewZoom.WideView", false);
	WholeMap = pINI->ReadBool(pSection, "ViewZoom.WholeMap", true);
	DebugFill = pINI->ReadBool(pSection, "ViewZoom.DebugFill", false);
	Diagnostics = pINI->ReadBool(pSection, "ViewZoom.Diagnostics", false);
	NoBandFill = pINI->ReadBool(pSection, "ViewZoom.NoBandFill", false);
	SidebarNativeView = pINI->ReadBool(pSection, "ViewZoom.SidebarNativeView", true);
	TranslateDirtyAreas = pINI->ReadBool(pSection, "ViewZoom.TranslateDirtyAreas", true);
	WideCellEnqueue = pINI->ReadBool(pSection, "ViewZoom.WideCellEnqueue", false);
	BandFillOnScroll = pINI->ReadBool(pSection, "ViewZoom.BandFillOnScroll", false);
	FullRedrawEvery = pINI->ReadInteger(pSection, "ViewZoom.FullRedrawEvery", 8);
	LevelCount = 0;

	for (int i = 0; i < MaxLevels; ++i)
		WholeMapLevels[i] = false;

	bool hasWholeMapLevel = false;

	if (pINI->ReadString(pSection, "ViewZoom.Levels", "", Phobos::readBuffer) > 0)
	{
		char* context = nullptr;

		for (char* cur = strtok_s(Phobos::readBuffer, Phobos::readDelims, &context);
			cur && LevelCount < MaxLevels;
			cur = strtok_s(nullptr, Phobos::readDelims, &context))
		{
			// "full" (also whole/wholemap/all) is the runtime whole-map level;
			// it has no fixed factor, so it is kept as a sentinel.
			if (WholeMap && !hasWholeMapLevel && IsWholeMapToken(cur))
			{
				Levels[LevelCount] = WholeMapFactor;
				WholeMapLevels[LevelCount] = true;
				hasWholeMapLevel = true;
				++LevelCount;
				continue;
			}

			const double factor = std::atof(cur);

			if (factor > 0.0)
			{
				Levels[LevelCount] = factor;
				WholeMapLevels[LevelCount] = false;
				++LevelCount;
			}
			else
			{
				Debug::Log("[ViewZoom] ignoring invalid level '%s'\n", cur);
			}
		}
	}

	int numericCount = 0;

	for (int i = 0; i < LevelCount; ++i)
	{
		if (!WholeMapLevels[i])
			++numericCount;
	}

	// A single level makes stepping meaningless, and an empty table means the
	// ini is broken: fall back to the measured defaults either way.
	if (numericCount < 2)
	{
		LevelCount = DefaultLevelCount;
		hasWholeMapLevel = false;

		for (int i = 0; i < LevelCount; ++i)
		{
			Levels[i] = DefaultLevels[i];
			WholeMapLevels[i] = false;
		}
	}

	// Keep the numeric levels ascending so "zoom in" is always the lower index;
	// the whole-map level is dynamic and always sits past the numeric ones.
	double numeric[MaxLevels];
	int sortedCount = 0;

	for (int i = 0; i < LevelCount; ++i)
	{
		if (!WholeMapLevels[i])
			numeric[sortedCount++] = Levels[i];
	}

	for (int i = 1; i < sortedCount; ++i)
	{
		const double key = numeric[i];
		int j = i - 1;

		while (j >= 0 && numeric[j] > key)
		{
			numeric[j + 1] = numeric[j];
			--j;
		}

		numeric[j + 1] = key;
	}

	LevelCount = 0;

	for (int i = 0; i < sortedCount; ++i)
	{
		Levels[LevelCount] = numeric[i];
		WholeMapLevels[LevelCount] = false;
		++LevelCount;
	}

	// Opt-out rather than opt-in: the whole-map level is appended unless the
	// ini asked for one explicitly or turned the feature off.
	if (WholeMap && !hasWholeMapLevel && LevelCount < MaxLevels)
	{
		Levels[LevelCount] = WholeMapFactor;
		WholeMapLevels[LevelCount] = true;
		++LevelCount;
	}

	CurrentLevel = GetNativeLevel();

	// A reload changes the geometry the surfaces were built for.
	Active = false;
	VisualReady = false;
	NativeFrameValid = false;
	SourceRect = { 0, 0, 0, 0 };
	WideReady = false;
	WideLevelLive = false;
	BandFillActive = false;

	if (!WideInstalled)
		ReleaseBigSurfaces();

	Debug::Log("[ViewZoom] enabled %d, middle click reset %d, hint %d, wide %d, whole map %d, debugfill %d, diagnostics %d, %d levels\n",
		Enabled, MiddleClickReset, ShowHint, WideView, WholeMap, DebugFill, Diagnostics, LevelCount);

	// Whole-viewport repaints forced every N frames. 8 is the value that
	// measured well in game at k = 1.25..2.5 (animation residue reduced to a
	// momentary flicker while the frame rate stays acceptable), so it is the
	// default; 0 measures the cost itself and adapts, < 0 disables the whole
	// mechanism.
	Debug::Log("[ViewZoom] translate dirty areas %d, wide cell enqueue %d, band fill on scroll %d, full redraw every %d (0=auto), sidebar native view %d, band fill %d\n",
		TranslateDirtyAreas, WideCellEnqueue, BandFillOnScroll, FullRedrawEvery, SidebarNativeView, !NoBandFill);

	for (int i = 0; i < LevelCount; ++i)
	{
		if (WholeMapLevels[i])
			Debug::Log("[ViewZoom] level %d: widest (map covers screen)%s\n", i + 1, i == CurrentLevel ? " (native)" : "");
		else
			Debug::Log("[ViewZoom] level %d: k=%.3f%s\n", i + 1, Levels[i], i == CurrentLevel ? " (native)" : "");
	}
}

double ViewZoomClass::GetFactor()
{
	if (LevelCount <= 0 || CurrentLevel < 0 || CurrentLevel >= LevelCount)
		return NativeFactor;

	if (WholeMapLevels[CurrentLevel])
	{
		// The whole-map level has no fixed factor: it depends on the loaded
		// map and on the screen layout. The value is used for comparisons and
		// for reporting only, the viewport itself comes from
		// GetRenderViewport. Inside the render window the live view is already
		// enlarged, so the native size sampled at frame start is the only
		// stable base here.
		RectangleStruct bounds;

		if (GetMapPixelBounds(bounds))
		{
			const RectangleStruct& base = NativeViewSize.Width > 0
				? NativeViewSize : DSurface::ViewBounds;

			if (base.Width > 0 && base.Height > 0)
			{
				// The widest level stops where the map's own bounding box no
				// longer covers the map area: beyond that the corners of the
				// screen would show terrain that does not exist. That is the
				// smaller ratio of the two, and it shrinks with small maps.
				const double byWidth = static_cast<double>(bounds.Width) / base.Width;
				const double byHeight = static_cast<double>(bounds.Height) / base.Height;

				return byWidth < byHeight ? byWidth : byHeight;
			}
		}

		return NativeFactor;
	}

	return Levels[CurrentLevel];
}

bool ViewZoomClass::IsWholeMapLevel(int level)
{
	return WholeMap && level >= 0 && level < LevelCount && WholeMapLevels[level];
}

bool ViewZoomClass::IsWideLevel(int level)
{
	if (level < 0 || level >= LevelCount)
		return false;

	return WholeMapLevels[level] || Levels[level] > NativeFactor;
}

bool ViewZoomClass::GetMapPixelBounds(RectangleStruct& bounds)
{
	int cellsX = MapClass::Instance.MapRect.Width;
	int cellsY = MapClass::Instance.MapRect.Height;

	if (cellsX <= 0 || cellsY <= 0)
	{
		cellsX = MapClass::Instance.MapCoordBounds.Right + 1;
		cellsY = MapClass::Instance.MapCoordBounds.Bottom + 1;
	}

	if (cellsX <= 0 || cellsY <= 0)
		return false;

	bounds.X = 0;
	bounds.Y = 0;
	bounds.Width = cellsX * Unsorted::CellWidthInPixels;
	bounds.Height = cellsY * Unsorted::CellHeightInPixels;

	return true;
}

int ViewZoomClass::GetNativeLevel()
{
	int best = -1;
	double bestDelta = 0.0;

	for (int i = 0; i < LevelCount; ++i)
	{
		// The whole-map level is dynamic and can never be the native view.
		if (WholeMapLevels[i])
			continue;

		const double delta = std::abs(Levels[i] - NativeFactor);

		if (best < 0 || delta < bestDelta)
		{
			bestDelta = delta;
			best = i;
		}
	}

	return best < 0 ? 0 : best;
}

bool ViewZoomClass::IsZoomed()
{
	if (!Enabled)
		return false;

	const double factor = GetFactor();

	if (factor < NativeFactor)
		return Active;

	if (factor > NativeFactor)
		return WideView && WideReady;

	return false;
}

double ViewZoomClass::GetViewScale()
{
	const auto& view = DSurface::ViewBounds;

	if (!Enabled || view.Width <= 0)
		return NativeFactor;

	const double factor = GetFactor();

	if (factor < NativeFactor)
	{
		RectangleStruct source;

		if (CalculateSourceRect(view, source))
			return static_cast<double>(source.Width) / view.Width;

		return NativeFactor;
	}

	if (factor > NativeFactor && WideView && IsWideLevel(CurrentLevel))
	{
		// The wider view is offset by the ratio between the rendered frame and
		// the rectangle it is presented in. For the plain k > 1 levels that is
		// the whole native view and equals k; the whole-map level is letterboxed
		// and has to use its fitted rectangle instead.
		if (WholeMapLevels[CurrentLevel] && WideDestRect.Width > 0 && BigWidth > 0)
			return static_cast<double>(BigWidth) / WideDestRect.Width;

		return static_cast<double>(static_cast<int>(view.Width * factor + 0.5)) / view.Width;
	}

	return NativeFactor;
}

bool ViewZoomClass::GetRenderViewport(const RectangleStruct& nativeView, RectangleStruct& viewport)
{
	if (nativeView.Width < 2 || nativeView.Height < 2)
		return false;

	const double factor = GetFactor();
	const int width = static_cast<int>(nativeView.Width * factor + 0.5);
	const int height = static_cast<int>(nativeView.Height * factor + 0.5);

	if (width < 2 || height < 2)
		return false;

	viewport.X = 0;
	viewport.Y = 0;
	viewport.Width = width;
	viewport.Height = height;

	return true;
}

bool ViewZoomClass::CalculateWideDestination(const RectangleStruct& view, int width, int height,
	RectangleStruct& dest)
{
	if (view.Width < 2 || view.Height < 2 || width < 2 || height < 2)
		return false;

	const double byWidth = static_cast<double>(view.Width) / width;
	const double byHeight = static_cast<double>(view.Height) / height;
	const double scale = byWidth < byHeight ? byWidth : byHeight;

	int destWidth = static_cast<int>(width * scale + 0.5);
	int destHeight = static_cast<int>(height * scale + 0.5);

	destWidth = Clamp(destWidth, 1, view.Width);
	destHeight = Clamp(destHeight, 1, view.Height);

	dest.X = view.X + (view.Width - destWidth) / 2;
	dest.Y = view.Y + (view.Height - destHeight) / 2;
	dest.Width = destWidth;
	dest.Height = destHeight;

	return true;
}

bool ViewZoomClass::Step(int direction)
{
	if (!Enabled || direction == 0 || LevelCount < 2)
		return false;

	const int next = CurrentLevel + (direction < 0 ? -1 : 1);

	if (next < 0 || next >= LevelCount)
	{
		if (ShowHint)
		{
			Debug::Log("[ViewZoom] %s limit reached, k=%.3f\n",
				direction < 0 ? "zoom in" : "zoom out", GetFactor());
		}

		return false;
	}

	if (IsWideLevel(next) && !WideView)
	{
		// A wider view needs the enlarged render pair; refuse the level instead
		// of leaving the screen and the state out of sync.
		if (WholeMapLevels[next])
			Debug::Log("[ViewZoom] the whole map level needs ViewZoom.WideView=true\n");
		else
			Debug::Log("[ViewZoom] zoom out to k=%.3f needs ViewZoom.WideView=true\n", Levels[next]);

		return false;
	}

	CurrentLevel = next;
	OnLevelChanged();
	ReportLevel(direction < 0 ? "zoom in" : "zoom out");

	return true;
}

bool ViewZoomClass::ResetToNative(const char* pAction)
{
	if (!Enabled || LevelCount <= 0)
		return false;

	if (CurrentLevel != GetNativeLevel())
	{
		CurrentLevel = GetNativeLevel();
		OnLevelChanged();
	}

	ReportLevel(pAction);

	return true;
}

// Called from the two entry points of a new match: ScenarioClass::StartScenario
// (0x683AB0) and ScenarioClass::LoadGame (0x67E440). Both are shared by
// skirmish, campaign missions, save loading and the CnCNet -SPAWN flow --
// cncnet5.dll calls those two functions directly (0x6BBC2D69 / 0x6BBC2D8A /
// 0x6BBC2DA5), which is why they are the only points that cover every path.
//
// Order matters: the level goes back to native *before* OnLevelChanged() runs, so
// the wide branch (RestartBandFill, which reads MapClass::Instance.Cells) cannot
// be taken while the new map is still being loaded.
void ViewZoomClass::ResetForNewMatch(const char* pAction)
{
	// Defensive: a match can only start with the engine's own surfaces in place.
	if (WideInstalled)
		EndTacticalRender();

	// A click of the previous match must not leak a half-installed viewport.
	if (InputViewportInstalled)
		EndInputViewport();

	MiddleButtonDown = false;
	MiddleButtonDownPoint = 0;

	const int previous = CurrentLevel;

	CurrentLevel = GetNativeLevel();
	OnLevelChanged();

	// Always logged, even with ShowHint off: this line is how the next session
	// tells which entry point a match (in particular -SPAWN) actually took.
	Debug::Log("[ViewZoom] new match (%s): level %d -> native %d\n",
		pAction, previous + 1, CurrentLevel + 1);

	if (ShowHint)
		ReportLevel(pAction);
}

void ViewZoomClass::OnWindowMessage(unsigned int message, unsigned int packedPoint)
{
	if (message == MiddleButtonDownMessage || message == MiddleButtonDoubleClickMessage)
	{
		MiddleButtonDown = true;
		MiddleButtonDownPoint = packedPoint;
		return;
	}

	if (message != MiddleButtonUpMessage)
		return;

	const bool wasDown = MiddleButtonDown;
	MiddleButtonDown = false;

	if (!wasDown || !MiddleClickReset)
		return;

	const int downX = static_cast<short>(MiddleButtonDownPoint & 0xFFFFu);
	const int downY = static_cast<short>(MiddleButtonDownPoint >> 16);
	const int upX = static_cast<short>(packedPoint & 0xFFFFu);
	const int upY = static_cast<short>(packedPoint >> 16);
	const int dx = upX - downX;
	const int dy = upY - downY;

	// A gesture that moved is a drag, not a click; leave it to the engine.
	if (std::abs(dx) > MiddleButtonDragThreshold || std::abs(dy) > MiddleButtonDragThreshold)
	{
		if (ShowHint)
			Debug::Log("[ViewZoom] middle drag ignored (dx %d, dy %d)\n", dx, dy);

		return;
	}

	ResetToNative("middle click");
}

void ViewZoomClass::ReportLevel(const char* pAction)
{
	if (!ShowHint || LevelCount <= 0)
		return;

	const int nativeLevel = GetNativeLevel();

	if (WholeMapLevels[CurrentLevel])
	{
		RectangleStruct bounds;
		const bool known = GetMapPixelBounds(bounds);

		Debug::Log("[ViewZoom] %s: level %d/%d widest map %dx%d (k=%.3f) (native level %d k=%.3f)\n",
			pAction, CurrentLevel + 1, LevelCount,
			known ? bounds.Width : 0, known ? bounds.Height : 0, GetFactor(),
			nativeLevel + 1, Levels[nativeLevel]);
	}
	else
	{
		Debug::Log("[ViewZoom] %s: level %d/%d k=%.3f (native level %d k=%.3f)\n",
			pAction, CurrentLevel + 1, LevelCount, GetFactor(),
			nativeLevel + 1, Levels[nativeLevel]);
	}
}

void ViewZoomClass::OnLevelChanged()
{
	// The presented rectangle changed: the stashed native frame no longer
	// matches the screen, and the surfaces have to be rebuilt from native
	// pixels once before the engine's incremental redraw can continue.
	NativeFrameValid = false;
	VisualReady = false;
	WideReady = false;
	WideLevelLive = false;

	const double factor = GetFactor();

	if (Enabled && WideView && IsWideLevel(CurrentLevel) && factor > NativeFactor)
	{
		// A new level means a new surface: the current frame has to be painted
		// from scratch again.
		RestartBandFill();
	}
	else
	{
		BandFillActive = false;
	}

	if (!(Enabled && WideView && IsWideLevel(CurrentLevel) && factor > NativeFactor) && !WideInstalled)
		ReleaseBigSurfaces();

	RequestFullRedraw();
}

void ViewZoomClass::RequestFullRedraw()
{
	// DisplayClass::RenderMode (+0x0C): 2 requests a complete tactical pass.
	// Never write 0x884D44 here, that one is MouseClass::MouseCursorIsMini.
	*reinterpret_cast<unsigned int*>(
		reinterpret_cast<unsigned char*>(&DisplayClass::Instance) + 0x0Cu) = 2u;
}

bool ViewZoomClass::CalculateSourceRect(const RectangleStruct& view, RectangleStruct& source)
{
	if (!Enabled || view.Width < 2 || view.Height < 2)
		return false;

	const double factor = GetFactor();

	// k >= 1 is the wider view, handled by the enlarged render pair instead.
	if (factor >= NativeFactor)
		return false;

	const int width = Clamp(static_cast<int>(view.Width * factor + 0.5), 2, view.Width);
	const int height = Clamp(static_cast<int>(view.Height * factor + 0.5), 2, view.Height);

	if (width >= view.Width && height >= view.Height)
		return false;

	// ClampPosition drives the centre of this smaller viewport, so centring the
	// crop inside the full native render keeps the camera centre identical and
	// all four map edges line up exactly.
	source.X = view.X + (view.Width - width) / 2;
	source.Y = view.Y + (view.Height - height) / 2;
	source.Width = width;
	source.Height = height;

	return true;
}

void ViewZoomClass::SynchronizeState()
{
	const auto& view = DSurface::ViewBounds;
	RectangleStruct source;

	const bool shouldApply = CalculateSourceRect(view, source);

	if (shouldApply == Active)
		return;

	Active = shouldApply;
	VisualReady = false;
	NativeFrameValid = false;
	SourceRect = { 0, 0, 0, 0 };
}

// ---------------------------------------------------------------------------
// k > 1: the enlarged render pair
// ---------------------------------------------------------------------------

DSurface* ViewZoomClass::CreateSurface(int width, int height)
{
	void* pMemory = YRMemory::Allocate(DSurfaceSize);

	if (!pMemory)
		return nullptr;

	// DSurface::DSurface(int width, int height, bool zBuffer, bool offscreen),
	// exactly the arguments the engine's own surface factory uses.
	using Ctor_t = void* (__thiscall*)(void*, int, int, int, int);
	reinterpret_cast<Ctor_t>(DSurfaceCtor)(pMemory, width, height, 1, 1);

	DSurface* pSurface = static_cast<DSurface*>(pMemory);

	// The whole-map level asks for very large surfaces; when the engine cannot
	// back one with pixels the depth probe is the cheapest tell, and touching
	// the pixels before that check would be a hard crash.
	if (pSurface->GetBytesPerPixel() != RequiredBytesPerPixel)
	{
		Debug::Log("[ViewZoom] surface %dx%d has no %d bpp pixels, refusing it\n",
			width, height, RequiredBytesPerPixel);

		DestroySurface(pSurface);

		return nullptr;
	}

	pSurface->Fill(0);

	return pSurface;
}

void ViewZoomClass::DestroySurface(DSurface* pSurface)
{
	if (!pSurface)
		return;

	// The engine destroys a surface with `mov edx,[ecx] / push 1 / call [edx]`.
	using Destroy_t = void(__thiscall*)(void*, int);
	void** pVtable = *reinterpret_cast<void***>(pSurface);
	reinterpret_cast<Destroy_t>(pVtable[0])(pSurface, 1);
}

void ViewZoomClass::ReleaseBigSurfaces()
{
	if (WideInstalled)
		return;

	DestroySurface(pBigComposite);
	DestroySurface(pBigTile);
	pBigComposite = nullptr;
	pBigTile = nullptr;
	BigWidth = 0;
	BigHeight = 0;
	WideDestRect = { 0, 0, 0, 0 };
	WideReady = false;
	WideLevelLive = false;
	BandFillActive = false;

	ReleaseWideViewBuffers();
}

bool ViewZoomClass::EnsureBigSurfaces(int width, int height)
{
	if (pBigComposite && pBigTile && width == BigWidth && height == BigHeight)
		return true;

	ReleaseBigSurfaces();

	DSurface* pFirst = CreateSurface(width, height);
	DSurface* pSecond = pFirst ? CreateSurface(width, height) : nullptr;

	if (!pFirst || !pSecond)
	{
		DestroySurface(pFirst);
		DestroySurface(pSecond);

		Debug::Log("[ViewZoom] could not create the %dx%d render pair, wider view stays off\n", width, height);

		return false;
	}

	pBigComposite = pFirst;
	pBigTile = pSecond;
	BigWidth = width;
	BigHeight = height;
	WideReady = false;

	const double megabytes = static_cast<double>(pFirst->GetPitch()) * height / (1024.0 * 1024.0);

	Debug::Log("[ViewZoom] created %dx%d render pair %p / %p (%.1f MB each, %.1f MB total)\n",
		width, height, static_cast<void*>(pFirst), static_cast<void*>(pSecond),
		megabytes, megabytes * 2.0);

	return true;
}

// The two view-size engine scratch buffers (0x887644 / 0x87E8A4). They are
// rebuilt at the enlarged size here, and swapped in only for the render window,
// so the renderer never sees a buffer smaller than the viewport it draws.
bool ViewZoomClass::EnsureWideViewBuffers(const RectangleStruct& rect)
{
	if (pBigShadowBuffer && pBigVisibilityBuffer
		&& rect.Width == BigBufferWidth && rect.Height == BigBufferHeight)
	{
		return true;
	}

	if (BuffersInstalled)
		return false;

	ReleaseWideViewBuffers();

	pBigShadowBuffer = CreateViewBuffer(ShadowBufferCtor, ShadowBufferDtor, rect);
	pBigVisibilityBuffer = pBigShadowBuffer
		? CreateViewBuffer(VisibilityBufferCtor, VisibilityBufferDtor, rect)
		: nullptr;

	if (!pBigShadowBuffer || !pBigVisibilityBuffer)
	{
		ReleaseWideViewBuffers();

		Debug::Log("[ViewZoom] could not create the %dx%d view buffers, staying native\n",
			rect.Width, rect.Height);

		return false;
	}

	BigBufferWidth = rect.Width;
	BigBufferHeight = rect.Height;

	const double megabytes = static_cast<double>(rect.Width) * rect.Height * 2.0 / (1024.0 * 1024.0);

	Debug::Log("[ViewZoom] created %dx%d view buffers %p / %p (%.1f MB each, %.1f MB total)\n",
		rect.Width, rect.Height, pBigShadowBuffer, pBigVisibilityBuffer, megabytes, megabytes * 2.0);

	return true;
}

void ViewZoomClass::ReleaseWideViewBuffers()
{
	if (BuffersInstalled)
		return;

	DestroyViewBuffer(pBigShadowBuffer, ShadowBufferDtor);
	DestroyViewBuffer(pBigVisibilityBuffer, VisibilityBufferDtor);
	pBigShadowBuffer = nullptr;
	pBigVisibilityBuffer = nullptr;
	BigBufferWidth = 0;
	BigBufferHeight = 0;
}

bool ViewZoomClass::InstallWideViewBuffers()
{
	if (BuffersInstalled || !pBigShadowBuffer || !pBigVisibilityBuffer)
		return false;

	pSavedShadowBuffer = *reinterpret_cast<void**>(TacticalShadowBuffer);
	pSavedVisibilityBuffer = *reinterpret_cast<void**>(TacticalVisibilityBuffer);

	*reinterpret_cast<void**>(TacticalShadowBuffer) = pBigShadowBuffer;
	*reinterpret_cast<void**>(TacticalVisibilityBuffer) = pBigVisibilityBuffer;

	BuffersInstalled = true;

	return true;
}

void ViewZoomClass::RestoreWideViewBuffers()
{
	if (!BuffersInstalled)
		return;

	*reinterpret_cast<void**>(TacticalShadowBuffer) = pSavedShadowBuffer;
	*reinterpret_cast<void**>(TacticalVisibilityBuffer) = pSavedVisibilityBuffer;

	pSavedShadowBuffer = nullptr;
	pSavedVisibilityBuffer = nullptr;
	BuffersInstalled = false;
}

// ---------------------------------------------------------------------------
// banded full draw of the enlarged viewport
// ---------------------------------------------------------------------------

void ViewZoomClass::RestartBandFill()
{
	BandFillActive = true;
	BandFrames = 0;
	BandPushed = 0;
	BandCursorIndex = 0;
	++DiagBandRestarts;

	if (ShowHint)
	{
		const auto& mapRect = MapClass::Instance.MapRect;
		const auto& coordBounds = MapClass::Instance.MapCoordBounds;
		const auto& cells = MapClass::Instance.Cells;
		const auto& view = DSurface::ViewBounds;

		// The cell array is not a dense rectangle: sweeping MapRect's cell
		// coordinates found only 13346 of 26400 slots, so the fill walks the
		// engine's own cell vector instead and takes every non-null entry.
		Debug::Log("[ViewZoom] band fill start: maprect %d,%d %dx%d, coords %d,%d..%d,%d, cells %d, view %dx%d\n",
			mapRect.X, mapRect.Y, mapRect.Width, mapRect.Height,
			coordBounds.Left, coordBounds.Top, coordBounds.Right, coordBounds.Bottom,
			cells.Capacity, view.Width, view.Height);
	}
}

void ViewZoomClass::TraceInput(const char* pTag, const Point2D& input, const Point2D& output, bool applied)
{
	// Only the zoomed levels are interesting, and the engine converts the same
	// click many times per frame, so identical repeats are dropped and the
	// budget is spent on actual changes only. Every zoomed level is traced: the
	// sidebar click regression (super weapon cameo firing on the spot) has to be
	// checkable at 1.25x as well, not only at the whole-map level.
	if (!Diagnostics || InputTraceCount >= 200 || !IsZoomed())
		return;

	if (input.X == LastTraceInput.X && input.Y == LastTraceInput.Y
		&& output.X == LastTraceOutput.X && output.Y == LastTraceOutput.Y
		&& GetFactor() == LastTraceFactor)
	{
		return;
	}

	LastTraceInput = input;
	LastTraceOutput = output;
	LastTraceFactor = GetFactor();
	++InputTraceCount;

	const auto& view = DSurface::ViewBounds;

	// The engine's own ProcessClickCoords tolerates a missing TacticalClass
	// (0x692361), and widening the trace gate to every zoomed level made this
	// dereference reachable in more situations than the whole-map level alone,
	// so it must tolerate it too.
	const TacticalClass* pTactical = TacticalClass::Instance;
	const unsigned char* pTacticalBytes = reinterpret_cast<const unsigned char*>(pTactical);
	const int originX = pTactical ? *reinterpret_cast<const int*>(pTacticalBytes + TacticalViewOrigin) : 0;
	const int originY = pTactical ? *reinterpret_cast<const int*>(pTacticalBytes + TacticalViewOrigin + 4u) : 0;

	Debug::Log("[ViewZoom] input %-6s (%d,%d) -> (%d,%d) applied %d | factor %.3f | view %d,%d %dx%d big %dx%d dest %d,%d %dx%d | cam %d,%d org was %d,%d now %d,%d\n",
		pTag, input.X, input.Y, output.X, output.Y, applied,
		GetFactor(),
		view.X, view.Y, view.Width, view.Height,
		BigWidth, BigHeight,
		WideDestRect.X, WideDestRect.Y, WideDestRect.Width, WideDestRect.Height,
		TraceCamera[0], TraceCamera[1],
		TracePreviousOrigin[0], TracePreviousOrigin[1],
		originX, originY);
}

void ViewZoomClass::InjectBandFill(int budget)
{
	// A/B switch: tells whether the banded fill is what keeps the sidebar from
	// being re-presented while a wide level is active.
	if (NoBandFill)
		return;

	AdvanceBandFill(budget);
}

void ViewZoomClass::RefreshBandFillCamera(int cameraX, int cameraY)
{
	// The whole-map viewport is exactly the map's own rectangle, so the engine
	// parks the camera on a clamp boundary and it jitters by a pixel or two
	// every frame. Restarting the fill on any change at all meant it restarted
	// every frame and never painted anything, so only a move big enough to be an
	// actual scroll invalidates the frame.
	const int deltaX = cameraX - BandLastCameraX;
	const int deltaY = cameraY - BandLastCameraY;

	const bool moved = deltaX > BandCameraTolerance || deltaX < -BandCameraTolerance
		|| deltaY > BandCameraTolerance || deltaY < -BandCameraTolerance;

	if (!moved)
		return;

	BandLastCameraX = cameraX;
	BandLastCameraY = cameraY;

	// Repairing on every camera move is what makes a zoomed-out pan stutter:
	// each restart walks the map's whole cell vector again and then pushes up to
	// 512 cells per frame through all twelve per-cell passes, on top of the work
	// the engine already does. A pan therefore restarts the sweep every few
	// frames, which is pure added cost. The sweep's job is only to paint a
	// surface that has never been painted; a level change starts it directly
	// (OnLevelChanged), and after that the incremental paths corrected by
	// TranslateDirtyArea / TryWideCellEnqueue keep the enlarged view current.
	// A/B switch: ViewZoom.BandFillOnScroll=true restores the old behaviour
	// (but still never restarts a sweep that is already running).
	if (!BandFillOnScroll || BandFillActive)
		return;

	RestartBandFill();
}

// Walks the map's cell grid and appends the next slice straight to the engine's
// visible-cell queue - the array the eight per-cell draw layers read. Going
// through RegisterCellAsVisible (0x6DA7D0) was tried first and never enqueued
// anything (it rejected all 26k cells of a whole-map fill across every frame),
// so the queue is written directly here; the engine itself only ever appends a
// cell pointer and bumps the count, and it clears the count at the end of every
// pass, which is what makes this safe.
void ViewZoomClass::AdvanceBandFill(int budget)
{
	if (!BandFillActive)
		return;

	TacticalClass* pTactical = TacticalClass::Instance;

	if (!pTactical)
		return;

	const auto& cells = MapClass::Instance.Cells;
	const int capacity = cells.Capacity;
	CellClass** pItems = cells.Items;

	if (!pItems || capacity <= 0)
	{
		BandFillActive = false;
		return;
	}

	const int queueBefore = pTactical->VisibleCellCount;
	int appended = 0;

	while (appended < budget)
	{
		if (BandCursorIndex >= capacity)
		{
			BandFillActive = false;

			if (ShowHint)
			{
				Debug::Log("[ViewZoom] band fill complete: %d frames, %d cells, queue %d -> %d\n",
					BandFrames, BandPushed, queueBefore, pTactical->VisibleCellCount);
			}

			break;
		}

		CellClass* pCell = pItems[BandCursorIndex];

		++BandCursorIndex;

		// While zoomed out but not to the whole map, only the cells the current
		// viewport can show are worth queueing. Without this a repair sweep after
		// a scroll has to walk the whole map (52k cells, ~130 frames), so the
		// screen stays wrong until it finishes; restricted to the viewport it is
		// a dozen frames. The whole-map level keeps the unfiltered path so the
		// one case that is known to work cannot be broken by a projection
		// mismatch.
		if (pCell && !IsWholeMapLevel(CurrentLevel))
		{
			TacticalClass* pOwner = pTactical;
			const Point2D screen = TacticalClass::CoordsToScreen(pCell->GetCellCoords());
			const int* pOrigin = reinterpret_cast<const int*>(
				reinterpret_cast<const unsigned char*>(pOwner) + TacticalViewOrigin);

			const auto& view = DSurface::ViewBounds;
			const int relativeX = screen.X - pOrigin[0];
			const int relativeY = screen.Y - pOrigin[1];
			const int margin = 240;

			if (relativeX < view.X - margin || relativeX > view.X + view.Width + margin
				|| relativeY < view.Y - margin || relativeY > view.Y + view.Height + margin)
			{
				pCell = nullptr;
			}
		}

		if (pCell)
		{
			if (pTactical->VisibleCellCount >= VisibleCellCapacity)
				break;

			if (BandFrames == 0 && appended == 0)
			{
				// One-shot proof of why 0x6DA7D0 rejected everything.
				Debug::Log("[ViewZoom] band probe: rendermode %u, frame %u, cell stamp %u, flags 0x%X, pending %u\n",
					*reinterpret_cast<const unsigned int*>(DisplayRenderMode),
					*reinterpret_cast<const unsigned int*>(CurrentFrameIndex),
					*reinterpret_cast<const unsigned int*>(
						reinterpret_cast<const unsigned char*>(pCell) + CellFrameStampOffset),
					*reinterpret_cast<const unsigned int*>(
						reinterpret_cast<const unsigned char*>(pCell) + CellFlagsOffset),
					*reinterpret_cast<const unsigned char*>(
						reinterpret_cast<const unsigned char*>(pCell) + CellPendingFlagOffset));
			}

			pTactical->VisibleCells[pTactical->VisibleCellCount] = pCell;
			++pTactical->VisibleCellCount;

			// Same housekeeping the engine's own entry does: consume the cell's
			// pending flag, stamp it with the current frame, and mark the
			// tactical view as redrawn.
			*reinterpret_cast<unsigned char*>(
				reinterpret_cast<unsigned char*>(pCell) + CellPendingFlagOffset) = 0;
			*reinterpret_cast<unsigned int*>(
				reinterpret_cast<unsigned char*>(pCell) + CellFrameStampOffset) =
				*reinterpret_cast<const unsigned int*>(CurrentFrameIndex);
			*reinterpret_cast<unsigned char*>(
				reinterpret_cast<unsigned char*>(pTactical) + TacticalRedrawFlagOffset) = 1;

			++appended;
			++BandPushed;
			++DiagBandPushed;
		}
	}

	if (BandFrames == 0 && pTactical->VisibleCellCount == queueBefore)
	{
		Debug::Log("[ViewZoom] band fill appended %d cells but the queue stayed at %d\n",
			appended, queueBefore);
	}

	++BandFrames;
}

// The engine's visible cell list is capped at 800 entries, so a fresh enlarged
// surface never gets fully redrawn in one pass (a native pass already registers
// ~1044 cells, 1.25x needs ~1630). Seed the new pair with the previous native
// frame scaled up around the view centre so the cells the cap drops show a
// sensible image instead of black.
bool ViewZoomClass::SeedBigSurfaces(DSurface* pSource)
{
	if (!pSource || !pBigComposite || !pBigTile || BigWidth < 2 || BigHeight < 2)
		return false;

	if (pSource->GetBytesPerPixel() != RequiredBytesPerPixel)
		return false;

	const auto& view = SavedViewBounds;

	if (view.Width < 2 || view.Height < 2)
		return false;

	const int sourcePitch = pSource->GetPitch();

	if (sourcePitch <= 0
		|| view.X < 0 || view.Y < 0
		|| view.X + view.Width > pSource->Width
		|| view.Y + view.Height > pSource->Height)
	{
		return false;
	}

	unsigned char* pSourcePixels = static_cast<unsigned char*>(pSource->Lock(0, 0));

	if (!pSourcePixels)
		return false;

	const double centerX = view.X + view.Width * 0.5;
	const double centerY = view.Y + view.Height * 0.5;
	const double bigCenterX = BigWidth * 0.5;
	const double bigCenterY = BigHeight * 0.5;

	// Per axis: the whole-map level renders a 2:1 rectangle into a non 2:1
	// native view, so a single ratio would seed a distorted image.
	const double inverseScaleX = static_cast<double>(view.Width) / BigWidth;
	const double inverseScaleY = static_cast<double>(view.Height) / BigHeight;

	bool seeded = false;

	DSurface* targets[] = { pBigComposite, pBigTile };

	for (DSurface* pTarget : targets)
	{
		if (!pTarget || pTarget->GetBytesPerPixel() != RequiredBytesPerPixel)
			continue;

		const int pitch = pTarget->GetPitch();

		if (pitch <= 0)
			continue;

		unsigned char* pPixels = static_cast<unsigned char*>(pTarget->Lock(0, 0));

		if (!pPixels)
			continue;

		for (int y = 0; y < BigHeight; ++y)
		{
			const int sourceY = static_cast<int>(centerY + (y - bigCenterY) * inverseScaleY + 0.5);

			if (sourceY < view.Y || sourceY >= view.Y + view.Height)
				continue;

			const unsigned short* pSourceRow = reinterpret_cast<const unsigned short*>(
				pSourcePixels + sourceY * sourcePitch);
			unsigned short* pTargetRow = reinterpret_cast<unsigned short*>(
				pPixels + y * pitch);

			for (int x = 0; x < BigWidth; ++x)
			{
				const int sourceX = static_cast<int>(centerX + (x - bigCenterX) * inverseScaleX + 0.5);

				// Anything the map does not cover stays black: a level whose
				// viewport is wider than the map leaves those corners visible
				// and a stale or tinted seed there is what looked wrong.
				pTargetRow[x] = (sourceX >= view.X && sourceX < view.X + view.Width)
					? pSourceRow[sourceX] : 0;
			}
		}

		pTarget->Unlock();
		seeded = true;
	}

	pSource->Unlock();

	if (seeded && ShowHint)
	{
		Debug::Log("[ViewZoom] seeded the %dx%d pair from the native frame\n", BigWidth, BigHeight);
	}

	return seeded;
}

bool ViewZoomClass::FillNativeView(DSurface* pSurface)
{
	if (!pSurface || pSurface->GetBytesPerPixel() != RequiredBytesPerPixel)
		return false;

	const auto& view = SavedViewBounds;
	const int pitch = pSurface->GetPitch();

	if (pitch <= 0 || view.Width <= 0 || view.Height <= 0)
		return false;

	unsigned char* pPixels = static_cast<unsigned char*>(pSurface->Lock(0, 0));

	if (!pPixels)
		return false;

	// Magenta in RGB565, impossible to confuse with the game's palette.
	const unsigned short color = 0xF81Fu;

	for (int y = 0; y < view.Height; ++y)
	{
		unsigned short* pRow = reinterpret_cast<unsigned short*>(
			pPixels + (view.Y + y) * pitch + view.X * 2);

		for (int x = 0; x < view.Width; ++x)
			pRow[x] = color;
	}

	pSurface->Unlock();

	return true;
}

bool ViewZoomClass::DownscaleToNative(DSurface* pSource, DSurface* pDestination)
{
	if (!pSource || !pDestination || BigWidth < 2 || BigHeight < 2)
		return false;

	// `view` here is the destination rectangle: the whole native view for the
	// plain k > 1 levels, or the centred letterboxed fit for the whole-map
	// level (the enlarged frame is then mapped onto this smaller rectangle).
	const auto& view = WideDestRect;

	if (view.Width < 2 || view.Height < 2)
		return false;

	if (pSource->GetBytesPerPixel() != RequiredBytesPerPixel
		|| pDestination->GetBytesPerPixel() != RequiredBytesPerPixel)
	{
		return false;
	}

	const int sourcePitch = pSource->GetPitch();
	const int destPitch = pDestination->GetPitch();

	if (sourcePitch <= 0 || destPitch <= 0)
		return false;

	if (view.X < 0 || view.Y < 0
		|| view.X + view.Width > pDestination->Width
		|| view.Y + view.Height > pDestination->Height
		|| BigWidth > pSource->Width || BigHeight > pSource->Height)
	{
		return false;
	}

	unsigned char* pSourcePixels = static_cast<unsigned char*>(pSource->Lock(0, 0));

	if (!pSourcePixels)
		return false;

	unsigned char* pDestPixels = static_cast<unsigned char*>(pDestination->Lock(0, 0));

	if (!pDestPixels)
	{
		pSource->Unlock();
		return false;
	}

	if (Diagnostics)
	{
		SampleRegion(pSourcePixels, sourcePitch, { 0, 0, BigWidth, BigHeight },
			SampleStep, SampleStep, DiagSourceNonBlack, DiagSourceSamples);
	}

	// Nearest neighbour from the enlarged frame down to the presented rectangle.
	//
	// The accumulator takes one *source* pixel per step and drops a whole
	// *destination* pixel as soon as it has accumulated one, so the source
	// advances by source/destination per destination pixel. Because that ratio is
	// greater than one when shrinking, the subtraction has to run in a `while`
	// loop: with a single `if` the accumulator drifts upward and the source
	// pointer ends up advancing one pixel per pixel, which samples only the top
	// left corner of the enlarged frame. (The magnify path further down may use
	// an `if` because there the source is the smaller one, so the ratio is below
	// one and one subtraction per step is always enough.)
	int sourceY = 0;
	int verticalPhase = 0;

	for (int destY = 0; destY < view.Height; ++destY)
	{
		if (sourceY >= BigHeight)
			sourceY = BigHeight - 1;

		const unsigned short* pSourceRow = reinterpret_cast<const unsigned short*>(
			pSourcePixels + sourceY * sourcePitch);
		unsigned short* pDestinationRow = reinterpret_cast<unsigned short*>(
			pDestPixels + (view.Y + destY) * destPitch + view.X * 2);
		int sourceX = 0;
		int horizontalPhase = 0;

		for (int destX = 0; destX < view.Width; ++destX)
		{
			if (sourceX >= BigWidth)
				sourceX = BigWidth - 1;

			pDestinationRow[destX] = pSourceRow[sourceX];

			horizontalPhase += BigWidth;

			while (horizontalPhase >= view.Width)
			{
				horizontalPhase -= view.Width;
				++sourceX;
			}
		}

		verticalPhase += BigHeight;

		while (verticalPhase >= view.Height)
		{
			verticalPhase -= view.Height;
			++sourceY;
		}
	}

	if (Diagnostics)
	{
		SampleRegion(pDestPixels, destPitch, view, SampleStep, SampleStep,
			DiagDestNonBlack, DiagDestSamples);
	}

	pDestination->Unlock();
	pSource->Unlock();

	return true;
}

bool ViewZoomClass::WantsWideView()
{
	const auto& view = DSurface::ViewBounds;

	return Enabled && WideView && view.Width >= 2 && view.Height >= 2
		&& IsWideLevel(CurrentLevel) && GetFactor() > NativeFactor;
}

// Probe and mitigation for the residue that the engine's incremental
// invalidation leaves behind while zoomed out (a chimney's smoke keeps every
// frame it ever drew). The eight per-cell layers repaint the whole ViewBounds
// when the passes get bFullRedraw, so forcing that flag every N frames bounds
// (or, at N = 1, removes) whatever the per-frame invalidation missed. It also
// answers whether the stale pixels live in the surface the layers repaint at
// all -- if even N = 1 does not clear them, the residue is drawn somewhere our
// repaint never touches.
//
// Cost is one whole-viewport pass (the ZBuffer/ABuffer are filled as well), so
// this is off by default and only meant as a dial while the root cause is
// being chased.
bool ViewZoomClass::ShouldForceFullRedraw()
{
	if (FullRedrawEvery < 0 || !WideLevelLive || !WantsWideView())
		return false;

	int interval = FullRedrawEvery;

	if (interval == 0)
	{
		// Automatic: keep the added cost of the forced repaints within a
		// fraction of a normal frame. At k = 1.25..2.5 that fraction works out
		// to roughly the validated interval of 8; a heavier viewport backs off
		// on its own.
		interval = 8;

		if (NormalFrameCost > 1.0 && ForcedFrameCost > NormalFrameCost)
		{
			const double added = ForcedFrameCost - NormalFrameCost;
			const double budget = 0.35 * NormalFrameCost;

			if (added > budget)
			{
				interval = static_cast<int>(added / budget + 0.5);

				if (interval < 8)
					interval = 8;

				if (interval > 240)
					interval = 240;
			}
		}
	}

	if (--FullRedrawCountdown > 0)
		return false;

	FullRedrawCountdown = interval;
	DiagFullRedrawInterval = interval;
	FrameWasForced = true;

	if (Diagnostics)
		++DiagForcedRedraws;

	return true;
}

// The enlarged viewport only exists between 0x4F44CB and 0x4F451B. Everywhere
// else DSurface::ViewBounds and TacticalClass::TacticalPos describe the native
// viewport, and the engine's own invalidation producers -- which all run from
// game logic, never from the render passes -- use exactly those live values.
//
// TacticalClass::RegisterDirtyArea (0x6D2790) keeps its rectangles in
// viewport-relative space and the layer passes turn them back into screen space
// with the origin live at *consumption* time:
//
//     6D6D39: mov ebp,[edi+0xb0]      ; TacticalPos.X
//     6D6D45: add eax,ebp             ; screenX = rect.X + TacticalPos.X - ViewBounds.X
//
// so a rectangle produced outside the window is short by (enlarged - native)/2
// on each axis. The repaint then lands half the growth away from the cell that
// actually changed, that cell keeps whatever the previous frame's scroll copy
// left there (0x6D3FCF), and it never heals on its own. Moving the rectangle
// here is the only place where the correction can be made: by the time the
// layers consume it, the original origin is gone.
void ViewZoomClass::TranslateDirtyArea(RectangleStruct& area)
{
	const bool wide = WideLevelLive && WantsWideView();

	if (Diagnostics)
		++DiagDirtyAll;

	if (!wide)
		return;

	if (WideInstalled)
	{
		// Produced while the enlarged viewport is live: already in the enlarged
		// space (this is the object and animation draw path, which runs in the
		// object pass after the present). Nothing to move -- only measure.
		if (Diagnostics)
		{
			++DiagDirtyInWindow;

			if (DiagDirtyInWindowSamples < 2)
			{
				++DiagDirtyInWindowSamples;

				const auto& view = DSurface::ViewBounds;

				Debug::Log("[ViewZoom] dirty rect in window: %d,%d %dx%d | view %dx%d big %dx%d\n",
					area.X, area.Y, area.Width, area.Height,
					view.Width, view.Height, BigWidth, BigHeight);
			}
		}

		return;
	}

	if (Diagnostics)
		++DiagDirtySeen;

	if (!TranslateDirtyAreas)
		return;

	if (BigWidth < 2 || BigHeight < 2)
		return;

	// Outside the render window this is the native rectangle, which is what the
	// producer measured against.
	const auto& native = DSurface::ViewBounds;

	if (native.Width < 2 || native.Height < 2)
		return;

	const int shiftX = (BigWidth - native.Width) / 2;
	const int shiftY = (BigHeight - native.Height) / 2;

	if (shiftX <= 0 && shiftY <= 0)
		return;

	area.X += shiftX;
	area.Y += shiftY;

	if (Diagnostics)
	{
		++DiagDirtyCorrected;

		if (DiagDirtySamples < 4)
		{
			++DiagDirtySamples;

			Debug::Log("[ViewZoom] dirty rect moved %+d,%+d: %d,%d %dx%d -> %d,%d | native view %dx%d big %dx%d\n",
				shiftX, shiftY,
				area.X - shiftX, area.Y - shiftY, area.Width, area.Height,
				area.X, area.Y,
				native.Width, native.Height, BigWidth, BigHeight);
		}
	}
}

// TacticalClass::RegisterCellAsVisible (0x6DA7D0) is the only writer of the
// visible-cell queue -- the 25 call sites are all game-logic entry points
// (fog/shroud reveal, building destruction, cell occlusion recompute) and none
// is reachable from TacticalClass::Render. The queue is what the per-cell
// terrain/overlay layers draw from, and the overlay layer is the only caller of
// CellClass::DrawOverlay, so a cell that never gets queued keeps its old pixels.
//
// The function culls the cell against DSurface::ViewBounds with TacticalPos as
// origin (0x6DA901-0x6DA936). With the native rectangle live that rejects every
// cell the enlarged view added. This reproduces the success path with the cull
// widened to the enlarged viewport, and then stamps the cell's redraw frame so
// the engine's own copy -- which runs right after this hook -- short-circuits
// at its frame-stamp check (0x6DA7F1). Nothing has to be skipped and no stack
// frame is disturbed.
void ViewZoomClass::TryWideCellEnqueue(void* pCellVoid)
{
	if (!WideLevelLive || !WantsWideView())
		return;

	// Every call the engine makes while a wide level is live, whether or not
	// the switch below lets us act on it. Together with the enqueue count this
	// is how the queue pressure that starves the engine's own registrations
	// becomes visible.
	if (Diagnostics)
		++DiagCellCalls;

	if (WideInstalled || !WideCellEnqueue)
		return;

	CellClass* pCell = static_cast<CellClass*>(pCellVoid);

	if (!pCell)
		return;

	TacticalClass* pTactical = TacticalClass::Instance;

	if (!pTactical)
		return;

	// The engine's own two gates, in its own order.
	const unsigned int frame = *reinterpret_cast<const unsigned int*>(CurrentFrameIndex);

	if (*reinterpret_cast<const unsigned int*>(
		reinterpret_cast<const unsigned char*>(pCell) + CellFrameStampOffset) == frame)
	{
		return;
	}

	const unsigned char flags = *reinterpret_cast<const unsigned char*>(
		reinterpret_cast<const unsigned char*>(pCell) + CellFlagsOffset);

	const unsigned char pending = *reinterpret_cast<const unsigned char*>(
		reinterpret_cast<const unsigned char*>(pCell) + CellPendingFlagOffset);

	if (!(flags & 8) && pending == 0)
		return;

	const auto& native = DSurface::ViewBounds;

	if (native.Width < 2 || native.Height < 2 || BigWidth < 2 || BigHeight < 2)
		return;

	const int growX = BigWidth - native.Width;
	const int growY = BigHeight - native.Height;

	if (growX <= 0 && growY <= 0)
		return;

	const int halfX = growX > 0 ? growX / 2 : 0;
	const int halfY = growY > 0 ? growY / 2 : 0;

	// The engine's window, including its margins, then widened by exactly half
	// the growth. Y's lower bound is the literal -30 in the engine, so it stays
	// relative to the viewport top rather than to ViewBounds.Y.
	const int nativeMinX = native.X - 30;
	const int nativeMaxX = native.X + native.Width + 30;
	const int nativeMinY = -30;
	const int nativeMaxY = native.Y + native.Height + 15;

	const int* pOrigin = reinterpret_cast<const int*>(
		reinterpret_cast<const unsigned char*>(pTactical) + TacticalViewOrigin);

	const Point2D screen = TacticalClass::CoordsToScreen(pCell->GetCellCoords());
	const int relX = screen.X - pOrigin[0];
	const int relY = screen.Y - pOrigin[1];

	const bool nativeInside = relX >= nativeMinX && relX <= nativeMaxX
		&& relY >= nativeMinY && relY <= nativeMaxY;

	const bool wideInside = relX >= nativeMinX - halfX && relX <= nativeMaxX + halfX
		&& relY >= nativeMinY - halfY && relY <= nativeMaxY + halfY;

	if (!wideInside)
		return;

	if (Diagnostics)
	{
		++DiagCellSeen;

		if (!nativeInside)
			++DiagCellWideOnly;

		if (DiagCellSamples < 4)
		{
			++DiagCellSamples;

			Debug::Log("[ViewZoom] cell enqueue (%d,%d) rel %d,%d native %d | native view %dx%d big %dx%d\n",
				pCell->GetCellCoords().X, pCell->GetCellCoords().Y, relX, relY,
				nativeInside ? 1 : 0, native.Width, native.Height, BigWidth, BigHeight);
		}
	}

	// Mirror the engine's success path exactly. If the list is full the engine
	// would skip the insert, clear the pending flag and set Redrawing; leaving
	// that to the engine keeps the cap semantics untouched.
	if (pTactical->VisibleCellCount >= VisibleCellCapacity)
	{
		if (Diagnostics)
			++DiagCellCapped;

		return;
	}

	pTactical->VisibleCells[pTactical->VisibleCellCount] = pCell;
	++pTactical->VisibleCellCount;

	*reinterpret_cast<unsigned char*>(
		reinterpret_cast<unsigned char*>(pCell) + CellPendingFlagOffset) = 0;

	*reinterpret_cast<unsigned int*>(
		reinterpret_cast<unsigned char*>(pCell) + CellFrameStampOffset) = frame;

	*reinterpret_cast<unsigned char*>(
		reinterpret_cast<unsigned char*>(pTactical) + TacticalRedrawFlagOffset) = 1;

	if (Diagnostics)
		++DiagCellEnqueued;
}

// Frame start, before the engine presents the previous frame (the presentation
// happens at 0x53BAE0, between this point and InstallWideRender). The enlarged
// pair must NOT be installed yet, otherwise that presentation reads the
// enlarged surface and shows a 1:1 crop of it instead of the finished frame.
void ViewZoomClass::BeginTacticalRender()
{
	// Defensive: a previous frame that never reached the post hook would leave
	// the globals pointing at our pair.
	if (WideInstalled)
		EndTacticalRender();

	// Frame timing for the automatic FullRedrawEvery. Only wide frames are
	// measured, and the window closes in EndTacticalRender.
	FrameTimed = WideLevelLive && WantsWideView();

	if (FrameTimed)
		FrameStartTick = GetTickCount();

	// A click may have left the viewport globals describing the enlarged view;
	// the frame starts from the native one.
	EndInputViewport();

	// Is the sidebar being redrawn at all while a wide level is active? Its
	// surface changing but the screen not following means a presentation
	// problem; never changing means the redraw is being suppressed.
	if (Diagnostics && DSurface::Sidebar && DSurface::Primary)
	{
		const RectangleStruct sidebar { 0, 0, DSurface::Sidebar->Width, DSurface::Sidebar->Height };
		const unsigned int hash = FingerprintRect(DSurface::Sidebar, sidebar);

		if (hash != DiagSidebarHash)
		{
			DiagSidebarHash = hash;
			++DiagSidebarChanges;
		}

		// The same region on the presented surface. Surface changing while this
		// stays put means the sidebar is drawn but never presented; both frozen
		// means the gadget redraw is being suppressed.
		const RectangleStruct shown {
			DSurface::ViewBounds.X + DSurface::ViewBounds.Width, 0,
			DSurface::Sidebar->Width, DSurface::Primary->Height };
		if (DSurface::Hidden)
		{
			const unsigned int hiddenHash = FingerprintRect(DSurface::Hidden, shown);

			if (hiddenHash != DiagSidebarHiddenHash)
			{
				DiagSidebarHiddenHash = hiddenHash;
				++DiagSidebarHiddenChanges;
			}
		}

		const unsigned int shownHash = FingerprintRect(DSurface::Primary, shown);

		if (shownHash != DiagSidebarShownHash)
		{
			DiagSidebarShownHash = shownHash;
			++DiagSidebarShownChanges;
		}
	}

	// The viewport is native here (End restored it last frame), so this is the
	// baseline every "effective size" calculation has to be derived from.
	if (!WideInstalled)
		NativeViewSize = DSurface::ViewBounds;

	SynchronizeState();

	BeginGeometrySample();
	LogGeometry("begin");
}

// Right before the three TacticalClass::Render passes: install the enlarged
// pair and the enlarged viewport for the duration of the passes.
void ViewZoomClass::InstallWideRender()
{
	if (!WantsWideView())
		return;

	if (WideInstalled)
		return;

	const auto& view = DSurface::ViewBounds;
	RectangleStruct viewport;

	if (!GetRenderViewport(view, viewport))
		return;

	const int width = viewport.Width;
	const int height = viewport.Height;

	if (IsWholeMapLevel(CurrentLevel) && Diagnostics)
	{
		Debug::Log("[ViewZoom] widest level: map %dx%d, native view %dx%d, k=%.3f\n",
			width, height, view.Width, view.Height, GetFactor());
	}

	// Has to be captured before EnsureBigSurfaces updates the cached size.
	const bool viewportChanged = (pBigComposite == nullptr
		|| width != BigWidth || height != BigHeight);

	if (!EnsureBigSurfaces(width, height))
	{
		if (IsWholeMapLevel(CurrentLevel))
		{
			Debug::Log("[ViewZoom] whole map needs a %dx%d surface pair; staying native\n",
				width, height);
		}

		return;
	}

	if (!DSurface::Composite || !DSurface::Tile)
		return;

	if (DSurface::Composite == pBigComposite || DSurface::Tile == pBigTile)
		return;

	pSavedComposite = DSurface::Composite;
	pSavedTile = DSurface::Tile;
	pSavedTemp = DSurface::Temp;
	SavedViewBounds = view;

	SavedViewBound[0] = *reinterpret_cast<int*>(TacticalViewBound + 0u);
	SavedViewBound[1] = *reinterpret_cast<int*>(TacticalViewBound + 4u);
	SavedViewBound[2] = *reinterpret_cast<int*>(TacticalViewBound + 8u);
	SavedViewBound[3] = *reinterpret_cast<int*>(TacticalViewBound + 12u);

	// Where the finished frame is presented: the whole native view for the
	// plain k > 1 levels, a centred uniform fit (letterbox) for the whole-map
	// level, whose 2:1 map bounds cannot fill the map area without distorting.
	if (!CalculateWideDestination(SavedViewBounds, width, height, WideDestRect))
	{
		WideDestRect = { 0, 0, 0, 0 };
		return;
	}

	if (Diagnostics)
	{
		Debug::Log("[ViewZoom] present rect %d,%d %dx%d inside view %d,%d %dx%d\n",
			WideDestRect.X, WideDestRect.Y, WideDestRect.Width, WideDestRect.Height,
			SavedViewBounds.X, SavedViewBounds.Y, SavedViewBounds.Width, SavedViewBounds.Height);
	}

	// The renderer reads DSurface::ViewBounds for the projection and view_bound
	// for the visible cell range and the per-cell blit rectangles (0x6D3E9B
	// onwards). Both have to grow together, and the tactical class has to
	// rebuild its derived state through its own viewport change: writing only
	// the globals leaves the isometric matrix on the old viewport, which shows
	// up as terrain zoomed but objects and buildings still at the old scale.
	const RectangleStruct big { 0, 0, width, height };

	// The renderer's own view-size scratch buffers (0x887644 / 0x87E8A4) have to
	// grow with the viewport. Skipping this is what made every wide level write
	// a viewport's worth of pixels into a native-sized buffer and corrupt the
	// heap; if they cannot be rebuilt the level stays native instead.
	if (!EnsureWideViewBuffers(big) || !InstallWideViewBuffers())
		return;

	DSurface::ViewBounds = big;

	// A new size goes through the engine's own viewport change so the camera is
	// clamped against it; otherwise only the derived state has to follow.
	if (viewportChanged)
	{
		TacticalClass* pTactical = TacticalClass::Instance;
		int beforeX = 0;
		int beforeY = 0;
		int afterX = 0;
		int afterY = 0;

		unsigned char* pBytes = reinterpret_cast<unsigned char*>(pTactical);

		if (pTactical)
		{
			beforeX = *reinterpret_cast<const int*>(pBytes + 0xD74u);
			beforeY = *reinterpret_cast<const int*>(pBytes + 0xD78u);
		}

		// The engine's own viewport change clamps the camera against the new
		// rectangle, which teleports the view when the bigger viewport would
		// stick out of the map. Zooming is a view change, not a camera move, so
		// the camera is restored afterwards and the engine's normal clamping
		// (which our hooks feed the effective size) takes over during play.
		//
		const bool keepClampedCamera = false;

		SeedBigSurfaces(pSavedComposite);

		ApplyTacticalViewport(big);

		if (pTactical && !keepClampedCamera)
		{
			*reinterpret_cast<int*>(pBytes + 0xD64u) = beforeX;
			*reinterpret_cast<int*>(pBytes + 0xD68u) = beforeY;
			*reinterpret_cast<int*>(pBytes + 0xD74u) = beforeX;
			*reinterpret_cast<int*>(pBytes + 0xD78u) = beforeY;

			RebuildTacticalView();
		}

		if (pTactical)
		{
			const unsigned char* pAfter = reinterpret_cast<const unsigned char*>(pTactical);
			afterX = *reinterpret_cast<const int*>(pAfter + 0xD74u);
			afterY = *reinterpret_cast<const int*>(pAfter + 0xD78u);
		}

		if (Diagnostics)
		{
			Debug::Log("[ViewZoom] viewport %dx%d: cam %d,%d -> %d,%d (delta %d,%d)%s\n",
				width, height, beforeX, beforeY, afterX, afterY,
				afterX - beforeX, afterY - beforeY,
				keepClampedCamera ? " (centred)" : "");
		}
	}
	else
	{
		WriteViewBound(big);
		RebuildTacticalView();
	}

	// Install the enlarged pair. TacticalClass::Render swaps Composite and Tile
	// once per frame (0x6D405D), so both roles have to be ours and both have to
	// be the same size.
	DSurface::Composite = pBigComposite;
	DSurface::Tile = pBigTile;
	DSurface::Temp = pBigComposite;

	WideInstalled = true;
	WideLevelLive = true;

	// The engine only draws what changed, so a freshly enlarged viewport would
	// stay unpainted. Feed it one slice of the map per frame through its own
	// queue until the whole map has been drawn once.
	TacticalClass* pForBand = TacticalClass::Instance;

	if (pForBand)
	{
		const unsigned char* pCameraBytes = reinterpret_cast<const unsigned char*>(pForBand);

		RefreshBandFillCamera(
			*reinterpret_cast<const int*>(pCameraBytes + CameraXOffset),
			*reinterpret_cast<const int*>(pCameraBytes + CameraYOffset));
	}

	InjectBandFill(BandCellsPerPass);

	LogGeometry("install");
}

void ViewZoomClass::EndTacticalRender()
{
	// Close the frame timing first: the tactical passes have just finished.
	if (FrameTimed)
	{
		FrameTimed = false;

		const unsigned int elapsed = GetTickCount() - FrameStartTick;
		double& cost = FrameWasForced ? ForcedFrameCost : NormalFrameCost;

		cost = cost > 0.0
			? cost * 0.9 + static_cast<double>(elapsed) * 0.1
			: static_cast<double>(elapsed);

		FrameWasForced = false;
	}

	// Safety net: the sidebar swap must never survive the frame.
	if (SidebarViewActive)
		EndSidebarNativeView();

	// The same for the UI step's native surfaces.
	EndUiNativeStep();

	if (!WideInstalled)
	{
		ApplyToComposite() ? ++DiagMagnifyOk : ++DiagMagnifyFail;

		if (Diagnostics)
			ReportDiagnostics();

		return;
	}

	LogGeometry("end");

	// Find out which of our surfaces the engine left in the Composite role:
	// the swap happens once per pass set, so the roles may have flipped.
	DSurface* pCurrent = DSurface::Composite;
	DSurface* pSource = nullptr;
	DSurface* pDestination = nullptr;

	++DiagWideFrames;

	if (pCurrent == pBigComposite)
	{
		++DiagWideRoleA;
		pSource = pBigComposite;
		pDestination = pSavedComposite;
		DSurface::Composite = pSavedComposite;
		DSurface::Tile = pSavedTile;
	}
	else if (pCurrent == pBigTile)
	{
		++DiagWideRoleB;
		pSource = pBigTile;
		pDestination = pSavedTile;
		DSurface::Composite = pSavedTile;
		DSurface::Tile = pSavedComposite;
	}
	else
	{
		++DiagWideRoleLost;
		DSurface::Composite = pSavedComposite;
		DSurface::Tile = pSavedTile;

		if (ShowHint)
		{
			Debug::Log("[ViewZoom] render pair lost (Composite %p, expected %p / %p)\n",
				static_cast<void*>(pCurrent), static_cast<void*>(pBigComposite),
				static_cast<void*>(pBigTile));
		}
	}

	// The engine keeps DSurface::Temp aliased to the surface being presented
	// (0x4F4492 sets it, 0x6D4076 re-sets it after the internal swap), and the
	// window/cursor code reads it, so it has to follow the Composite role
	// instead of being restored to the pre-render value.
	DSurface::Temp = DSurface::Composite;

	DSurface::ViewBounds = SavedViewBounds;
	WriteViewBound(SavedViewBounds);
	RebuildTacticalView();

	// The view-size scratch buffers go back to the engine's native ones together
	// with the viewport, so anything that runs after the passes sees a pair that
	// matches the rectangle.
	RestoreWideViewBuffers();

	WideInstalled = false;

	if (pSource && pDestination && DebugFill)
	{
		DSurface* pOther = (pDestination == pSavedComposite) ? pSavedTile : pSavedComposite;

		WideReady = FillNativeView(pDestination);

		if (pOther && pOther != pDestination)
			FillNativeView(pOther);

		++DiagDownscaleOk;
	}
	else if (pSource && pDestination)
	{
		WideReady = DownscaleToNative(pSource, pDestination);

		// While a wide level is active the engine renders into the enlarged
		// pair only, so both native surfaces are free to hold the finished
		// frame. Keeping them identical means the presentation shows the same
		// image whichever of the two (or Temp) it reads.
		DSurface* pOther = (pDestination == pSavedComposite) ? pSavedTile : pSavedComposite;

		if (pOther && pOther != pDestination)
			DownscaleToNative(pSource, pOther);

		WideReady ? ++DiagDownscaleOk : ++DiagDownscaleFail;
	}

	if (Diagnostics)
		ReportDiagnostics();
}

// 0x6A6C30 is the sidebar's own redraw + present routine. It is reached from
// 0x6D0E38, inside the GScreenClass virtual that 0x4F4502 calls between the
// second and the third tactical pass -- that is, from inside the very window in
// which the passes render against the enlarged viewport.
//
// The routine puts the strip on the primary surface at
// `client + DSurface::ViewBounds.Width` (right/left comes from
// GameOptionsClass::SidebarMode at 0xA8EB7C) and repeats that read for every
// band it blits, so with the enlarged ViewBounds still installed the sidebar is
// presented at `native * k`: at 800x600 / k = 1.25 the destination starts at
// x = 790, which leaves only the last 10 pixels of the strip on screen and
// freezes everything left of them; from k = 1.5 on nothing is visible at all.
// The map is untouched because its own present clips against WindowBounds and
// DSurface::Sidebar -- which is exactly why the symptom was a frozen sidebar on
// top of a perfectly correct map.
//
// Handing the routine the native rectangle for the duration of the call keeps
// the interface where it belongs while the map render keeps the enlarged
// viewport. Only this one call is wrapped.
void ViewZoomClass::BeginSidebarNativeView()
{
	// Called at 0x6D0E38, i.e. after 0x6D0E37 pushed the routine's argument and
	// immediately before the engine's own `call 0x6A6C30`. The call itself is
	// left to the engine: the callee is `ret 4` and therefore pops that argument,
	// so making the call from here would unbalance the caller's epilogue.
	if (SidebarViewActive || !WideInstalled || !SidebarNativeView)
		return;

	if (NativeViewSize.Width < 2 || NativeViewSize.Height < 2)
		return;

	SidebarViewSaved = DSurface::ViewBounds;
	DSurface::ViewBounds = NativeViewSize;
	SidebarViewActive = true;

	if (Diagnostics && SidebarViewSaved.Width != DiagSidebarLoggedWidth)
	{
		DiagSidebarLoggedWidth = SidebarViewSaved.Width;

		Debug::Log("[ViewZoom] sidebar present with the native view: ViewBounds.Width %d -> %d\n",
			SidebarViewSaved.Width, NativeViewSize.Width);
	}
}

void ViewZoomClass::EndSidebarNativeView()
{
	// Called at 0x6D0E3D, the instruction after the engine's call returned.
	if (!SidebarViewActive)
		return;

	DSurface::ViewBounds = SidebarViewSaved;
	SidebarViewActive = false;

	++DiagSidebarNativePresents;
}

// 0x4F44F9..0x4F4505: the UI step between the tactical passes and the object
// pass. The GScreenClass virtual at 0x4F4502 redraws the sidebar and the bottom
// bar; natively it draws them into DSurface::Temp, which at this point aliases
// the native Composite (0x4F4492 set Temp := Composite before the passes).
//
// Inside the widened window Temp points at the enlarged Composite, so the
// bottom bar strip is written at native coordinates into the enlarged surface
// -- inside the map area, since the enlarged surface is taller than the native
// view -- and our downscale then bakes it into the battlefield. (Measured: the
// strip lands at the native view height, i.e. around 4/5 of the map height at
// 1.25x.)
//
// The whole step therefore runs against the native pair and the native
// rectangle, exactly as it would without the zoom. Everything is put back at
// 0x4F4505, before the object pass needs the enlarged pair again.
void ViewZoomClass::BeginUiNativeStep()
{
	if (UiStepActive || !WideInstalled)
		return;

	if (!pSavedComposite || !pSavedTile || !pSavedTemp)
		return;

	UiStepSavedComposite = DSurface::Composite;
	UiStepSavedTile = DSurface::Tile;
	UiStepSavedTemp = DSurface::Temp;
	UiStepSavedViewBounds = DSurface::ViewBounds;

	const int* pViewBound = reinterpret_cast<const int*>(TacticalViewBound);

	for (int i = 0; i < 4; ++i)
		UiStepSavedViewBound[i] = pViewBound[i];

	DSurface::Composite = pSavedComposite;
	DSurface::Tile = pSavedTile;
	DSurface::Temp = pSavedTemp;
	DSurface::ViewBounds = SavedViewBounds;
	WriteViewBound(SavedViewBounds);

	UiStepActive = true;
}

void ViewZoomClass::EndUiNativeStep()
{
	if (!UiStepActive)
		return;

	DSurface::Composite = UiStepSavedComposite;
	DSurface::Tile = UiStepSavedTile;
	DSurface::Temp = UiStepSavedTemp;
	DSurface::ViewBounds = UiStepSavedViewBounds;

	int* pViewBound = reinterpret_cast<int*>(TacticalViewBound);

	for (int i = 0; i < 4; ++i)
		pViewBound[i] = UiStepSavedViewBound[i];

	UiStepActive = false;
}

// ---------------------------------------------------------------------------
// magnify (k < 1) rendering
// ---------------------------------------------------------------------------

bool ViewZoomClass::ApplyToComposite()
{
	if (!Active)
		return false;

	// Composite and Tile swap places every frame (double buffering), so the
	// global has to be re-read here and in RestoreNativeFrame.
	DSurface* pSurface = DSurface::Composite;

	if (!pSurface || pSurface->GetBytesPerPixel() != RequiredBytesPerPixel)
		return false;

	const auto& view = DSurface::ViewBounds;
	RectangleStruct source;

	if (!CalculateSourceRect(view, source))
		return false;

	const int surfaceWidth = pSurface->Width;
	const int surfaceHeight = pSurface->Height;

	if (view.X < 0 || view.Y < 0
		|| view.X + view.Width > surfaceWidth
		|| view.Y + view.Height > surfaceHeight
		|| source.X < view.X || source.Y < view.Y
		|| source.X + source.Width > view.X + view.Width
		|| source.Y + source.Height > view.Y + view.Height)
	{
		return false;
	}

	const unsigned int requiredPixels =
		static_cast<unsigned int>(view.Width) * static_cast<unsigned int>(view.Height);

	if (!requiredPixels || requiredPixels > MaxScratchPixels)
		return false;

	if (!Scratch || ScratchPixels < requiredPixels)
	{
		unsigned short* pReplacement = static_cast<unsigned short*>(
			YRMemory::Allocate(requiredPixels * sizeof(unsigned short)));

		if (!pReplacement)
		{
			Active = false;
			VisualReady = false;
			return false;
		}

		if (Scratch)
			YRMemory::Deallocate(Scratch);

		Scratch = pReplacement;
		ScratchPixels = requiredPixels;
		NativeFrameValid = false;
	}

	const int pitch = pSurface->GetPitch();
	unsigned char* pPixels = static_cast<unsigned char*>(pSurface->Lock(0, 0));

	if (!pPixels || pitch <= 0)
	{
		if (pPixels)
			pSurface->Unlock();

		return false;
	}

	// 1. Keep the complete native view. The pre-render hook restores it before
	// the next dirty-area pass so partial redraws keep working on native pixels.
	for (int y = 0; y < view.Height; ++y)
	{
		const unsigned short* pSourceRow = reinterpret_cast<const unsigned short*>(
			pPixels + (view.Y + y) * pitch + view.X * 2);

		CopyRow(Scratch + y * view.Width, pSourceRow, view.Width);
	}

	NativeView = view;
	NativeFrameValid = true;

	// 2. Scale the crop up to the whole view, in place. Fixed point
	// accumulators avoid floating point and per-pixel division; a destination
	// row that maps to the same source row reuses the row already expanded.
	const int sourceOffsetX = source.X - view.X;
	const int sourceOffsetY = source.Y - view.Y;
	int sourceY = 0;
	int verticalPhase = 0;
	int previousSourceY = -1;
	unsigned short* pPreviousRow = nullptr;

	for (int destinationY = 0; destinationY < view.Height; ++destinationY)
	{
		if (sourceY >= source.Height)
			sourceY = source.Height - 1;

		unsigned short* pDestinationRow = reinterpret_cast<unsigned short*>(
			pPixels + (view.Y + destinationY) * pitch + view.X * 2);

		if (pPreviousRow && sourceY == previousSourceY)
		{
			CopyRow(pDestinationRow, pPreviousRow, view.Width);
		}
		else
		{
			const unsigned short* pSourceRow = Scratch
				+ (sourceOffsetY + sourceY) * view.Width + sourceOffsetX;
			int sourceX = 0;
			int horizontalPhase = 0;

			for (int destinationX = 0; destinationX < view.Width; ++destinationX)
			{
				if (sourceX >= source.Width)
					sourceX = source.Width - 1;

				pDestinationRow[destinationX] = pSourceRow[sourceX];

				horizontalPhase += source.Width;

				if (horizontalPhase >= view.Width)
				{
					horizontalPhase -= view.Width;
					++sourceX;
				}
			}
		}

		previousSourceY = sourceY;
		pPreviousRow = pDestinationRow;
		verticalPhase += source.Height;

		if (verticalPhase >= view.Height)
		{
			verticalPhase -= view.Height;
			++sourceY;
		}
	}

	pSurface->Unlock();

	SourceRect = source;
	VisualReady = true;

	return true;
}

bool ViewZoomClass::RestoreNativeFrame()
{
	if (!Active || !NativeFrameValid || !Scratch)
		return false;

	const auto& view = DSurface::ViewBounds;

	if (!SameRect(view, NativeView) || view.Width <= 0 || view.Height <= 0)
	{
		NativeFrameValid = false;
		return false;
	}

	DSurface* pSurface = DSurface::Composite;

	if (!pSurface || pSurface->GetBytesPerPixel() != RequiredBytesPerPixel)
		return false;

	if (view.X < 0 || view.Y < 0
		|| view.X + view.Width > pSurface->Width
		|| view.Y + view.Height > pSurface->Height)
	{
		NativeFrameValid = false;
		return false;
	}

	const int pitch = pSurface->GetPitch();
	unsigned char* pPixels = static_cast<unsigned char*>(pSurface->Lock(0, 0));

	if (!pPixels || pitch <= 0)
	{
		if (pPixels)
			pSurface->Unlock();

		return false;
	}

	for (int y = 0; y < view.Height; ++y)
	{
		unsigned short* pDestinationRow = reinterpret_cast<unsigned short*>(
			pPixels + (view.Y + y) * pitch + view.X * 2);

		CopyRow(pDestinationRow, Scratch + y * view.Width, view.Width);
	}

	pSurface->Unlock();

	return true;
}

// ---------------------------------------------------------------------------
// input mapping
// ---------------------------------------------------------------------------

int ViewZoomClass::ScaleMapDistanceX(int distance)
{
	const auto& view = DSurface::ViewBounds;

	if (view.Width <= 0)
		return distance;

	return static_cast<int>(distance * GetViewScale());
}

int ViewZoomClass::ScaleMapDistanceY(int distance)
{
	const auto& view = DSurface::ViewBounds;

	if (view.Height <= 0)
		return distance;

	return static_cast<int>(distance * GetViewScale());
}

int ViewZoomClass::GetEffectiveViewWidth()
{
	const auto& view = DSurface::ViewBounds;
	RectangleStruct source;

	if (!Enabled)
		return view.Width;

	if (Active && CalculateSourceRect(view, source))
		return source.Width;

	if (WideView && IsWideLevel(CurrentLevel) && NativeViewSize.Width > 0
		&& pBigComposite && pBigTile)
	{
		RectangleStruct viewport;

		// Inside the render window view_bound is already the enlarged rectangle;
		// scaling it again would inflate the camera's clamp range and push the
		// whole view around. The whole-map viewport is not derived from the
		// native size at all, so it comes from the same helper the install uses.
		if (GetRenderViewport(NativeViewSize, viewport))
		{
			if (view.Width >= viewport.Width)
				return view.Width;

			return viewport.Width;
		}
	}

	return view.Width;
}

int ViewZoomClass::GetEffectiveViewHeight()
{
	const auto& view = DSurface::ViewBounds;
	RectangleStruct source;

	if (!Enabled)
		return view.Height;

	if (Active && CalculateSourceRect(view, source))
		return source.Height;

	if (WideView && IsWideLevel(CurrentLevel) && NativeViewSize.Height > 0
		&& pBigComposite && pBigTile)
	{
		RectangleStruct viewport;

		if (GetRenderViewport(NativeViewSize, viewport))
		{
			if (view.Height >= viewport.Height)
				return view.Height;

			return viewport.Height;
		}
	}

	return view.Height;
}

bool ViewZoomClass::TransformClientPoint(const Point2D& input, Point2D& output)
{
	const auto& view = DSurface::ViewBounds;

	if (view.Width <= 0 || view.Height <= 0 || !PointInsideRect(input, view))
		return false;

	// Magnify: map the screen point into the crop inside the native render,
	// which is the coordinate space the engine's projection works in.
	if (Active)
	{
		if (!VisualReady || SourceRect.Width <= 0 || SourceRect.Height <= 0)
			return false;

		int localX = (input.X - view.X) * SourceRect.Width / view.Width;
		int localY = (input.Y - view.Y) * SourceRect.Height / view.Height;

		localX = Clamp(localX, 0, SourceRect.Width - 1);
		localY = Clamp(localY, 0, SourceRect.Height - 1);

		output.X = SourceRect.X + localX;
		output.Y = SourceRect.Y + localY;

		return true;
	}

	// Wider view: the screen shows the enlarged frame scaled down while the
	// engine still projects against the native view.
	if (!WideView || !WideReady || GetFactor() <= NativeFactor)
		return false;

	if (WideDestRect.Width < 2 || WideDestRect.Height < 2 || BigWidth < 2 || BigHeight < 2)
		return false;

	// 1. Point -> pixel in the enlarged viewport, through the rectangle the frame
	//    was actually presented in. That rectangle is the whole native view for
	//    the plain k > 1 levels, but the whole-map level is letterboxed, so
	//    anchoring on the view centre instead would shift every click by the
	//    height of the bar (measured: 168 px at 2560x1600).
	const double scaleX = static_cast<double>(BigWidth) / WideDestRect.Width;
	const double scaleY = static_cast<double>(BigHeight) / WideDestRect.Height;

	const double bigX = WideDestRect.X + (input.X - WideDestRect.X) * scaleX;
	const double bigY = WideDestRect.Y + (input.Y - WideDestRect.Y) * scaleY;

	// The result is in the enlarged viewport, which is the space the engine has
	// to un-project in: the engine rejects a client point outside view_bound and
	// returns a fixed fallback for it, so BeginInputViewport() makes its viewport
	// globals describe this same rectangle for the duration of the click. Only
	// then is the world point under a screen corner expressible at all - in the
	// native viewport it lies far outside.
	output.X = static_cast<int>(std::lround(bigX));
	output.Y = static_cast<int>(std::lround(bigY));

	return true;
}

void ViewZoomClass::BeginInputViewport()
{
	if (InputViewportInstalled)
		return;

	if (!Enabled || !WideView || !WideReady || GetFactor() <= NativeFactor)
		return;

	if (BigWidth < 2 || BigHeight < 2)
		return;

	TacticalClass* pTactical = TacticalClass::Instance;

	if (!pTactical)
		return;

	const int* pViewBound = reinterpret_cast<const int*>(TacticalViewBound);

	for (int i = 0; i < 4; ++i)
		SavedInputViewBound[i] = pViewBound[i];

	unsigned char* pBytes = reinterpret_cast<unsigned char*>(pTactical);

	SavedInputTacticalPos[0] = *reinterpret_cast<int*>(pBytes + TacticalViewOrigin);
	SavedInputTacticalPos[1] = *reinterpret_cast<int*>(pBytes + TacticalViewOrigin + 4u);

	TracePreviousOrigin[0] = SavedInputTacticalPos[0];
	TracePreviousOrigin[1] = SavedInputTacticalPos[1];

	const int cameraX = *reinterpret_cast<const int*>(pBytes + CameraXOffset);
	const int cameraY = *reinterpret_cast<const int*>(pBytes + CameraYOffset);

	TraceCamera[0] = cameraX;
	TraceCamera[1] = cameraY;

	const RectangleStruct big { 0, 0, BigWidth, BigHeight };
	WriteViewBound(big);

	// TacticalPos is the view origin that both the renderer and ClientToCoords
	// subtract from a projected point: camera minus half the viewport.
	*reinterpret_cast<int*>(pBytes + TacticalViewOrigin) = cameraX - BigWidth / 2;
	*reinterpret_cast<int*>(pBytes + TacticalViewOrigin + 4u) = cameraY - BigHeight / 2;

	InputViewportInstalled = true;
}

void ViewZoomClass::EndInputViewport()
{
	if (!InputViewportInstalled)
		return;

	InputViewportInstalled = false;

	int* pViewBound = reinterpret_cast<int*>(TacticalViewBound);

	for (int i = 0; i < 4; ++i)
		pViewBound[i] = SavedInputViewBound[i];

	TacticalClass* pTactical = TacticalClass::Instance;

	if (pTactical)
	{
		unsigned char* pBytes = reinterpret_cast<unsigned char*>(pTactical);

		*reinterpret_cast<int*>(pBytes + TacticalViewOrigin) = SavedInputTacticalPos[0];
		*reinterpret_cast<int*>(pBytes + TacticalViewOrigin + 4u) = SavedInputTacticalPos[1];
	}
}

bool ViewZoomClass::TransformViewRelativePoint(const Point2D& input, Point2D& output)
{
	const auto& view = DSurface::ViewBounds;
	const Point2D physical { input.X + view.X, input.Y + view.Y };
	Point2D logical = physical;

	if (!TransformClientPoint(physical, logical))
		return false;

	output.X = logical.X - view.X;
	output.Y = logical.Y - view.Y;

	return true;
}

bool ViewZoomClass::TooltipIntersectsView(const void* pToolTip)
{
	if (!IsZoomed() || !pToolTip)
		return false;

	const unsigned char* pBytes = static_cast<const unsigned char*>(pToolTip);

	if (!*reinterpret_cast<void* const*>(pBytes + 0x04u))
		return false;

	const RectangleStruct& bounds = *reinterpret_cast<const RectangleStruct*>(pBytes + 0x18u);
	const auto& view = DSurface::ViewBounds;

	if (bounds.Width <= 0 || bounds.Height <= 0 || view.Width <= 0 || view.Height <= 0)
		return false;

	return bounds.X < view.X + view.Width
		&& bounds.X + bounds.Width > view.X
		&& bounds.Y < view.Y + view.Height
		&& bounds.Y + bounds.Height > view.Y;
}

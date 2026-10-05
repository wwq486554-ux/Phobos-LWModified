#include <Phobos.h>
#include <Misc/ViewZoom.h>
#include <Utilities/Debug.h>

#include <algorithm>

#include <Helpers/Macro.h>
#include <Surface.h>
#include <TacticalClass.h>

/*
 *	Tactical view zoom -- hooks.
 *
 *	Stage A diagnostics stay in this file, gated behind
 *	[Phobos] ViewZoom.Diagnostics (off by default).
 *
 *	Stage B added the window procedure hook that watches the middle button.
 *
 *	Stage C adds the magnify (k < 1) rendering pair around the tactical passes
 *	and the input mapping that has to agree with the cropped image: camera
 *	clamping, click coordinates, the selection band, right-drag scrolling and
 *	tooltips. All of them reproduce the overridden instructions and return
 *	explicit continuation addresses, so an inactive zoom leaves the native flow
 *	bit for bit unchanged.
 *
 *	Note for whoever adds logging here: Debug::Log does NOT append a newline,
 *	every format string has to end with one or the output runs into the next
 *	engine log message.
 */

namespace ViewZoomDiag
{

	// Report cadence, in milliseconds of wall clock time.
	constexpr unsigned int ReportIntervalMs = 5000;
	constexpr int VisibleCellCapacity = 800;

	// The hooks stay installed but do nothing unless [Phobos]
	// ViewZoom.Diagnostics asks for them (off by default, they are a
	// measurement tool, not a feature).
	inline bool Enabled()
	{
		return ViewZoomClass::Diagnostics;
	}

	// Tactical render passes. 004F44CB is only reached when the frame really
	// runs the passes; the skip branch above jumps straight to 004F451B, so a
	// pending flag keeps the accounting honest.
	bool RenderPending = false;
	unsigned int RenderStartTick = 0;
	unsigned int WindowStartTick = 0;
	unsigned int RenderPasses = 0;
	unsigned int WindowRenderTicks = 0;
	unsigned int WindowPeakRenderTicks = 0;

	// Cell registration. Every cell handed to the list passes 006DA938; the
	// count read there is the list length *before* this insert. A decrease
	// outside 006D4571 would mean the list was dropped somewhere else, which is
	// the only observable signature of the 800 entry cap truncating inserts.
	unsigned int CellsThisPass = 0;
	unsigned int CellsInWindow = 0;
	unsigned int CellsMaxPerPass = 0;

	// Inserts that arrive while the list already holds 799 entries. The engine
	// skips the insert *and* clears the cell's pending flag, so these are
	// updates that are never retried -- the observable cause of the missing
	// regions in the enlarged surface.
	unsigned int DroppedCells = 0;
	unsigned int PassDroppedCells = 0;
	unsigned int DroppedMaxPerPass = 0;
	int CellListPeak = 0;
	int PassCellListPeak = 0;
	int LastObservedCount = -1;
	unsigned int CountResets = 0;
	unsigned int SaturatedPasses = 0;

	// How often the engine drops the whole list (once per render pass).
	unsigned int ListClears = 0;

	// One-shot probe of how many entries were still in the list when the engine
	// cleared it (proves whether the banded fill survives into the passes).
	unsigned int ClearProbeCount = 0;

	// The engine rebuilds every surface when the display layout changes, so the
	// interesting values are logged whenever the signature changes rather than
	// only once (the first call may well happen on a menu screen).
	//
	// The surface *pointer* must not be part of the signature: Composite and
	// Tile swap places every frame (double buffering), so comparing pointers
	// logs on every single frame.
	struct Signature
	{
		int viewX, viewY, viewWidth, viewHeight;
		int compositeWidth, compositeHeight;
		int screenWidth, screenHeight;

		bool operator==(const Signature& rhs) const
		{
			return viewX == rhs.viewX && viewY == rhs.viewY
				&& viewWidth == rhs.viewWidth && viewHeight == rhs.viewHeight
				&& compositeWidth == rhs.compositeWidth
				&& compositeHeight == rhs.compositeHeight
				&& screenWidth == rhs.screenWidth
				&& screenHeight == rhs.screenHeight;
		}
	};

	Signature LastSignature { -1, -1, -1, -1, -1, -1, -1, -1 };

	Signature CurrentSignature()
	{
		const auto& view = DSurface::ViewBounds;

		Signature signature;
		signature.viewX = view.X;
		signature.viewY = view.Y;
		signature.viewWidth = view.Width;
		signature.viewHeight = view.Height;
		signature.compositeWidth = DSurface::Composite ? DSurface::Composite->Width : -1;
		signature.compositeHeight = DSurface::Composite ? DSurface::Composite->Height : -1;
		signature.screenWidth = *reinterpret_cast<int*>(0x8A00A4u);
		signature.screenHeight = *reinterpret_cast<int*>(0x8A00A8u);

		return signature;
	}

	static void LogSurface(const char* pName, DSurface* pSurface)
	{
		if (!pSurface)
		{
			Debug::Log("[ViewZoom] %-10s (null)\n", pName);
			return;
		}

		Debug::Log("[ViewZoom] %-10s %p  %dx%d\n",
			pName, static_cast<void*>(pSurface), pSurface->Width, pSurface->Height);
	}

	static void LogLayout(const Signature& signature)
	{
		const auto& view = DSurface::ViewBounds;

		Debug::Log("[ViewZoom] ---- layout sample ----\n");
		LogSurface("Primary", DSurface::Primary);
		LogSurface("Hidden", DSurface::Hidden);
		LogSurface("Composite", DSurface::Composite);
		LogSurface("Tile", DSurface::Tile);
		LogSurface("Sidebar", DSurface::Sidebar);
		LogSurface("Alternate", DSurface::Alternate);

		Debug::Log("[ViewZoom] Screen   %dx%d\n", signature.screenWidth, signature.screenHeight);
		Debug::Log("[ViewZoom] ViewBounds   X=%d Y=%d W=%d H=%d\n",
			view.X, view.Y, view.Width, view.Height);

		const auto& sidebar = DSurface::SidebarBounds;
		Debug::Log("[ViewZoom] SidebarBounds X=%d Y=%d W=%d H=%d\n",
			sidebar.X, sidebar.Y, sidebar.Width, sidebar.Height);

		if (DSurface::Composite)
		{
			const bool fits = view.X >= 0 && view.Y >= 0
				&& view.X + view.Width <= DSurface::Composite->Width
				&& view.Y + view.Height <= DSurface::Composite->Height;

			Debug::Log("[ViewZoom] ViewBounds fits Composite: %s (Composite %dx%d, %s)\n",
				fits ? "yes" : "no",
				DSurface::Composite->Width, DSurface::Composite->Height,
				view.Width == DSurface::Composite->Width
					&& view.Height == DSurface::Composite->Height
					? "same size" : "different size");
		}

		Debug::Log("[ViewZoom] Tactical %p  Display %p\n",
			static_cast<void*>(TacticalClass::Instance), reinterpret_cast<void*>(0x87F7E8u));
	}

	// ---- T6: per-segment frame timing (ViewZoom.Diagnostics) ----
	//
	// `render avg/peak` above is a single number for the whole window
	// 004F44CB..004F451B, which mixes the engine's three passes with our own
	// EndTacticalRender(). That number cannot say which of the two to attack, so
	// the same frames are sampled again per segment, in microseconds taken from
	// QueryPerformanceCounter (GetTickCount resolves to a whole millisecond,
	// which is the same order as the values being compared):
	//
	//   install   InstallWideRender()                  (004F44CB, before the frame)
	//   A         the engine's three passes            (frame start .. 004F451B)
	//   B         EndTacticalRender()                  (004F451B: downscale + restore)
	//   total     A + B
	//   C         the whole 004F44AF hook, and RestoreNativeFrame() inside it
	//
	// Frames that had to force a full redraw (ShouldForceFullRedraw, i.e.
	// ViewZoom.FullRedrawEvery) are accounted separately as well: they are the
	// p99 spike, and mixing them into the average hides it.
	constexpr int SegmentSamples = 4096;

	enum SampleChannel
	{
		SmpTotal,
		SmpA,
		SmpB,
		SmpCount
	};

	enum MeanChannel
	{
		MnPre,
		MnRestore,
		MnInstall,
		MnForcedA,
		MnForcedB,
		MnForcedTotal,
		MnClearA,
		MnClearB,
		MnClearTotal,
		MnCount
	};

	struct SegmentMean
	{
		unsigned long long Sum;
		unsigned int Count;
		unsigned int Max;
	};

	unsigned int SegSamples[SmpCount][SegmentSamples];
	unsigned int SegCounts[SmpCount];
	unsigned int SegOverflow = 0;
	SegmentMean SegMeans[MnCount];
	unsigned int SegForcedFrames = 0;

	// A frame is sampled between 004F44CB (after the install) and 004F451B
	// (after EndTacticalRender). 004F44AF runs first and hands the forced-redraw
	// flag over through PendingForcedFrame.
	unsigned long long QpcFrameStart = 0;
	unsigned long long QpcFrequency = 0;
	bool SegPending = false;
	bool SegFrameForced = false;
	bool PendingForcedFrame = false;

	inline unsigned long long QpcNow()
	{
		LARGE_INTEGER value;
		QueryPerformanceCounter(&value);

		return static_cast<unsigned long long>(value.QuadPart);
	}

	inline unsigned int QpcToMicroseconds(unsigned long long ticks)
	{
		if (!QpcFrequency)
		{
			LARGE_INTEGER frequency;
			QueryPerformanceFrequency(&frequency);

			QpcFrequency = frequency.QuadPart > 0
				? static_cast<unsigned long long>(frequency.QuadPart)
				: 1;
		}

		return static_cast<unsigned int>(ticks * 1000000ull / QpcFrequency);
	}

	inline void RecordSample(SampleChannel channel, unsigned long long ticks)
	{
		if (SegCounts[channel] < static_cast<unsigned int>(SegmentSamples))
			SegSamples[channel][SegCounts[channel]] = QpcToMicroseconds(ticks);
		else
			++SegOverflow;

		++SegCounts[channel];
	}

	inline void RecordMean(MeanChannel channel, unsigned long long ticks)
	{
		const unsigned int microseconds = QpcToMicroseconds(ticks);
		SegmentMean& mean = SegMeans[channel];

		mean.Sum += microseconds;
		++mean.Count;

		if (microseconds > mean.Max)
			mean.Max = microseconds;
	}

	// RestoreNativeFrame() sits inside the short-circuit chain of 004F44AF, so it
	// is wrapped rather than moved: the chain keeps its exact evaluation order.
	static bool TimeRestoreNativeFrame()
	{
		if (!Enabled())
			return ViewZoomClass::RestoreNativeFrame();

		const unsigned long long start = QpcNow();
		const bool restored = ViewZoomClass::RestoreNativeFrame();

		RecordMean(MnRestore, QpcNow() - start);

		return restored;
	}

	static unsigned int ChannelCount(SampleChannel channel)
	{
		const unsigned int count = SegCounts[channel];

		return count > static_cast<unsigned int>(SegmentSamples)
			? static_cast<unsigned int>(SegmentSamples)
			: count;
	}

	static void SortChannel(SampleChannel channel)
	{
		const unsigned int count = ChannelCount(channel);

		if (count > 1)
			std::sort(SegSamples[channel], SegSamples[channel] + count);
	}

	static unsigned int ChannelAt(SampleChannel channel, double fraction)
	{
		const unsigned int count = ChannelCount(channel);

		if (!count)
			return 0;

		unsigned int index = static_cast<unsigned int>(fraction * (count - 1) + 0.5);

		if (index >= count)
			index = count - 1;

		return SegSamples[channel][index];
	}

	static unsigned int ChannelAverage(MeanChannel channel)
	{
		const SegmentMean& mean = SegMeans[channel];

		return mean.Count ? static_cast<unsigned int>(mean.Sum / mean.Count) : 0;
	}

	static void ResetSegments()
	{
		for (int channel = 0; channel < SmpCount; ++channel)
			SegCounts[channel] = 0;

		for (int channel = 0; channel < MnCount; ++channel)
			SegMeans[channel] = SegmentMean { 0, 0, 0 };

		SegOverflow = 0;
		SegForcedFrames = 0;
		SegPending = false;
		SegFrameForced = false;
	}

	static void ReportSegments()
	{
		if (!SegCounts[SmpTotal])
		{
			ResetSegments();
			return;
		}

		for (int channel = 0; channel < SmpCount; ++channel)
			SortChannel(static_cast<SampleChannel>(channel));

		Debug::Log("[ViewZoom] seg us: frames %u (stored %u, overflow %u) | total p50 %u p95 %u p99 %u | A engine p50 %u p95 %u p99 %u\n",
			SegCounts[SmpTotal], ChannelCount(SmpTotal), SegOverflow,
			ChannelAt(SmpTotal, 0.50), ChannelAt(SmpTotal, 0.95), ChannelAt(SmpTotal, 0.99),
			ChannelAt(SmpA, 0.50), ChannelAt(SmpA, 0.95), ChannelAt(SmpA, 0.99));

		Debug::Log("[ViewZoom] seg us: B endrender p50 %u p95 %u p99 %u | C prehook avg %u max %u | C restore avg %u max %u | install avg %u max %u\n",
			ChannelAt(SmpB, 0.50), ChannelAt(SmpB, 0.95), ChannelAt(SmpB, 0.99),
			ChannelAverage(MnPre), SegMeans[MnPre].Max,
			ChannelAverage(MnRestore), SegMeans[MnRestore].Max,
			ChannelAverage(MnInstall), SegMeans[MnInstall].Max);

		Debug::Log("[ViewZoom] seg us: forced %u frames | without D: n %u A %u B %u total %u max %u | with D: n %u A %u B %u total %u max %u\n",
			SegForcedFrames,
			SegMeans[MnClearA].Count, ChannelAverage(MnClearA), ChannelAverage(MnClearB),
			ChannelAverage(MnClearTotal), SegMeans[MnClearTotal].Max,
			SegMeans[MnForcedA].Count, ChannelAverage(MnForcedA), ChannelAverage(MnForcedB),
			ChannelAverage(MnForcedTotal), SegMeans[MnForcedTotal].Max);

		ResetSegments();
	}

	static void ResetWindow()
	{
		ResetSegments();

		WindowStartTick = GetTickCount();
		WindowRenderTicks = 0;
		WindowPeakRenderTicks = 0;
		RenderPasses = 0;
		CellsInWindow = 0;
		CellsMaxPerPass = 0;
		DroppedCells = 0;
		PassDroppedCells = 0;
		DroppedMaxPerPass = 0;
		CellListPeak = 0;
		PassCellListPeak = 0;
		LastObservedCount = -1;
		CountResets = 0;
		SaturatedPasses = 0;
		ListClears = 0;
	}

	static void ReportWindow()
	{
		const unsigned int now = GetTickCount();
		const unsigned int elapsedMs = now - WindowStartTick;

		if (!RenderPasses || !elapsedMs)
		{
			ResetWindow();
			return;
		}

		Debug::Log("[ViewZoom] window: passes %u in %.1f s (%.0f/s), render avg %.2f ms peak %.2f ms, list clears %u\n",
			RenderPasses, elapsedMs / 1000.0, RenderPasses * 1000.0 / elapsedMs,
			static_cast<double>(WindowRenderTicks) / RenderPasses,
			static_cast<double>(WindowPeakRenderTicks), ListClears);

		Debug::Log("[ViewZoom] window: cells total %u, per pass avg %.2f max %u, list peak %d / %d, saturated passes %u, unexplained resets %u, dropped %u (max/pass %u)\n",
			CellsInWindow,
			static_cast<double>(CellsInWindow) / RenderPasses,
			CellsMaxPerPass, CellListPeak, VisibleCellCapacity,
			SaturatedPasses, CountResets, DroppedCells, DroppedMaxPerPass);

		ReportSegments();

		ResetWindow();
	}
}

// ScenarioClass::StartScenario (0x683AB0) -- a new match begins: skirmish,
// campaign mission, retry, next mission, or the CnCNet -SPAWN flow (cncnet5.dll
// calls this address directly). The zoom is per-match view state, so it goes back
// to the native level here; the covered `sub esp,0xEC` is executed normally
// because the handler returns 0, so nothing about the stack or the arguments is
// touched.
DEFINE_HOOK(0x683AB0, ViewZoom_ScenarioStart, 0x6)
{
	ViewZoomClass::ResetForNewMatch("scenario start");

	return 0;
}

// ScenarioClass::LoadGame (0x67E440) -- loading a saved game, or the "load"
// variant of the CnCNet -SPAWN flow. Same reset, same reasoning.
DEFINE_HOOK(0x67E440, ViewZoom_LoadGame, 0x6)
{
	ViewZoomClass::ResetForNewMatch("load game");

	return 0;
}

// The first of the three TacticalClass::Render passes. 004F44CB is only
// reached when this frame really runs the passes; the redraw-mode shortcut
// above jumps straight to 004F451B.
//
// This is also where the enlarged surface pair has to be installed: the engine
// presents the previous frame at 0x53BAE0, *before* this point, so installing
// any earlier would make that presentation read the enlarged surface.
DEFINE_HOOK(0x4F44CB, ViewZoom_PreTacticalPasses, 0x5)
{
	using namespace ViewZoomDiag;

	// T6: the install runs unconditionally, so it is timed around the call and
	// the clock is only read when the diagnostics ask for it.
	const bool timing = Enabled();
	const unsigned long long installStart = timing ? QpcNow() : 0;

	ViewZoomClass::InstallWideRender();

	if (timing)
		RecordMean(MnInstall, QpcNow() - installStart);

	if (!Enabled())
		return 0;

	RenderPending = true;
	RenderStartTick = GetTickCount();

	// T6: the frame sample starts here -- after the install, before the engine's
	// first pass -- so `A` is the three passes alone.
	SegPending = timing;
	SegFrameForced = PendingForcedFrame;
	QpcFrameStart = timing ? QpcNow() : 0;

	if (!WindowStartTick)
		WindowStartTick = RenderStartTick;

	if (DSurface::Composite && DSurface::Primary)
	{
		const Signature signature = CurrentSignature();

		if (!(signature == LastSignature))
		{
			LastSignature = signature;
			LogLayout(signature);
		}
	}

	return 0;
}

// 004F4502 calls the GScreenClass virtual at +0x40 (0x6D0A20), which in turn
// reaches the sidebar's redraw + present routine at 0x6D0E38. All of that runs
// between ViewZoom_PreTacticalPasses and ViewZoom_PostTacticalRender, i.e. while
// DSurface::ViewBounds still describes the enlarged viewport -- and the sidebar
// routine uses ViewBounds.Width to place the strip on the primary surface, so
// the strip would be blitted past the right edge of the window.
//
// Hand the native rectangle to that one call and restore the enlarged one
// afterwards; the tactical passes keep the enlarged viewport.
//
// The call itself must remain the engine's: 0x6D0E37 pushed the argument and the
// callee ends in `ret 4`, so it is the callee that pops it. Calling 0x6A6C30
// from here (the first version of this hook did) leaves that pushed argument on
// the stack, the epilogue at 0x6D0E3D then runs four bytes off, and the return
// address it pops is garbage -- an immediate crash.
//
// Returning 0 runs the covered bytes (here: the call) and resumes at
// hook + size, which is exactly 0x6D0E3D, where the second hook puts the
// enlarged rectangle back.
DEFINE_HOOK(0x6D0E38, ViewZoom_SidebarPresentNativeView, 0x5)
{
	ViewZoomClass::BeginSidebarNativeView();

	return 0;
}

// 0x6D0E3D: `pop edi; pop esi; pop ebp; pop ebx; add esp,0x3c` -- the sidebar
// routine has returned. Restore the enlarged rectangle for the rest of the
// frame and let the engine run its own epilogue.
DEFINE_HOOK(0x6D0E3D, ViewZoom_SidebarPresentRestoreView, 0x7)
{
	ViewZoomClass::EndSidebarNativeView();

	return 0;
}

// Every cell handed to the visible list passes here, whether it fits or not.
// EBX is the TacticalClass instance, [ebx+0xE0] the list length before insert.
DEFINE_HOOK(0x6DA938, ViewZoomDiag_RegisterCell, 0x6)
{
	// Every cell the passes will draw is queued here, so this is the one place
	// that reliably says the enlarged surface is about to change. It has to run
	// with the diagnostics off too, which is why it sits above the gate. The
	// queue does not carry a rectangle, so this only marks the frame dirty --
	// the partial resample still trusts the rectangles it was given.
	ViewZoomClass::NoteFrameCellEnqueued();

	using namespace ViewZoomDiag;

	if (!Enabled())
		return 0;

	GET(TacticalClass*, pTactical, EBX);

	if (!pTactical)
		return 0;

	++CellsThisPass;
	const int count = pTactical->VisibleCellCount;

	if (LastObservedCount >= 0 && count < LastObservedCount)
		++CountResets;

	LastObservedCount = count;

	// The cap check at 006DA93E rejects the insert once the list holds 799
	// entries, so every call from here on is an update the engine will discard
	// (and then also forget, because it clears the pending flag anyway).
	if (count >= VisibleCellCapacity - 1)
	{
		++DroppedCells;
		++PassDroppedCells;
	}

	if (count > CellListPeak)
		CellListPeak = count;

	if (count > PassCellListPeak)
		PassCellListPeak = count;

	return 0;
}

// The one place the visible cell list is dropped, at the end of a render pass.
DEFINE_HOOK(0x6D4571, ViewZoomDiag_ListClear, 0xA)
{
	using namespace ViewZoomDiag;

	if (!Enabled())
		return 0;

	++ListClears;
	LastObservedCount = -1;

	// One-shot probe: how many entries the list still held when the engine
	// dropped it. Anything appended before the passes is discarded here, which
	// is why the banded fill is fed again after this point.
	if (ViewZoomClass::Diagnostics && ClearProbeCount < 6)
	{
		GET(TacticalClass*, pTactical, EBP);

		if (pTactical)
		{
			++ClearProbeCount;

			Debug::Log("[ViewZoom] list clear #%u: had %d entries\n",
				ClearProbeCount, pTactical->VisibleCellCount);
		}
	}

	if (PassDroppedCells > DroppedMaxPerPass)
		DroppedMaxPerPass = PassDroppedCells;

	PassDroppedCells = 0;

	return 0;
}

// 004F4505: the visibility pass (mode 1) has just run and cleared the visible
// cell list; the last pass is about to draw. Cells appended by the banded fill
// are fed again here so they are still in the list when the draw happens.
DEFINE_HOOK(0x4F4505, ViewZoom_PreCellDraw, 0x6)
{
	// The UI step in between ran against the native pair; the object pass needs
	// the enlarged one back.
	ViewZoomClass::EndUiNativeStep();

	ViewZoomClass::InjectBandFill(0x200);

	return 0;
}

// 004F44F9..004F4504: the UI step (sidebar and bottom bar) between the second
// tactical pass and the object pass. It has to draw against the native
// surfaces, otherwise the bottom bar strip lands inside the enlarged map. The
// covered instructions are the argument setup and the engine's own
// `call [edx+40h`]; returning 0 lets the engine run them, so only the state is
// swapped here and restored at 004F4505.
DEFINE_HOOK(0x4F44F9, ViewZoom_UiStepNativeSurfaces, 0xC)
{
	ViewZoomClass::BeginUiNativeStep();

	return 0;
}

// 004F44AF: the orchestration has just read and cleared the tactical redraw
// mode into EAX. Restore the native frame before the dirty-area passes run, and
// force one complete pass whenever the backup cannot be restored (first zoomed
// frame, surface loss, or a changed view geometry).
DEFINE_HOOK(0x4F44AF, ViewZoom_PreTacticalRender, 0x5)
{
	enum
	{
		Continue = 0x4F44B4,
		FullRedrawMode = 2
	};

	using namespace ViewZoomDiag;

	// T6: time the whole pre hook, and RestoreNativeFrame() separately inside it.
	const bool timing = Enabled();
	const unsigned long long preStart = timing ? QpcNow() : 0;

	const bool wasActive = ViewZoomClass::Active;

	ViewZoomClass::BeginTacticalRender();

	// Evaluated exactly once per frame, whatever the other terms decide.
	const bool forcePeriodic = ViewZoomClass::ShouldForceFullRedraw();

	// One complete native pass is needed whenever the zoom starts or stops
	// (the surfaces still hold the other mode's pixels), whenever the magnify
	// backup cannot be restored -- first zoomed frame, surface loss, or a
	// changed view geometry -- and on the first frame of an enlarged pair.
	if (wasActive != ViewZoomClass::Active
		|| (ViewZoomClass::Active && !ViewZoomDiag::TimeRestoreNativeFrame())
		|| (ViewZoomClass::WantsWideView() && !ViewZoomClass::WideReady)
		|| forcePeriodic)
	{
		R->EAX(FullRedrawMode);
	}

	// T6: hand the forced-redraw flag to 004F44CB, which is where the frame
	// sample starts (this hook runs first).
	if (timing)
	{
		PendingForcedFrame = forcePeriodic;
		RecordMean(MnPre, QpcNow() - preStart);
	}

	// T1: EAX now holds the redraw mode the passes will run with -- the engine's
	// own value unless the branch above replaced it with FullRedrawMode. A
	// non-zero mode repaints the whole viewport, which no invalidation counter
	// reports, so the frame must not be skipped.
	ViewZoomClass::NoteFrameRedrawMode(R->EAX() != 0);

	// Reproduce 004F44AF..004F44B3: `test eax,eax` / `setne cl`. No EFLAGS may
	// be relied upon after a hook callback.
	R->ECX((R->ECX() & 0xFFFFFF00u) | (R->EAX() ? 1u : 0u));

	return Continue;
}

// 004F451B loads the sidebar redraw flag: the tactical passes are done, the
// sidebar and software cursor have not been composed yet. Back up the native
// view and scale it up in place here, then let the engine's own composition
// present the result.
DEFINE_HOOK(0x4F451B, ViewZoom_PostTacticalRender, 0x5)
{
	using namespace ViewZoomDiag;

	// T6: `A` ends here, at the point where the engine's passes are done and our
	// own EndTacticalRender() has not run yet; `B` is that call.
	const bool timing = Enabled() && SegPending;
	const unsigned long long postEntry = timing ? QpcNow() : 0;

	ViewZoomClass::EndTacticalRender();

	if (timing)
	{
		const unsigned long long postExit = QpcNow();

		RecordSample(SmpA, postEntry - QpcFrameStart);
		RecordSample(SmpB, postExit - postEntry);
		RecordSample(SmpTotal, postExit - QpcFrameStart);

		if (SegFrameForced)
		{
			++SegForcedFrames;
			RecordMean(MnForcedA, postEntry - QpcFrameStart);
			RecordMean(MnForcedB, postExit - postEntry);
			RecordMean(MnForcedTotal, postExit - QpcFrameStart);
		}
		else
		{
			RecordMean(MnClearA, postEntry - QpcFrameStart);
			RecordMean(MnClearB, postExit - postEntry);
			RecordMean(MnClearTotal, postExit - QpcFrameStart);
		}

		SegPending = false;
	}

	if (!Enabled())
		return 0;

	if (RenderPending)
	{
		const unsigned int elapsed = GetTickCount() - RenderStartTick;

		WindowRenderTicks += elapsed;

		if (elapsed > WindowPeakRenderTicks)
			WindowPeakRenderTicks = elapsed;

		++RenderPasses;
		RenderPending = false;
	}

	CellsInWindow += CellsThisPass;

	if (CellsThisPass > CellsMaxPerPass)
		CellsMaxPerPass = CellsThisPass;

	// A pass whose peak reached the cap is the candidate for silent truncation.
	if (PassCellListPeak >= VisibleCellCapacity)
		++SaturatedPasses;

	PassCellListPeak = 0;
	CellsThisPass = 0;

	if (GetTickCount() - WindowStartTick >= ReportIntervalMs)
		ReportWindow();

	return 0;
}

// Set_View_Dimensions -- the one runtime writer of DSurface::ViewBounds.
// ESI still points at the source RectangleStruct here.
DEFINE_HOOK(0x4A8987, ViewZoomDiag_SetViewDimensions, 0x6)
{
	using namespace ViewZoomDiag;

	if (!Enabled())
		return 0;

	GET(int*, pRect, ESI);

	if (pRect)
	{
		Debug::Log("[ViewZoom] Set_View_Dimensions(%d, %d, %d, %d) screen %dx%d\n",
			pRect[0], pRect[1], pRect[2], pRect[3],
			*reinterpret_cast<int*>(0x8A00A4u), *reinterpret_cast<int*>(0x8A00A8u));
	}

	return 0;
}

// The game's window procedure reaches 0077795A for every message above 0x205
// that is not the mouse wheel. The two replaced instructions are
//
//	8B C8              mov ecx,eax
//	81 E9 0A 02 00 00  sub ecx,20Ah
//
// i.e. exactly 8 bytes. Hook callbacks may not rely on EFLAGS, so both the ecx
// value and the continuation are reproduced explicitly. This hook never
// returns 0: 0077795A + 5 lands inside the second instruction.
//
// The middle button messages (0x207 down, 0x208 up, 0x209 double click) all
// pass through here. Nothing is consumed -- the engine's own input path still
// sees every message, this only observes the gesture.
DEFINE_HOOK(0x77795A, ViewZoom_WndProc_Message, 0x8)
{
	enum
	{
		MessageMouseWheel = 0x020A,
		ContinueNonWheel = 0x777964,
		ContinueWheel = 0x777985
	};

	const DWORD message = R->EAX();
	const DWORD packedPoint = R->Stack32(0x20);

	ViewZoomClass::OnWindowMessage(static_cast<unsigned int>(message), static_cast<unsigned int>(packedPoint));

	// Reimplement `mov ecx,eax` / `sub ecx,20Ah`.
	R->ECX(message - static_cast<DWORD>(MessageMouseWheel));

	return message == static_cast<DWORD>(MessageMouseWheel)
		? static_cast<DWORD>(ContinueWheel)
		: static_cast<DWORD>(ContinueNonWheel);
}

// ---------------------------------------------------------------------------
// Stage C: magnify (k < 1) rendering and input mapping.
//
// Composite still holds the native 1:1 render; the two hooks above turn it into
// the cropped, scaled image. The hooks below keep everything the engine does
// with that image consistent: the camera clamp range, clicks, the selection
// band, right-drag scrolling and tooltips.
//
// Every one of them reproduces the overridden instruction and returns an
// explicit continuation address, so the native flow is unchanged when the zoom
// is inactive.
// ---------------------------------------------------------------------------

// 006D864E loads DSurface::ViewBounds.Width into EBX for TacticalClass::
// ClampPosition, which uses it as the camera's half-range. While magnified only
// the smaller crop is visible, so the physical width stops edge scrolling early.
DEFINE_HOOK(0x6D864E, ViewZoom_ClampPosition_Width, 0x6)
{
	enum { Continue = 0x6D8654 };

	R->EBX(static_cast<DWORD>(ViewZoomClass::GetEffectiveViewWidth()));

	return Continue;
}

// 006D868A, the same for ViewBounds.Height.
DEFINE_HOOK(0x6D868A, ViewZoom_ClampPosition_Height, 0x6)
{
	enum { Continue = 0x6D8690 };

	R->EBX(static_cast<DWORD>(ViewZoomClass::GetEffectiveViewHeight()));

	return Continue;
}

// 00692325..0069232A: DisplayClass::ProcessClickCoords takes the source point
// (still at [esp+28h]) and loads it into EBP. Selection, movement, attack and
// action resolution all read that register, so remapping it here covers every
// command path at once. The raw mouse state stays native.
DEFINE_HOOK(0x692325, ViewZoom_ProcessClickCoords, 0x6)
{
	enum { Continue = 0x69232B };

	Point2D* pSource = reinterpret_cast<Point2D*>(R->Stack32(0x28));
	Point2D* pEffective = pSource;
	const Point2D before = pSource ? *pSource : Point2D { 0, 0 };

	// The engine un-projects this point against view_bound and TacticalPos, so
	// both have to describe the enlarged viewport while the click is converted.
	//
	// The mapping has to be computed *first*. TransformViewRelativePoint() works
	// off DSurface::ViewBounds (native outside the render window) and never reads
	// view_bound, so its verdict already tells us whether this point really is on
	// the map. Widening before that verdict is what let a sidebar click - the
	// super weapon cameo - masquerade as a valid map point: the engine's
	// ClientToCoords no longer took its "outside the view" fallback branch, so
	// the targeting code treated the widened cell as a chosen target and fired
	// at once, at whatever cell the enlarged geometry happened to produce (top
	// right of the map). A point outside the native view must stay native, so
	// the engine's own rejection still applies.
	const bool applied = pSource
		&& ViewZoomClass::TransformViewRelativePoint(*pSource, ViewZoomClass::AdjustedInput);

	if (applied)
		ViewZoomClass::BeginInputViewport();

	ViewZoomClass::TraceInput("click", before, ViewZoomClass::AdjustedInput, applied);

	if (applied)
		pEffective = &ViewZoomClass::AdjustedInput;

	// Reproduce `mov ebp,[esp+28h]` / `mov [edx],eax`.
	R->EBP(reinterpret_cast<DWORD>(pEffective));

	if (R->EDX())
		*reinterpret_cast<DWORD*>(R->EDX()) = R->EAX();

	return Continue;
}

// 006D9F80: TacticalClass stores the selection band's start point. Store it in
// logical (native render) coordinates so the band is drawn before the
// enlargement and lands under the physical cursor.
DEFINE_HOOK(0x6D9F80, ViewZoom_DragStart, 0x6)
{
	enum { Continue = 0x6D9F86 };

	Point2D* pSource = reinterpret_cast<Point2D*>(R->Stack32(4));
	const Point2D before = pSource ? *pSource : Point2D { 0, 0 };

	const bool applied = pSource
		&& ViewZoomClass::TransformViewRelativePoint(*pSource, ViewZoomClass::AdjustedDragStart);

	ViewZoomClass::TraceInput("dragSt", before, ViewZoomClass::AdjustedDragStart, applied);

	if (applied)
		R->Stack(4, reinterpret_cast<DWORD>(&ViewZoomClass::AdjustedDragStart));

	// Reproduce `mov eax,[ecx+0D90h]`.
	R->EAX(R->ECX() ? *reinterpret_cast<DWORD*>(R->ECX() + 0xD90u) : 0u);

	return Continue;
}

// 006D9FC0, the same for the band's end point.
DEFINE_HOOK(0x6D9FC0, ViewZoom_DragEnd, 0x6)
{
	enum { Continue = 0x6D9FC6 };

	Point2D* pSource = reinterpret_cast<Point2D*>(R->Stack32(4));
	const Point2D before = pSource ? *pSource : Point2D { 0, 0 };

	const bool applied = pSource
		&& ViewZoomClass::TransformViewRelativePoint(*pSource, ViewZoomClass::AdjustedDragEnd);

	ViewZoomClass::TraceInput("dragEnd", before, ViewZoomClass::AdjustedDragEnd, applied);

	if (applied)
		R->Stack(4, reinterpret_cast<DWORD>(&ViewZoomClass::AdjustedDragEnd));

	// Reproduce `mov eax,[ecx+0D90h]`.
	R->EAX(R->ECX() ? *reinterpret_cast<DWORD*>(R->ECX() + 0xD90u) : 0u);

	return Continue;
}

// 00693791..00693796: the right-drag displacement has already been turned into
// the two map-scroll magnitudes at [esp+18h]/[esp+1Ch]. Scale only those final
// magnitudes while magnified, otherwise the map moves faster than the cursor.
DEFINE_HOOK(0x693791, ViewZoom_RightDragSpeed, 0x6)
{
	enum
	{
		Continue = 0x693797,
		RightDraggingOffset = 0x5558
	};

	if (ViewZoomClass::IsZoomed() && R->EBX()
		&& *reinterpret_cast<unsigned char*>(R->EBX() + RightDraggingOffset))
	{
		int* pHorizontal = reinterpret_cast<int*>(R->ESP() + 0x18u);
		int* pVertical = reinterpret_cast<int*>(R->ESP() + 0x1Cu);

		if (*pHorizontal > 0)
			*pHorizontal = ViewZoomClass::ScaleMapDistanceX(*pHorizontal);

		if (*pVertical > 0)
			*pVertical = ViewZoomClass::ScaleMapDistanceY(*pVertical);
	}

	// Reproduce `xor esi,esi` / `mov [esp+28h],esi`.
	R->ESI(0);
	R->Stack(0x28, static_cast<DWORD>(0));

	return Continue;
}

// 006A7089..006A708F: a full tooltip redraw while the sidebar is drawn. A
// tooltip whose rectangle lies inside the tactical view would be baked into the
// enlarged composite and then drawn a second time at screen size afterwards, so
// suppress that pass only for those. Everything else keeps the native call.
DEFINE_HOOK(0x6A7089, ViewZoom_ToolTipFullRedraw, 0x7)
{
	enum { Continue = 0x6A7090 };

	void* pToolTip = reinterpret_cast<void*>(R->ECX());

	if (pToolTip && !ViewZoomClass::TooltipIntersectsView(pToolTip))
	{
		void** pVtable = *reinterpret_cast<void***>(pToolTip);

		if (pVtable)
		{
			using TooltipDraw_t = void(__thiscall*)(void*, int);
			TooltipDraw_t draw = reinterpret_cast<TooltipDraw_t>(pVtable[0x0Cu / 4u]);

			if (draw)
				draw(pToolTip, 1);
		}
	}

	return Continue;
}

// 006D2790: TacticalClass::RegisterDirtyArea(RectangleStruct Area, bool).
//
// Every invalidation the engine produces for the map -- ore being harvested
// (CellClass::ReduceTiberium at 00480A80, site 00480C8F), a cell being marked
// for redraw (00486E66), object and building render rectangles, terrain
// changes -- arrives here as a *viewport-relative* rectangle, and the eight
// per-cell layer passes convert it back to screen space with the origin that is
// live when they consume it:
//
//     006D6D39: mov ebp,[edi+0b0h]   ; TacticalPos.X
//     006D6D45: add eax,ebp          ; screenX = rect.X + TacticalPos.X - ViewBounds.X
//
// The enlarged viewport only exists between 004F44CB and 004F451B, and none of
// these producers runs from the render passes, so while zoomed out they measure
// against the native origin (camera - native / 2) while the layers reinterpret
// against the enlarged one (camera - big / 2). The rectangle is short by
// (big - native) / 2 on each axis, the repaint lands half the growth away from
// the cell that changed, and that cell keeps the previous frame's pixels from
// the scroll copy at 006D3FCF forever -- the observed "the ore stays on the map
// until the view is scrolled away and back".
//
// The engine's own argument copy is edited in place; the callee reads its
// parameter from that stack slot and the caller is `ret 14h`, so nothing else
// depends on the value.
DEFINE_HOOK(0x6D2790, ViewZoom_RegisterDirtyArea_Translate, 0x9)
{
	RectangleStruct* pArea = reinterpret_cast<RectangleStruct*>(R->ESP() + 0x4u);

	if (pArea)
		ViewZoomClass::TranslateDirtyArea(*pArea);

	return 0;
}

// 006DA7D0: TacticalClass::RegisterCellAsVisible(CellClass*).
//
// The only writer of the visible-cell queue in the whole binary is 006DA949,
// inside this function, and all 25 of its call sites are game-logic entry
// points (fog/shroud reveal, building destruction, cell occlusion recompute) --
// none is reachable from TacticalClass::Render. The queue is what the per-cell
// terrain and overlay layers draw from, and CellClass::DrawOverlay (0047F6A0)
// is called from nothing but those layers, so a cell that never enters the
// queue simply keeps its old pixels.
//
// The function culls the cell against DSurface::ViewBounds with TacticalPos as
// the origin (006DA901-006DA936). Outside the render window that is the native
// rectangle, so every cell the enlarged view added is rejected while zoomed
// out. The callback enqueues those cells itself with the enlarged window, then
// stamps the cell's redraw frame -- the engine's own frame-stamp check at
// 006DA7F1 makes its copy a no-op, so there is no double insert and no need to
// skip any instruction.
DEFINE_HOOK(0x6DA7D0, ViewZoom_RegisterCellAsVisible_Wide, 0x5)
{
	void* pCell = reinterpret_cast<void*>(R->Stack32(0x4u));

	if (pCell)
		ViewZoomClass::TryWideCellEnqueue(pCell);

	return 0;
}


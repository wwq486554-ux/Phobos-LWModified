#pragma once

#include <Phobos.h>
#include <BasicStructures.h>
#include <GeneralStructures.h>

class CCINIClass;
class DSurface;

// Tactical view zoom.
//
// Stage B is the input side: a level table, the three commands and the middle
// button.
//
// Stage C has two rendering directions, both built around the same pair of
// hooks around the tactical passes (0x4F44AF before, 0x4F451B after):
//
//   k < 1, magnify -- the engine renders the native view 1:1 into
//     DSurface::Composite, and we crop a centred "view * k" region out of it
//     and scale that region back up to the full view, in place. The engine's
//     own composition then presents the result, so no drawing code is touched.
//
//   k > 1, wider view -- the engine has to draw *more* cells, which means a
//     bigger viewport and, because it double buffers Composite and Tile (the
//     globals are swapped inside TacticalClass::Render at 0x6D405D), a bigger
//     surface *pair*. While such a level is active the two globals are pointed
//     at our own pair, ViewBounds and view_bound are enlarged for the passes,
//     and the finished image is scaled back down into the real Composite
//     before the engine composes it.
//
// Both directions keep the engine's incremental (dirty cell) redraw working:
// the tactical surfaces stay the persistent render targets of the frame, they
// are merely presented through a different rectangle.
class ViewZoomClass
{
public:
	static constexpr int MaxLevels = 16;
	static constexpr double NativeFactor = 1.0;

	// [Phobos] ViewZoom.* in the game's own ini.
	static bool Enabled;
	static bool MiddleClickReset;
	static bool ShowHint;
	static bool WideView;            // k > 1 support; off by default
	static bool WholeMap;            // append a whole-map level to the table
	static bool DebugFill;           // paint the native view instead of the frame
	static bool Diagnostics;
	static bool NoBandFill;    // A/B switch: skip the banded fill
	static bool SidebarNativeView;   // A/B switch: sidebar present sees the native view

	static int LevelCount;
	static int CurrentLevel;
	static double Levels[MaxLevels];
	static bool WholeMapLevels[MaxLevels];

	// ---- magnify state (k < 1) ----
	static bool Active;              // a magnified crop is being presented
	static bool VisualReady;         // SourceRect and the scratch are current
	static bool NativeFrameValid;    // the scratch holds the native view image
	static RectangleStruct SourceRect;
	static RectangleStruct NativeView;
	static unsigned short* Scratch;
	static unsigned int ScratchPixels;

	// ---- wider view state (k > 1) ----
	static bool WideReady;           // the enlarged pair produced a valid frame
	static DSurface* pBigComposite;  // enlarged pair, role matching Composite
	static DSurface* pBigTile;       // enlarged pair, role matching Tile
	static int BigWidth;
	static int BigHeight;

	// Where the enlarged frame is placed inside the native view. The whole-map
	// level renders a 2:1 map bounds rectangle, which cannot fill the 1.53:1
	// native map area without distorting it, so the frame is fitted uniformly
	// and centred (letterbox); for the plain k > 1 levels this is the whole
	// native view rectangle and the code below degenerates to the old mapping.
	static RectangleStruct WideDestRect;

	// Points handed to the engine have to outlive the hook that produced them.
	static Point2D AdjustedInput;
	static Point2D AdjustedDragStart;
	static Point2D AdjustedDragEnd;

	static void LoadFromINI(CCINIClass* pINI, const char* pSection);

	static double GetFactor();
	static int GetNativeLevel();
	static bool IsZoomed();
	static double GetViewScale();

	static bool IsWideLevel(int level);
	static bool IsWholeMapLevel(int level);

	// Pixel bounds of the loaded map (screen space, 1:1 cell size). The whole
	// map level renders exactly this rectangle.
	static bool GetMapPixelBounds(RectangleStruct& bounds);

	// The rectangle the engine renders into for the current level: the native
	// view, the native view scaled by k, or the whole map bounds.
	static bool GetRenderViewport(const RectangleStruct& nativeView, RectangleStruct& viewport);

	// Uniform, centred fit of a width x height frame inside a native view.
	static bool CalculateWideDestination(const RectangleStruct& view, int width, int height,
		RectangleStruct& dest);

	// direction < 0 zooms in (smaller k), > 0 zooms out. Returns false when the
	// feature is disabled or the first/last level is already selected.
	static bool Step(int direction);
	static bool ResetToNative(const char* pAction);

	// Called for every window message from the game's own window procedure;
	// only the middle button messages matter here.
	static void OnWindowMessage(unsigned int message, unsigned int packedPoint);

	// ---- rendering entry points, called from the hooks ----
	static void SynchronizeState();
	static bool RestoreNativeFrame();        // magnify, before the passes
	static bool ApplyToComposite();          // magnify, after the passes
	static void BeginTacticalRender();       // frame start, before the previous frame is presented
	static void InstallWideRender();         // right before the passes, after the presentation
	static void EndTacticalRender();         // after the passes

	// The sidebar's own redraw + present routine (0x6A6C30, called from
	// 0x6D0E38) runs inside the same window as the tactical passes and places
	// the strip on the primary surface at `client + DSurface::ViewBounds.Width`.
	// It therefore has to see the *native* viewport even while the enlarged one
	// is installed, otherwise the whole sidebar is blitted past the right edge of
	// the window and simply stops updating on screen.
	//
	// The engine's own `call 0x6A6C30` must stay in place: 0x6D0E37 pushes the
	// argument and the callee is `ret 4`, so calling the routine from here would
	// leave that argument on the stack and the epilogue at 0x6D0E3D would run
	// four bytes off -- a garbage return address and an instant crash. The swap
	// is therefore split around the engine's call: 0x6D0E38 begins it, 0x6D0E3D
	// (the instruction right after the call) ends it.
	static void BeginSidebarNativeView();
	static void EndSidebarNativeView();

	// A match always starts from the native level. The zoom is per-match view
	// state and must never be carried into the next one, so every entry point
	// that starts one (see the hooks on ScenarioClass::StartScenario and
	// ScenarioClass::LoadGame) resets here first.
	static void ResetForNewMatch(const char* pAction);
	static bool WantsWideView();
	static bool IsWideInstalled() { return WideInstalled; }
	static void RequestFullRedraw();

	static int GetEffectiveViewWidth();
	static int GetEffectiveViewHeight();
	static int ScaleMapDistanceX(int distance);
	static int ScaleMapDistanceY(int distance);
	static bool TransformViewRelativePoint(const Point2D& input, Point2D& output);
	static bool TransformClientPoint(const Point2D& input, Point2D& output);
	static bool TooltipIntersectsView(const void* pToolTip);
	static bool CalculateSourceRect(const RectangleStruct& view, RectangleStruct& source);

private:
	static bool MiddleButtonDown;
	static unsigned int MiddleButtonDownPoint;

	// Native viewport size, sampled at every frame start (the view is always
	// native there). Used to keep the clamp helpers idempotent.
	static RectangleStruct NativeViewSize;

	// native surfaces saved for the duration of one render
	static DSurface* pSavedComposite;
	static DSurface* pSavedTile;
	static DSurface* pSavedTemp;
	static RectangleStruct SavedViewBounds;
	static int SavedViewBound[4];
	static bool WideInstalled;

	// The sidebar present runs with the native viewport for exactly one call;
	// these hold what has to be put back (see BeginSidebarNativeView).
	static bool SidebarViewActive;
	static RectangleStruct SidebarViewSaved;

	// Two engine scratch buffers (0x887644, 0x87E8A4) are `view * 2` bytes each
	// and are only rebuilt by the engine's own view routine. They are rebuilt
	// here at the enlarged size and swapped in for the render window, otherwise
	// the renderer writes a viewport's worth of pixels into a native-sized
	// buffer and corrupts the heap.
	static void* pBigShadowBuffer;
	static void* pBigVisibilityBuffer;
	static void* pSavedShadowBuffer;
	static void* pSavedVisibilityBuffer;
	static int BigBufferWidth;
	static int BigBufferHeight;
	static bool BuffersInstalled;

	static bool EnsureWideViewBuffers(const RectangleStruct& rect);
	static void ReleaseWideViewBuffers();
	static bool InstallWideViewBuffers();
	static void RestoreWideViewBuffers();

	// The engine draws only the cells it was told changed, so an enlarged
	// viewport is never painted in full on its own. These push the map through
	// the engine's own visible-cell queue a strip per frame until every cell has
	// been drawn once.
	static bool BandFillActive;
	static int BandCursorIndex;
	static int BandFrames;
	static int BandPushed;
	static int BandLastCameraX;
	static int BandLastCameraY;

	static void RestartBandFill();
	static void AdvanceBandFill(int budget);
	static void RefreshBandFillCamera(int cameraX, int cameraY);

public:
	// Called from the hook between the visibility pass (which clears the list)
	// and the last pass, where the cells are actually drawn.
	static void InjectBandFill(int budget);

	// Diagnostic trace of the input remapping (ViewZoom.Diagnostics).
	static void TraceInput(const char* pTag, const Point2D& input, const Point2D& output, bool applied);

	// The engine un-projects a click with the point plus TacticalPos and rejects
	// any point outside view_bound, so for the duration of a click the viewport
	// has to describe the enlarged view.
	static void BeginInputViewport();
	static void EndInputViewport();

private:
	static int TracePreviousOrigin[2];
	static int TraceCamera[2];
	static Point2D LastTraceInput;
	static Point2D LastTraceOutput;
	static double LastTraceFactor;
	static bool InputViewportInstalled;
	static int SavedInputViewBound[4];
	static int SavedInputTacticalPos[2];
	static int InputTraceCount;

	static void ReportLevel(const char* pAction);
	static void OnLevelChanged();
	static bool EnsureBigSurfaces(int width, int height);
	static void ReleaseBigSurfaces();
	static DSurface* CreateSurface(int width, int height);
	static void DestroySurface(DSurface* pSurface);
	static bool FillNativeView(DSurface* pSurface);
	static bool SeedBigSurfaces(DSurface* pSource);
	static bool DownscaleToNative(DSurface* pSource, DSurface* pDestination);
};

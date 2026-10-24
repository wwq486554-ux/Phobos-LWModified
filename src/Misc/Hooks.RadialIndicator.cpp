#include <Phobos.h>

#include <Helpers/Macro.h>
#include <Surface.h>
#include <Utilities/Debug.h>

#include <algorithm>
#include <cmath>

/*
 * Radial indicator -- large range drawing.
 *
 * Game::DrawRadialIndicator (0x456980) calls XSurface::DrawEllipseOutline
 * (vtable slot 8, 0x7BB350) twice to paint the two-pixel ring:
 *
 *   0x456B57  call [eax+0x20]   outer ring, radii (radius_w, radius_h)
 *   0x456BEB  call [edx+0x20]   inner ring, radii (radius_w - 1, radius_h - 1)
 *
 * The rasterizer is built on 32-bit signed integer discriminants. Its leading
 * term is 2 * radius_w^2 * radius_h (and radius_h == radius_w / 2, so that is
 * radius_w^3): once the pixel radius gets large the comparisons overflow and
 * the whole circle disappears instead of being drawn incorrectly. The pixel
 * radius is derived from the cell range as radius_w = round(60 * (Range + 0.5)
 * / sqrt(2)), about 42.43 * Range, which puts the first overflow at Range = 30
 * (Range = 29 is the last safe value).
 *
 * Both call sites are hooked. While the cubic term stays below a conservative
 * limit the stolen call is replayed and the engine draws the ring exactly as
 * before; beyond it the outline is drawn here with double precision scan lines
 * (cost depends on the visible area, not on the radius). Everything else in
 * the drawing routine -- the range, the colour and the visibility decisions as
 * well as the rotating short-line pass after the second call -- is untouched.
 */

namespace
{
	// INT32_MAX is about 2.147e9; keep the engine rasterizer at ~75% of that so
	// that no intermediate discriminant can overflow. radius_w = 1167 (Range 27)
	// gives 1.589e9, radius_w = 1209 (Range 28) gives 1.766e9: the takeover
	// therefore starts at Range 28, one cell before the theoretical overflow.
	constexpr double VanillaCubicLimit = 1.6e9;

	// Both sites push the same nine dwords -- centre, both radii, the clip rect
	// by value and the colour -- and the callee pops them with `ret 0x24`.
	constexpr int PushedArgumentsSize = 0x24;

	bool DrawEllipseOutlineLarge(DSurface* pSurface, int centerX, int centerY,
		int radiusW, int radiusH, RectangleStruct const& clipRect, unsigned short color)
	{
		if (!pSurface || radiusW <= 0 || radiusH <= 0)
			return false;

		// The engine rasterizer writes 16bpp words unconditionally; anything else
		// is not worth guessing at, so fall back to it.
		if (pSurface->GetBytesPerPixel() != 2)
			return false;

		// Clip to the intersection of the engine clip rectangle and the surface.
		int const left = std::max(clipRect.X, 0);
		int const top = std::max(clipRect.Y, 0);
		int const right = std::min(clipRect.X + clipRect.Width, pSurface->Width);
		int const bottom = std::min(clipRect.Y + clipRect.Height, pSurface->Height);

		if (left >= right || top >= bottom)
			return false;

		int const pitch = pSurface->GetPitch();

		if (pitch <= 0)
			return false;

		unsigned char* const pPixels = static_cast<unsigned char*>(pSurface->Lock(0, 0));

		if (!pPixels)
			return false;

		auto const putPixel = [&](int x, int y)
		{
			if (x >= left && x < right && y >= top && y < bottom)
				*reinterpret_cast<unsigned short*>(pPixels + y * pitch + x * 2) = color;
		};

		// Sweep the rows for the shallow parts of the outline...
		int const rowFirst = std::max(top, centerY - radiusH);
		int const rowLast = std::min(bottom, centerY + radiusH + 1);

		for (int y = rowFirst; y < rowLast; ++y)
		{
			double const offset = static_cast<double>(y - centerY) / radiusH;
			double const remaining = std::max(0.0, 1.0 - offset * offset);
			int const dx = static_cast<int>(std::lround(radiusW * std::sqrt(remaining)));

			putPixel(centerX + dx, y);
			putPixel(centerX - dx, y);
		}

		// ... and the columns for the steep parts. Both sweeps meet, and overlap
		// harmlessly, around the 45 degree regions.
		int const columnFirst = std::max(left, centerX - radiusW);
		int const columnLast = std::min(right, centerX + radiusW + 1);

		for (int x = columnFirst; x < columnLast; ++x)
		{
			double const offset = static_cast<double>(x - centerX) / radiusW;
			double const remaining = std::max(0.0, 1.0 - offset * offset);
			int const dy = static_cast<int>(std::lround(radiusH * std::sqrt(remaining)));

			putPixel(x, centerY + dy);
			putPixel(x, centerY - dy);
		}

		pSurface->Unlock();
		return true;
	}
}

DEFINE_HOOK_AGAIN(0x456BEB, Game_DrawRadialIndicator_LargeEllipse, 7)
DEFINE_HOOK(0x456B57, Game_DrawRadialIndicator_LargeEllipse, 8)
{
	enum { ContinueOuter = 0x456B5F, ContinueInner = 0x456BF2 };

	GET(DSurface*, pSurface, ECX);

	int const centerX = static_cast<int>(R->Stack32(0x00));
	int const centerY = static_cast<int>(R->Stack32(0x04));
	int const radiusW = static_cast<int>(R->Stack32(0x08));
	int const radiusH = static_cast<int>(R->Stack32(0x0C));

	// Read the local flag byte while the argument block is still on the stack:
	// after the second stolen window the engine executes `test al, al` and the
	// stolen `mov al, [esp+0xF]` is what would have loaded it.
	unsigned char const innerFlag = R->Stack8(0x0F);

	if (radiusW <= 0 || radiusH <= 0
		|| 2.0 * radiusW * radiusW * radiusH <= VanillaCubicLimit)
	{
		return 0;
	}

	RectangleStruct const clipRect {
		static_cast<int>(R->Stack32(0x10)), static_cast<int>(R->Stack32(0x14)),
		static_cast<int>(R->Stack32(0x18)), static_cast<int>(R->Stack32(0x1C)) };

	if (!DrawEllipseOutlineLarge(pSurface, centerX, centerY, radiusW, radiusH, clipRect,
		static_cast<unsigned short>(R->Stack32(0x20))))
	{
		// Could not draw (no surface, unexpected pixel format, lock failure) --
		// let the engine do whatever it would have done.
		return 0;
	}

	// The engine's call is skipped, so its stack arguments are not popped by the
	// callee anymore. Unwind them here before resuming after the stolen window;
	// SyringeEx's ESPModification is what makes this legal.
	R->ESP(R->ESP() + PushedArgumentsSize);

	if (R->Origin() == 0x456B57)
	{
		// Resumes at `mov edx, [eax]`; the replayed `mov eax, ds:0x887314`
		// would have loaded the surface into EAX.
		R->EAX(R->ECX());
		return ContinueOuter;
	}

	R->EAX(static_cast<DWORD>(innerFlag));
	return ContinueInner;
}

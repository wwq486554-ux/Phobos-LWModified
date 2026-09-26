#pragma once

#include <GeneralStructures.h>

/*
	EngraveLine

	Shared line-segment math extracted from EngraveTrajectory so that the same
	"virtual line" calculations can be reused by the SweepFire weapon logic.

	An engrave-style line is described by two offsets in virtual space, relative
	to an anchor coordinate (normally the aim target):

	  - the source offset (Trajectory.Engrave.SourceCoord),
	  - the target offset (Trajectory.Engrave.TargetCoord).

	Both offsets are rotated by the firer -> anchor direction (RotateRadian) and
	may be mirrored along Y for the alternating Burst shots. Resolving them
	yields the world-space source/target of the line that the projectile (or the
	sweep aim point) travels along.

	Every function is a stateless calculation over its arguments.
*/
class EngraveLine final
{
public:
	EngraveLine() = delete;

	// Orientation of the line: the radian of the source -> target direction.
	static double GetRotateRadian(const CoordStruct& source, const CoordStruct& target);

	// Mirrors the virtual offsets along Y. Used for the second Burst shot,
	// which fires from the mirrored muzzle and sweeps the other way.
	static void MirrorVirtualCoord(Point2D& virtualSource, Point2D& virtualTarget);

	// Whether a virtual offset actually displaces the point from its anchor.
	static bool HasOffset(const Point2D& virtualCoord);

	// Anchor plus the rotated virtual offset. The CoordStruct overload keeps the
	// anchor's Z (the offset itself is horizontal); the Point2D overload is the
	// horizontal-only variant used by the movement math.
	static CoordStruct AddVirtualOffset(const CoordStruct& anchor, const Point2D& virtualCoord, double rotateRadian);
	static Point2D AddVirtualOffset(const Point2D& anchor, const Point2D& virtualCoord, double rotateRadian);

	// Ground or bridge height at coord. sourceZ / targetZ are the projectile's
	// original source and target heights: when either is at or above the bridge
	// surface the bridge height is preferred over the ground height.
	static int GetFloorCoordHeight(const CoordStruct& coord, int sourceZ, int targetZ);

	// Point on the horizontal line at `distance` leptons from its start, keeping
	// the given Z. Returns false for a degenerate line (start == end); `coord` is
	// left untouched in that case.
	static bool GetCoordAtDistance(const Point2D& source, const Point2D& target, double distance, int z, CoordStruct& coord);
};

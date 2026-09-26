#include "EngraveLine.h"

#include <CellClass.h>
#include <MapClass.h>

#include <Ext/Bullet/Body.h>

double EngraveLine::GetRotateRadian(const CoordStruct& source, const CoordStruct& target)
{
	return BulletExt::Get2DOpRadian(source, target);
}

void EngraveLine::MirrorVirtualCoord(Point2D& virtualSource, Point2D& virtualTarget)
{
	virtualSource.Y = -virtualSource.Y;
	virtualTarget.Y = -virtualTarget.Y;
}

bool EngraveLine::HasOffset(const Point2D& virtualCoord)
{
	return virtualCoord.X != 0 || virtualCoord.Y != 0;
}

CoordStruct EngraveLine::AddVirtualOffset(const CoordStruct& anchor, const Point2D& virtualCoord, double rotateRadian)
{
	return anchor + BulletExt::Point2Coord(BulletExt::PointRotate(virtualCoord, rotateRadian));
}

Point2D EngraveLine::AddVirtualOffset(const Point2D& anchor, const Point2D& virtualCoord, double rotateRadian)
{
	return anchor + BulletExt::PointRotate(virtualCoord, rotateRadian);
}

int EngraveLine::GetFloorCoordHeight(const CoordStruct& coord, int sourceZ, int targetZ)
{
	const int onFloor = MapClass::Instance.GetCellFloorHeight(coord);
	const int onBridge = MapClass::Instance.GetCellAt(coord)->ContainsBridge() ? onFloor + CellClass::BridgeHeight : onFloor;

	// Take the higher position
	return (sourceZ >= onBridge || targetZ >= onBridge) ? onBridge : onFloor;
}

bool EngraveLine::GetCoordAtDistance(const Point2D& source, const Point2D& target, double distance, int z, CoordStruct& coord)
{
	const auto delta = target - source;
	const double distanceSq = delta.MagnitudeSquared();

	if (distanceSq < BulletExt::EpsilonSquared)
		return false;

	coord = BulletExt::Point2Coord(source + delta * (distance / sqrt(distanceSq)), z);
	return true;
}

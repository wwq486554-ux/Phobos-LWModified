#pragma once
#include <BulletClass.h>

#include <cmath>

#include <Ext/BulletType/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/WeaponType/Body.h>
#include <New/Entity/LaserTrailClass.h>
#include <Ext/Object/Body.h>

#include <Ext/Bullet/Trajectories/PhobosTrajectory.h>

struct BulletGroupData
{
	std::vector<DWORD> Bullets {}; // <UniqueID>, Capacity
	double Angle { 0.0 }; // Tracing.StableRotation use this value to update the angle
	bool ShouldUpdate { true }; // Remind members to update themselves

	BulletGroupData() = default;

	bool Load(PhobosStreamReader& stm, bool registerForChange);
	bool Save(PhobosStreamWriter& stm) const;

private:
	template <typename T>
	bool Serialize(T& stm);
};

struct RadialFireStruct
{
	int Segments = 0;
	int Index = 0;
	DirStruct Direction {};
};

class BulletExt final : public ObjectExt
{
public:
	using base_type = BulletClass;

	// deprecated: the pre-rework nested data class is now the extension class itself
	using ExtData [[deprecated("use the extension class itself instead")]] = BulletExt;

	static constexpr DWORD Canary = 0x2A2A2A2A;

public:
	// typed owner accessor
	BulletClass* OwnerObject() const
	{
		return static_cast<BulletClass*>(this->GetAttachedObject());
	}

	BulletTypeExt* TypeExtData;
	HouseClass* FirerHouse;
	int CurrentStrength;
	TechnoTypeExt* InterceptorTechnoType;
	InterceptedStatus InterceptedStatus;
	bool DetonateOnInterception;
	std::vector<std::unique_ptr<LaserTrailClass>> LaserTrails;
	bool SnappedToTarget; // Used for custom trajectory projectile target snap checks
	int DamageNumberOffset;
	int ParabombFallRate;
	bool IsInstantDetonation;
	double FirepowerMult;
	bool IsSplitFromAirburst;
	int DistanceTraveled;

	TrajectoryPointer Trajectory;
	bool DispersedTrajectory;
	// Set by SweepFire on a shot whose aim point is an exact coordinate rather than
	// a cell centre. It makes the drawing helpers use that coordinate, and it
	// selects the exact-distance detonation instead of the vanilla cell-based one.
	// Because it affects the whole flight it is serialized with the bullet.
	bool SweepFireAim;
	// Diagnostic only (transient, not serialized): the point this sweep shot's launch was
	// solved for, i.e. its aim point with the projectile's scatter applied. Logged next to the
	// detonation coordinate so a shot's scatter can be traced all the way to its impact.
	CoordStruct SweepFireLaunchAim;
	CDTimerClass LifeDurationTimer;
	CDTimerClass NoTargetLifeTimer;
	CDTimerClass RetargetTimer;
	int AttenuationRange;
	bool TargetIsInAir;
	bool TargetIsTechno;
	bool NotMainWeapon;
	TrajectoryStatus Status;
	CoordStruct FLHCoord;
	std::shared_ptr<PhobosMap<BulletTypeClass*, BulletGroupData>> TrajectoryGroup;
	int GroupIndex;
	int PassDetonateDamage;
	CDTimerClass PassDetonateTimer;
	int ProximityImpact;
	int ProximityDamage;
	TechnoClass* ExtraCheck;
	std::map<DWORD, int> Casualty;
	int DisperseIndex;
	int DisperseCount;
	int DisperseCycle;
	CDTimerClass DisperseTimer;

	BulletExt(BulletClass* OwnerObject) : ObjectExt(OwnerObject)
		, TypeExtData { nullptr }
		, FirerHouse { nullptr }
		, CurrentStrength { 0 }
		, InterceptorTechnoType { nullptr }
		, InterceptedStatus { InterceptedStatus::None }
		, DetonateOnInterception { true }
		, LaserTrails {}
		, SnappedToTarget { false }
		, DamageNumberOffset { INT32_MIN }
		, ParabombFallRate { 0 }
		, IsInstantDetonation { false }
		, FirepowerMult { 1.0 }
		, IsSplitFromAirburst { false }
		, DistanceTraveled { 0 }

		, Trajectory { nullptr }
		, DispersedTrajectory { false }
		, SweepFireAim { false }
		, SweepFireLaunchAim { CoordStruct::Empty }
		, LifeDurationTimer {}
		, NoTargetLifeTimer {}
		, RetargetTimer {}
		, AttenuationRange { 0 }
		, TargetIsInAir { false }
		, TargetIsTechno { false }
		, NotMainWeapon { false }
		, Status { TrajectoryStatus::None }
		, FLHCoord { CoordStruct::Empty }
		, TrajectoryGroup {}
		, GroupIndex { -1 }
		, PassDetonateDamage { 0 }
		, PassDetonateTimer {}
		, ProximityImpact { 0 }
		, ProximityDamage { 0 }
		, ExtraCheck { nullptr }
		, Casualty {}
		, DisperseIndex { 0 }
		, DisperseCount { 0 }
		, DisperseCycle { 0 }
		, DisperseTimer {}
	{}

	virtual ~BulletExt() override;

	virtual void LoadFromStream(PhobosStreamReader& Stm) override;
	virtual void SaveToStream(PhobosStreamWriter& Stm) override;

	// the extension state that goes with BulletClass::Init
	void InitializeState();

	// the bullet was created while a savegame was loading, so BulletClass::Init found
	// no extension to initialize; catch up now that there is one
	virtual void OnDeferredAllocation() override { this->InitializeState(); }

	void InterceptBullet(TechnoClass* pSource, BulletClass* pInterceptor);
	void ApplyRadiationToCell(CellStruct cell, int spread, int radLevel);
	void InitializeLaserTrails();

	void InitializeOnUnlimbo();
	bool CheckOnEarlyUpdate();
	void CheckOnPreDetonate();
	bool FireAdditionals();
	void DetonateOnObstacle();
	bool CheckSynchronize();
	bool CheckNoTargetLifeTime();
	void UpdateGroupIndex();

	std::vector<CellClass*> GetCellsInProximityRadius();
	bool CheckThroughAndSubjectInCell(CellClass* pCell, HouseClass* pOwner);
	void CalculateNewDamage();
	void PassWithDetonateAt();
	template<bool allies, bool sphere>
	std::vector<TechnoClass*> GetTargetsInProximityRadius(HouseClass* pOwner);
	void PrepareForDetonateAt();
	void ProximityDetonateAt(HouseClass* pOwner, TechnoClass* pTarget);
	int GetTrueDamage(int damage, bool self);
	double GetExtraDamageMultiplier();

	bool BulletRetargetTechno();
	void GetTechnoFLHCoord();
	CoordStruct GetDisperseWeaponFireCoord(TechnoClass* pTechno);
	bool PrepareDisperseWeapon();
	bool FireDisperseWeapon(TechnoClass* pFirer, const CoordStruct& sourceCoord, HouseClass* pOwner);
	void CreateDisperseBullets(TechnoClass* pTechno, const CoordStruct& sourceCoord, WeaponTypeClass* pWeapon, AbstractClass* pTarget, HouseClass* pOwner, int curBurst, int maxBurst);

private:
	template <typename T>
	void Serialize(T& Stm);

public:
	class ExtContainer final : public Container<BulletExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	static constexpr double Epsilon = 1e-10;
	static constexpr double EpsilonSquared = 1e-20;

	static BulletExt* Fetch(const BulletClass* pThis)
	{
		return AbstractExt::Fetch<BulletExt>(pThis);
	}

	static BulletExt* TryFetch(const BulletClass* pThis)
	{
		return AbstractExt::TryFetch<BulletExt>(pThis);
	}

	static void Detonate(const CoordStruct& coords, TechnoClass* pOwner, int damage, HouseClass* pFiringHouse, AbstractClass* pTarget, bool isBright, WeaponTypeClass* pWeapon, WarheadTypeClass* pWarhead);
	static void ApplyArcingFix(BulletClass* pThis, const CoordStruct& sourceCoords, const CoordStruct& targetCoords, BulletVelocity& velocity);
	static CoordStruct GetTargetCoordsForFiring(BulletClass* pBullet);

	// Ballistic launch velocity for an Arcing projectile aimed at an explicit point. This is
	// the same approximation SimulatedFiringUnlimbo uses for hand-made shots; it is exposed
	// because the first shot of a SweepFire sweep is launched by the engine, whose own
	// solution follows the firing techno's facing (and its Inaccurate+Arcing scatter) rather
	// than the aim point, so that shot has to be re-solved once the bullet exists.
	static BulletVelocity ComputeArcingLaunchVelocity(BulletTypeClass* pType, WeaponTypeClass* pWeapon, const CoordStruct& sourceCoords, const CoordStruct& targetCoords);

	// Explicit aim point for the launch velocity. When set, it is used instead of
	// the aim target's own coordinates, which is what lets SweepFire aim a shot at
	// an arbitrary point rather than at the centre of a cell.
	static void SimulatedFiringUnlimbo(BulletClass* pBullet, HouseClass* pHouse, WeaponTypeClass* pWeapon, const CoordStruct& sourceCoords, bool headToTarget, const RadialFireStruct& radialFire = {}, const CoordStruct* pAimCoords = nullptr);
	static void SimulatedFiringEffects(BulletClass* pBullet, HouseClass* pHouse, ObjectClass* pAttach, bool firingEffect, bool visualEffect);
	static inline void SimulatedFiringAnim(BulletClass* pBullet, HouseClass* pHouse, ObjectClass* pAttach);
	static inline void SimulatedFiringReport(BulletClass* pBullet);
	static inline void SimulatedFiringLaser(BulletClass* pBullet, HouseClass* pHouse);
	static inline void SimulatedFiringElectricBolt(BulletClass* pBullet);
	static inline void SimulatedFiringRadBeam(BulletClass* pBullet, HouseClass* pHouse);
	static inline void SimulatedFiringParticleSystem(BulletClass* pBullet, HouseClass* pHouse);
	static inline BulletVelocity ApplyRadialFireVelocityWarp(BulletVelocity velocity, const RadialFireStruct& radialFire);

	static inline double Get2DDistance(const CoordStruct& coords)
	{
		return Point2D { coords.X, coords.Y }.Magnitude();
	}
	static inline double Get2DDistanceSquared(const CoordStruct& coords)
	{
		return Point2D { coords.X, coords.Y }.MagnitudeSquared();
	}
	static inline double Get2DDistance(const CoordStruct& source, const CoordStruct& target)
	{
		return Point2D { source.X, source.Y }.DistanceFrom(Point2D { target.X, target.Y });
	}
	static inline double Get2DVelocity(const BulletVelocity& velocity)
	{
		return Vector2D<double>{ velocity.X, velocity.Y }.Magnitude();
	}
	static inline double Get2DOpRadian(const CoordStruct& source, const CoordStruct& target)
	{
		return Math::atan2(target.Y - source.Y, target.X - source.X);
	}
	static inline BulletVelocity Coord2Vector(const CoordStruct& coords)
	{
		return BulletVelocity { static_cast<double>(coords.X), static_cast<double>(coords.Y), static_cast<double>(coords.Z) };
	}
	static inline CoordStruct Vector2Coord(const BulletVelocity& velocity)
	{
		return CoordStruct { static_cast<int>(velocity.X), static_cast<int>(velocity.Y), static_cast<int>(velocity.Z) };
	}
	static inline BulletVelocity HorizontalRotate(const CoordStruct& coords, const double radian)
	{
		return BulletVelocity { coords.X * Math::cos(radian) + coords.Y * Math::sin(radian), coords.X * Math::sin(radian) - coords.Y * Math::cos(radian), static_cast<double>(coords.Z) };
	}
	static inline Point2D Coord2Point(const CoordStruct& coords)
	{
		return Point2D { coords.X, coords.Y };
	}
	static inline CoordStruct Point2Coord(const Point2D& point, const int z = 0)
	{
		return CoordStruct { point.X, point.Y, z };
	}
	static inline Point2D PointRotate(const Point2D& point, const double radian)
	{
		return Point2D { static_cast<int>(point.X * Math::cos(radian) + point.Y * Math::sin(radian)), static_cast<int>(point.X * Math::sin(radian) - point.Y * Math::cos(radian)) };
	}
	static inline double GetDistanceFrom(const CoordStruct& source, const TechnoClass* const pTarget)
	{
		auto distance = source.DistanceFrom(pTarget->GetCoords());

		if (const auto pBuilding = abstract_cast<const BuildingClass*, true>(pTarget))
		{
			const auto pType = pBuilding->Type;
			distance = Math::max(0, distance - 64 * (pType->GetFoundationHeight(false) + pType->GetFoundationWidth()));
		}

		return distance;
	}
	static inline bool CheckTechnoIsInvalid(const TechnoClass* const pTechno)
	{
		return (!pTechno->IsAlive || !pTechno->IsOnMap || pTechno->InLimbo || pTechno->IsSinking || pTechno->Health <= 0);
	}
	static inline bool CheckWeaponCanTarget(const WeaponTypeExt* const pWeaponExt, TechnoClass* const pFirer, TechnoClass* const pTarget)
	{
		return !pWeaponExt || (EnumFunctions::IsTechnoEligible(pTarget, pWeaponExt->CanTarget) && pWeaponExt->IsHealthInThreshold(pTarget) && pWeaponExt->HasRequiredAttachedEffects(pTarget, pFirer));
	}
	static inline bool CheckWeaponValidness(HouseClass* const pHouse, const TechnoClass* const pTechno, const CellClass* const pCell, const AffectedHouse flags)
	{
		if (pHouse == pTechno->Owner)
			return (flags & AffectedHouse::Owner) != AffectedHouse::None;
		else if (pHouse->IsAlliedWith(pTechno->Owner) || pTechno->IsDisguisedAs(pHouse))
			return (flags & AffectedHouse::Allies) != AffectedHouse::None;
		else if ((flags & AffectedHouse::Enemies) == AffectedHouse::None)
			return false;

		return pTechno->CloakState != CloakState::Cloaked || pCell->Sensors_InclHouse(pHouse->ArrayIndex);
	}
	static inline bool CheckCanRetarget(TechnoClass* const pTechno, HouseClass* const pOwner, const AffectedHouse retargetHouses, const CoordStruct& center, const double retargetRange, const int range,
		const BulletClass* const pBullet, const WeaponTypeClass* const pWeapon, const WeaponTypeExt* const pWeaponExt, TechnoClass* const pFirer)
	{
		const auto pTechnoType = pTechno->GetTechnoType();

		return pTechnoType->LegalTarget
			&& !pTechno->IsBeingWarpedOut()
			&& BulletExt::CheckWeaponValidness(pOwner, pTechno, pTechno->GetCell(), retargetHouses)
			&& BulletExt::GetDistanceFrom(center, pTechno) <= retargetRange
			&& MapClass::GetTotalDamage(100, pBullet->WH, pTechnoType->Armor, 0) != 0
			&& (!pWeapon || BulletExt::GetDistanceFrom(pFirer ? pFirer->GetCoords() : pBullet->SourceCoords, pTechno) <= range)
			&& BulletExt::CheckWeaponCanTarget(pWeaponExt, pFirer, pTechno);
	}
	static inline bool CheckCanDisperse(TechnoClass* const pTechno, HouseClass* const pOwner, const BulletTypeExt* const pType, const CoordStruct& center, const CellClass* const pCell, const int range,
		const AbstractClass* const pTarget, const WeaponTypeClass* const pWeapon, const WeaponTypeExt* const pWeaponExt, TechnoClass* const pFirer)
	{
		const auto pTechnoType = pTechno->GetTechnoType();

		return pTechnoType->LegalTarget
			&& (!pType->DisperseTendency || pType->DisperseDoRepeat || pTechno != pTarget)
			&& !pTechno->IsBeingWarpedOut()
			&& BulletExt::CheckWeaponValidness(pOwner, pTechno, pCell, pWeaponExt->CanTargetHouses)
			&& BulletExt::GetDistanceFrom(center, pTechno) <= range
			&& MapClass::GetTotalDamage(100, pWeapon->Warhead, pTechnoType->Armor, 0) != 0
			&& BulletExt::CheckWeaponCanTarget(pWeaponExt, pFirer, pTechno);
	}
	static inline void SetNewDamage(int& damage, const double ratio)
	{
		if (damage)
		{
			if (const auto newDamage = static_cast<int>(damage * ratio))
				damage = newDamage;
			else
				damage = Math::sgn(damage);
		}
	}
	static inline TechnoClass* GetSurfaceFirer(TechnoClass* pFirer)
	{
		for (auto pTrans = pFirer; pTrans; pTrans = pTrans->Transporter)
			pFirer = pTrans;

		return pFirer;
	}
	static inline std::pair<int, int> GetScatterOffsets(BulletTypeClass* pType, WeaponTypeClass* pWeapon, const CoordStruct& sourceCoord, const CoordStruct& targetCoord)
	{
		const auto pTypeExt = BulletTypeExt::Fetch(pType);

		// Both variants are distance-scaled. The engine used to scale linearly by
		// (distance / range); that ramp is now shaped by the Divergence coefficient, which also
		// caps it at 1 so that shooting beyond the weapon's range cannot exceed
		// BallisticScatter.Max.
		//
		// The !FlakScatter variant used to return a fixed interval and was therefore identical
		// at every range (the handover manual's P0-3); it now takes the same ramp. Each variant
		// keeps the lower-bound default it already had - [0, global] for the FlakScatter variant
		// and [global / 2, global] for the other one - so at the weapon's full range both still
		// produce exactly what they produced before.
		const double distance = sourceCoord.DistanceFrom(targetCoord);
		const double range = pWeapon ? pWeapon->Range : (10.0 * Unsorted::LeptonsPerCell);
		const double divergence = pTypeExt->GetScatterDivergence();
		const double offsetMult = ScatterDistanceFactor(distance, range, divergence);

		const int defaultMin = pType->FlakScatter ? 0 : RulesClass::Instance->BallisticScatter / 2;
		const int min = static_cast<int>(offsetMult * pTypeExt->BallisticScatter_Min.Get(Leptons(defaultMin)));
		const int max = static_cast<int>(offsetMult * pTypeExt->BallisticScatter_Max.Get(Leptons(RulesClass::Instance->BallisticScatter)));

		return std::make_pair(min, max);
	}

	static inline std::pair<int, int> GetScatterOffsets(BulletClass* pBullet, const CoordStruct& sourceCoord, const CoordStruct& targetCoord)
	{
		return GetScatterOffsets(pBullet->Type, pBullet->WeaponType, sourceCoord, targetCoord);
	}

	// Whether a projectile takes part in the universal scatter system at all. This mirrors
	// the gates of the engine's two sites: path 1 (BulletClass::Unlimbo, FlakScatter + Inviso)
	// and path 2 (TechnoClass::Fire_At, Inaccurate + Arcing). Path 3 is driven by the
	// projectile having a trajectory and is handled by the trajectory itself, so a caller
	// that creates shots by hand must skip anything with a trajectory to avoid scattering
	// the same shot twice.
	static inline bool IsScatterEligible(BulletTypeClass* pType)
	{
		return (pType->Inaccurate && pType->Arcing) || (pType->FlakScatter && pType->Inviso);
	}

	static inline bool HasTrajectory(BulletTypeClass* pType)
	{
		return BulletTypeExt::Fetch(pType)->TrajectoryType.get() != nullptr;
	}

	// Scatter an explicit aim coordinate the same way the Phobos trajectory path does
	// (path 3): draw a radius from the projectile's own interval, shape it with Sigma,
	// turn it into a polar offset and finally apply the Aspect ellipse. Used by SweepFire,
	// whose shots aim at coordinates rather than at a target object.
	static CoordStruct GetScatteredCoord(const CoordStruct& baseCoord, const CoordStruct& sourceCoord, const CoordStruct& dirCoord, BulletTypeClass* pType, WeaponTypeClass* pWeapon);

#pragma region UniversalScatterShaping

	// Universal scatter shaping shared by every scatter path:
	//   * classic FlakScatter (BulletClass::Unlimbo, Inviso projectiles)
	//   * visible Fire_At scatter (Inaccurate + Arcing, FlakScatter + non-Inviso)
	//   * legacy Fire_At scatter (Inaccurate + Arcing, !FlakScatter or Inviso)
	//   * Phobos trajectories (ActualTrajectory::GetInaccurateTargetCoords), via
	//     BulletExt::GetScatterOffsets, which now distance-scales both of its variants
	//   * ClusterScatter (extra detonations)
	//   * SweepFire, through BulletExt::GetScatteredCoord, which reuses GetScatterOffsets
	//
	// Sigma  : radius distribution exponent. 1.0 reproduces the legacy uniform-in-radius
	//          draw exactly, so an unset Sigma is a strict no-op. Each +1 halves the median
	//          radius; 0.5 approximates a uniform-in-area (visually even) disc.
	// Aspect : area-conserving ellipse ratio (long axis / short axis). 1.0 keeps the circle.
	//          >1 stretches the ellipse perpendicular to the flight line, <1 along it.
	static constexpr double Scatter_SigmaMin = 0.5;
	static constexpr double Scatter_SigmaMax = 16.0;
	static constexpr double Scatter_AspectMin = 0.125;
	static constexpr double Scatter_AspectMax = 8.0;

	static inline double NormalizeScatterSigma(const double sigma)
	{
		return std::isfinite(sigma) ? Math::clamp(sigma, Scatter_SigmaMin, Scatter_SigmaMax) : 1.0;
	}

	static inline double NormalizeScatterAspect(const double aspect)
	{
		return std::isfinite(aspect) ? Math::clamp(aspect, Scatter_AspectMin, Scatter_AspectMax) : 1.0;
	}

	// Remap a radius the engine drew uniformly in [drawMin, drawMax] onto [targetMin, targetMax]
	// and optionally bias it towards the lower bound with the Sigma exponent.
	//
	// Every engine scatter site draws from a fixed interval derived from the *global*
	// [CombatDamage] -> BallisticScatter, so remapping after the draw is what lets the
	// per-projectile BallisticScatter.Min / .Max (and Sigma) reach those sites without
	// touching the random number stream. Clamping instead of remapping would pile values up
	// on the lower bound, so the uniform draw is rescaled, not clipped, unless the drawn
	// value already sits outside the engine's own interval.
	static inline int RemapScatterRadius(const int raw, const int drawMin, const int drawMax,
		const int targetMin, const int targetMax, const double sigma)
	{
		const double exponent = NormalizeScatterSigma(sigma);

		// Exact no-op when nothing is configured, so an unset pair stays bit-identical.
		if (exponent == 1.0 && drawMin == targetMin && drawMax == targetMax)
			return raw;

		const int drawLo = Math::min(drawMin, drawMax);
		const int drawHi = Math::max(drawMin, drawMax);

		if (drawHi <= drawLo)
			return raw;

		double u;

		if (raw <= drawLo)
			u = 0.0;
		else if (raw >= drawHi)
			u = 1.0;
		else
			u = (raw - drawLo) / static_cast<double>(drawHi - drawLo);

		if (exponent != 1.0)
			u = std::pow(u, exponent);

		const int outLo = Math::min(targetMin, targetMax);
		const int outHi = Math::max(targetMin, targetMax);

		if (outHi <= outLo)
			return outLo;

		return Math::clamp(static_cast<int>(std::lround(outLo + (outHi - outLo) * u)), outLo, outHi);
	}

	// Shaping for the sites whose drawn interval already is the wanted one.
	static inline int ShapeScatterRadius(const int raw, const int min, const int max, const double sigma)
	{
		return RemapScatterRadius(raw, min, max, min, max, sigma);
	}

	// Turn the circular scatter offset into an area-conserving ellipse. The offset is
	// re-expressed in the flight frame, scaled by sqrt(Aspect) laterally and 1/sqrt(Aspect)
	// along the flight line, then mapped back to world space. `base` is the scatter centre
	// and `dir` any non-zero vector along the flight direction.
	static inline void ApplyScatterAspect(CoordStruct& coords, const CoordStruct& base, const CoordStruct& dir, const double aspect)
	{
		const double ratio = NormalizeScatterAspect(aspect);

		if (ratio == 1.0)
			return;

		const double dx = static_cast<double>(coords.X - base.X);
		const double dy = static_cast<double>(coords.Y - base.Y);

		if (dx == 0.0 && dy == 0.0)
			return;

		const double fx = static_cast<double>(dir.X);
		const double fy = static_cast<double>(dir.Y);
		const double length = Math::sqrt(fx * fx + fy * fy);

		if (length < Epsilon)
			return;

		// Flight frame: u along the flight line, v perpendicular to it.
		const double ux = fx / length;
		const double uy = fy / length;

		const double along = dx * ux + dy * uy;
		const double lateral = dx * -uy + dy * ux;

		const double scaleLateral = Math::sqrt(ratio);
		const double scaleAlong = 1.0 / scaleLateral;

		const double alongNew = along * scaleAlong;
		const double lateralNew = lateral * scaleLateral;

		coords.X = base.X + static_cast<int>(std::lround(alongNew * ux - lateralNew * uy));
		coords.Y = base.Y + static_cast<int>(std::lround(alongNew * uy + lateralNew * ux));
	}

	// Divergence: how the scatter grows with distance. The engine scales the scatter radius
	// linearly by (distance / range); this replaces that ramp with
	//
	//     g(x) = x ^ (1 / Divergence),      x = clamp(distance / range, 0, 1)
	//
	//   Divergence = 1.0  -> g = x, the original linear ramp (but capped, see below)
	//   Divergence < 1.0  -> scatter stays small up close and climbs to Max near max range;
	//                        smaller values are more back-loaded
	//   Divergence > 1.0  -> the ramp flattens out: the scatter is already close to its
	//                        full-range size up close, and the larger the value the closer it
	//                        gets to a distance-independent radius. There is deliberately no
	//                        upper clamp, so any value above 1.0 is taken as written.
	//
	// The clamp at 1 means firing beyond the weapon's range can no longer push the scatter
	// past BallisticScatter.Max, which the unmodified engine did allow.
	//
	// Because x is clamped and raised to a positive power, every positive Divergence keeps the
	// factor inside [0, 1]: no value of this key can push a radius past BallisticScatter.Max,
	// which is why there is no upper bound to enforce here.
	//
	// Only applied where the engine already scaled by distance: the visible Fire_At scatter
	// and the distance-scaled Phobos trajectory branch. ClusterScatter and the classic
	// Inviso path are deliberately left alone.
	static constexpr double Scatter_DivergenceMin = 0.05;

	// The engine's original linear ramp. Also the fallback for a non-finite value, so that a
	// malformed key keeps the old behaviour instead of silently becoming the flattest curve.
	static constexpr double Scatter_DivergenceLinear = 1.0;

	static inline double NormalizeScatterDivergence(const double divergence)
	{
		return std::isfinite(divergence) ? Math::max(divergence, Scatter_DivergenceMin) : Scatter_DivergenceLinear;
	}

	static inline double ScatterDistanceFactor(const double distance, const double range, const double divergence)
	{
		if (!(range > 0.0))
			return 1.0;

		const double x = Math::clamp(distance / range, 0.0, 1.0);

		if (x <= 0.0)
			return 0.0;

		const double exponent = 1.0 / NormalizeScatterDivergence(divergence);

		return exponent == 1.0 ? x : std::pow(x, exponent);
	}

#pragma endregion

	static bool CheckExceededCapacity(TechnoClass* pTechno, BulletTypeClass* pBulletType, BulletExt* pBulletExt = nullptr);
	static std::vector<CellStruct> GetCellsInRectangle(const CellStruct bottomStaCell, const CellStruct leftMidCell, const CellStruct rightMidCell, const CellStruct topEndCell);
};


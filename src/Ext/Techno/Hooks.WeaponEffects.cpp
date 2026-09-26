#include "Body.h"

#include <Ext/ParticleSystemType/Body.h>
#include <Ext/WeaponType/Body.h>
#include <Ext/Bullet/Body.h>

// Contains hooks that fix weapon graphical effects like lasers, railguns, electric bolts, beams and waves not interacting
// correctly with obstacles between firer and target, as well as railgun / railgun particles being cut off by elevation.

namespace FireAtTemp
{
	BulletClass* FireBullet = nullptr;
	CoordStruct OriginalTargetCoords;
	CellClass* pObstacleCell = nullptr;
	AbstractClass* pOriginalTarget = nullptr;
	AbstractClass* pWaveOwnerTarget = nullptr;
	bool IgnoreTargetForWaveAmbientDamage = false;
	CellClass* SweepFireAimCell = nullptr; // Set by the SweepFire hook below, consumed by the obstacle cell hook.
	// Exact start of the line the current shot belongs to (SweepFire). The engine
	// launches the first shot of a sweep itself, towards the cell it was aimed at,
	// so the bullet has to be re-aimed here once it exists.
	CoordStruct SweepFireAimCoords = CoordStruct::Empty;
	bool SweepFireAimValid = false;
	// The weapon that started the sweep. Stashed here because the record hook that has to
	// re-solve the first shot's ballistic velocity only has the bullet at hand, and the
	// bullet's own WeaponType is not guaranteed to be set that early in Fire_At.
	WeaponTypeClass* SweepFireWeapon = nullptr;
}

// SweepFire: the first shot of a sweep is an ordinary shot whose aim point is
// moved to the start of the line. The rest of Fire_At (animation, report, ammo,
// ROF, Burst accounting and the projectile's own trajectory) stays untouched
// (F2 route).
//
// The aim point is written into Fire_At's pTarget argument, [ebp+8], instead of
// into a register, and it is written before anything consumes that argument:
//
//   * without Ares the engine loads EDX from [ebp+8] at 0x6FE554 and passes it
//     to CreateBullet at 0x6FE55D;
//   * with Ares, TechnoClass_Fire_CreateBullet (Ares's hook at 0x6FE53F)
//     creates the bullet itself. It takes the target straight from the saved EBP
//     (`mov 0x8(%eax),%ebp`) and returns 0x6FE562, skipping the whole vanilla
//     block - including 0x6FE557, which is why hooking there never fired at all
//     on an Ares setup.
//
// Hooking 0x6FE53F instead is not an option: a non-zero return short-circuits
// the rest of the hook chain, and Ares's handler returns 0x6FE562 there, so a
// Phobos hook at the same address would never run. Writing the argument this
// early works with and without Ares.
//
// The stolen bytes are `mov eax,[esp+0x88]`, which has no relative operand, so
// even a verbatim copy of it stays correct.
DEFINE_HOOK(0x6FE4F6, TechnoClass_FireAt_SweepFireFirstShotAim, 0x7)
{
	GET(TechnoClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EBX);
	GET_BASE(AbstractClass*, pTarget, 0x8);
	GET_BASE(int, weaponIndex, 0xC);

	// Cleared on every shot so a stale cell can never leak into another Fire_At.
	FireAtTemp::SweepFireAimCell = nullptr;
	FireAtTemp::SweepFireAimValid = false;
	FireAtTemp::SweepFireWeapon = nullptr;

	// Diagnostic: lists every weapon that actually reaches this hook. Logged once
	// per weapon so the line is never crowded out by repeated shots.
	if (pWeapon && SweepFireDiag::FirstTime(0, pWeapon->ID))
	{
		const auto pWeaponExt = WeaponTypeExt::TryFetch(pWeapon);
		Debug::Log("[SweepFire] @6FE4F6 weapon=[%s] ext=%d enable=%d burst=%d target=%d slot=%d\n",
			pWeapon->ID, pWeaponExt ? 1 : 0, pWeaponExt ? (pWeaponExt->SweepFire_Enable ? 1 : 0) : -1,
			pThis->CurrentBurstIndex, pTarget ? 1 : 0, weaponIndex);
	}

	CoordStruct lineStart {};
	const auto pAimCell = TechnoExt::Fetch(pThis)->TryStartSweepFire(pWeapon, weaponIndex, pTarget, pThis->CurrentBurstIndex, &lineStart);

	if (pAimCell)
	{
		FireAtTemp::SweepFireAimCell = pAimCell;
		FireAtTemp::SweepFireAimCoords = lineStart;
		FireAtTemp::SweepFireAimValid = true;
		FireAtTemp::SweepFireWeapon = pWeapon;
		R->Base(0x8, pAimCell);
	}

	return 0;
}

// Set obstacle cell.
DEFINE_HOOK(0x6FF15F, TechnoClass_FireAt_ObstacleCellSet, 0x6)
{
	GET(TechnoClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EBX);
	GET_BASE(AbstractClass*, pTarget, 0x8);
	LEA_STACK(CoordStruct*, pSourceCoords, STACK_OFFSET(0xB0, -0x6C));

	// A sweep's first shot travels to the start of the line, so everything drawn
	// towards it (laser, EBolt, beam, particles) has to be aimed there as well.
	// Reusing the obstacle cell gets that for free through the hooks below.
	const auto pAimCell = FireAtTemp::SweepFireAimCell;
	FireAtTemp::SweepFireAimCell = nullptr;

	if (pWeapon && SweepFireDiag::FirstTime(1, pWeapon->ID))
		Debug::Log("[SweepFire] @6FF15F weapon=[%s] sweepAimCell=%d\n", pWeapon->ID, pAimCell ? 1 : 0);

	if (pAimCell)
	{
		FireAtTemp::pObstacleCell = pAimCell;
		TechnoExt::Fetch(pThis)->FiringObstacleCell = pAimCell;

		return 0;
	}

	const auto pBuilding = abstract_cast<BuildingClass*, true>(pTarget);
	const auto coords = pBuilding ? pBuilding->GetTargetCoords() : pTarget->GetCenterCoords();

	// This is set to a temp variable as well, as accessing it everywhere needed from TechnoExt would be more complicated.
	FireAtTemp::pObstacleCell = TrajectoryHelper::FindFirstObstacle(*pSourceCoords, coords, pWeapon->Projectile, pThis->Owner);
	TechnoExt::Fetch(pThis)->FiringObstacleCell = FireAtTemp::pObstacleCell;

	return 0;
}

DEFINE_HOOK(0x6FF08B, TechnoClass_Fire_RecordBullet, 0x6)
{
	GET(BulletClass*, pBullet, EBX);
	FireAtTemp::FireBullet = pBullet;

	// SweepFire: the first shot of a sweep is created and launched by the engine.
	// The engine derived its velocity from the (cell) target object at 0x6FED39 and
	// unlimboed the bullet at 0x6FF014, so the aim cannot be corrected any earlier
	// without desynchronising velocity and aim. Now that the bullet exists, aim it at
	// the exact start of the line and adopt that point as its aim coordinate: the
	// velocity is rotated onto it (keeping the speed the engine computed), or, for an
	// Arcing projectile whose velocity is a ballistic solution rather than a direction,
	// solved again from scratch.
	//
	// This hook is only reached when the unlimbo succeeded - a failed one jumps to
	// 0x6FF749 and skips this address - so the bullet is guaranteed to be alive.
	if (FireAtTemp::SweepFireAimValid && pBullet)
	{
		const auto& aim = FireAtTemp::SweepFireAimCoords;
		const auto from = pBullet->Location;
		const double length = static_cast<double>(aim.DistanceFrom(from));
		const double speed = pBullet->Velocity.Magnitude();

		// The projectile's own scatter belongs to the *launch solution*, exactly where it sits for
		// a normal shot of the same projectile (the engine scatters the firing offset and leaves
		// the destination on the target). `aim` is the coordinate this shot was told to fire at
		// and stays exact; only the point the launch is solved for moves. Scattering the aim point
		// instead spread the sweep's impacts by the full BallisticScatter while a non-sweeping shot
		// of the same projectile stays inside the target's cell.
		CoordStruct launchAim = aim;

		if (FireAtTemp::SweepFireWeapon && BulletExt::IsScatterEligible(pBullet->Type) && !BulletExt::HasTrajectory(pBullet->Type))
			launchAim = BulletExt::GetScatteredCoord(aim, from, aim - from, pBullet->Type, FireAtTemp::SweepFireWeapon);

		BulletExt::Fetch(pBullet)->SweepFireLaunchAim = launchAim;

		Vector3D<double> direction {
			static_cast<double>(launchAim.X - from.X),
			static_cast<double>(launchAim.Y - from.Y),
			static_cast<double>(launchAim.Z - from.Z) };

		static int probeLines = 0;

		if (SweepFireDiag::On() && probeLines < 8)
		{
			++probeLines;
			Debug::Log("[SweepFire/probe] @6FF08B valid=1 type=[%s] arcing=%d invis=%d from=(%d,%d,%d) aim=(%d,%d,%d) len=%.0f speed=%.1f\n",
				pBullet->Type->ID, pBullet->Type->Arcing ? 1 : 0, pBullet->Type->Inviso ? 1 : 0,
				from.X, from.Y, from.Z, aim.X, aim.Y, aim.Z, length, speed);
			Debug::Log("[SweepFire/probe] @6FF08B launchAim=(%d,%d,%d) scattered=%d\n",
				launchAim.X, launchAim.Y, launchAim.Z, (launchAim == aim) ? 0 : 1);
		}

		// The aim coordinate and the flag are set for every sweep shot: that coordinate is
		// what the exact detonation in BulletClass::Update lands the impact on.
		pBullet->TargetCoords = aim;
		BulletExt::Fetch(pBullet)->SweepFireAim = true;

		if (length > 1.0 && speed > 0.0 && !pBullet->Type->Arcing)
		{
			// Rotate the velocity onto the exact start of the line, keeping the speed
			// the engine computed. Arcing (lobbed) projectiles need a whole new
			// ballistic solution instead, see below.
			direction /= length;
			pBullet->Velocity = BulletVelocity { direction.X * speed, direction.Y * speed, direction.Z * speed };

			if (SweepFireDiag::Allowed())
				Debug::Log("[SweepFire] first shot redirected to the exact line start (%d,%d,%d), vel=(%.1f,%.1f,%.1f)\n",
					aim.X, aim.Y, aim.Z, pBullet->Velocity.X, pBullet->Velocity.Y, pBullet->Velocity.Z);
		}
		else if (pBullet->Type->Arcing && FireAtTemp::SweepFireWeapon)
		{
			// An Arcing projectile's velocity is a ballistic solution, not a direction, so
			// it cannot simply be rotated: it has to be solved again for the new aim point.
			// The engine's own solution is unusable here for two reasons. It follows the
			// firing techno's facing rather than the exact target, and an
			// Inaccurate + Arcing projectile additionally takes the engine's Fire_At scatter
			// (path 2) on top of the sweep's own scatter (path 5), i.e. it gets scattered
			// twice. Either way the first shot ended up aimed far away from the start of the
			// line - measured at ~1100 leptons, four cells - so the exact detonation had to
			// teleport it back. Solving it here with the very formula every follow-up shot
			// uses makes the first shot behave exactly like the rest of the sweep.
			pBullet->Velocity = BulletExt::ComputeArcingLaunchVelocity(pBullet->Type, FireAtTemp::SweepFireWeapon, from, launchAim);

			if (SweepFireDiag::Allowed())
				Debug::Log("[SweepFire] first shot re-solved for the exact line start (%d,%d,%d), engine speed %.1f -> %.1f vel=(%.1f,%.1f,%.1f)\n",
					aim.X, aim.Y, aim.Z, speed, pBullet->Velocity.Magnitude(),
					pBullet->Velocity.X, pBullet->Velocity.Y, pBullet->Velocity.Z);
		}
	}

	FireAtTemp::SweepFireAimValid = false;
	FireAtTemp::SweepFireWeapon = nullptr;

	return 0;
}

// Apply obstacle logic to fire & spark particle system targets.
DEFINE_HOOK_AGAIN(0x6FF1D7, TechnoClass_FireAt_SparkFireTargetSet, 0x5)
DEFINE_HOOK(0x6FF189, TechnoClass_FireAt_SparkFireTargetSet, 0x5)
{
	if (FireAtTemp::pObstacleCell)
	{
		if (R->Origin() == 0x6FF189)
			R->ECX(FireAtTemp::pObstacleCell);
		else
			R->EDX(FireAtTemp::pObstacleCell);
	}

	return 0;
}

// Fix fire particle target coordinates potentially differing from actual target coords.
DEFINE_HOOK(0x62FA41, ParticleSystemClass_FireAI_TargetCoords, 0x6)
{
	enum { SkipGameCode = 0x62FBAF };

	GET(ParticleSystemClass*, pThis, ESI);

	auto const pTypeExt = ParticleSystemTypeExt::Fetch(pThis->Type);

	if (!pTypeExt->AdjustTargetCoordsOnRotation)
		return SkipGameCode;

	return 0;
}

// Fix fire particles being disallowed from going upwards.
DEFINE_HOOK(0x62D685, ParticleSystemClass_Fire_Coords, 0x5)
{
	enum { SkipGameCode = 0x62D6B7 };

	// Game checks if MapClass::GetCellFloorHeight() for currentCoords is larger than for previousCoords and sets the flags on ParticleClass to
	// remove it if so. Below is an attempt to create a smarter check that allows upwards movement and does not needlessly collide with elevation
	// but removes particles when colliding with flat ground. It doesn't work perfectly and covering all edge-cases is difficult or impossible so
	// preference was to disable it. Keeping the code here commented out, however.

	/*
	GET(ParticleClass*, pThis, ESI);
	REF_STACK(CoordStruct, currentCoords, STACK_OFFSET(0x24, -0x18));
	REF_STACK(CoordStruct, previousCoords, STACK_OFFSET(0x24, -0xC));

	auto const sourceLocation = pThis->ParticleSystem ? pThis->ParticleSystem->Location : CoordStruct { INT_MAX, INT_MAX, INT_MAX };
	auto const pCell = MapClass::Instance.TryGetCellAt(currentCoords);
	int cellFloor = MapClass::Instance.GetCellFloorHeight(currentCoords);
	bool downwardTrajectory = currentCoords.Z < previousCoords.Z;
	bool isBelowSource = cellFloor < sourceLocation.Z - Unsorted::LevelHeight * 2;
	bool isRamp = pCell ? pCell->SlopeIndex : false;

	if (!isRamp && isBelowSource && downwardTrajectory && currentCoords.Z < cellFloor)
	{
		pThis->unknown_12D = 1;
		pThis->unknown_131 = 1;
	}
	*/

	return SkipGameCode;
}

// Fix railgun target coordinates potentially differing from actual target coords.
DEFINE_HOOK(0x70C6B5, TechnoClass_Railgun_TargetCoords, 0x5)
{
	GET(AbstractClass*, pTarget, EBX);
	GET(CoordStruct*, pCoords, EAX);

	if (const auto pBuilding = abstract_cast<BuildingClass*, true>(pTarget))
		*pCoords = pBuilding->GetTargetCoords();
	else if (const auto pCell = abstract_cast<CellClass*, true>(pTarget))
		*pCoords = pCell->GetCoordsWithBridge();

	return 0;
}

// Cut railgun logic off at obstacle coordinates.
DEFINE_HOOK(0x70CA64, TechnoClass_Railgun_Obstacles, 0x5)
{
	enum { Continue = 0x70CA79, Stop = 0x70CAD8 };

	REF_STACK(CoordStruct const, coords, STACK_OFFSET(0xC0, -0x80));

	const auto pCell = MapClass::Instance.GetCellAt(coords);

	if (pCell == FireAtTemp::pObstacleCell)
		return Stop;

	return Continue;
}

DEFINE_HOOK(0x70C862, TechnoClass_Railgun_AmbientDamageIgnoreTarget1, 0x5)
{
	enum { IgnoreTarget = 0x70CA59 };

	GET_BASE(WeaponTypeClass*, pWeapon, 0x14);

	if (WeaponTypeExt::Fetch(pWeapon)->AmbientDamage_IgnoreTarget.Get(RulesExt::Global()->AmbientDamage_IgnoreTarget))
		return IgnoreTarget;

	return 0;
}

DEFINE_HOOK(0x70CA8B, TechnoClass_Railgun_AmbientDamageIgnoreTarget2, 0x6)
{
	enum { IgnoreTarget = 0x70CBB0 };

	GET_BASE(WeaponTypeClass*, pWeapon, 0x14);
	REF_STACK(DynamicVectorClass<ObjectClass*>, objects, STACK_OFFSET(0xC0, -0xAC));

	if (WeaponTypeExt::Fetch(pWeapon)->AmbientDamage_IgnoreTarget.Get(RulesExt::Global()->AmbientDamage_IgnoreTarget))
	{
		R->EAX(objects.Count);
		return IgnoreTarget;
	}

	return 0;
}

DEFINE_HOOK(0x70CBDA, TechnoClass_Railgun_AmbientDamageWarhead, 0x6)
{
	enum { SkipGameCode = 0x70CBE0 };

	GET(WeaponTypeClass*, pWeapon, EDI);

	R->EDX(WeaponTypeExt::Fetch(pWeapon)->AmbientDamage_Warhead.Get(pWeapon->Warhead));

	return SkipGameCode;
}

DEFINE_HOOK(0x75F39D, WaveClass_DamageAI_AmbientDamageWarhead, 0x6)
{
	enum { SkipGameCode = 0x75F3A3 };

	GET(WeaponTypeClass*, pWeapon, EBX);

	auto const pTypeExt = WeaponTypeExt::Fetch(pWeapon);
	FireAtTemp::IgnoreTargetForWaveAmbientDamage = pTypeExt->AmbientDamage_IgnoreTarget.Get(RulesExt::Global()->AmbientDamage_IgnoreTarget);
	R->EAX(pTypeExt->AmbientDamage_Warhead.Get(pWeapon->Warhead));

	return SkipGameCode;
}

DEFINE_HOOK(0x75F415, WaveClass_DamageAI_AmbientDamageIgnoreTarget, 0x6)
{
	enum { IgnoreTarget = 0x75F432 };

	GET(WaveClass*, pThis, EBP);
	GET(ObjectClass*, pObject, ESI);

	if (FireAtTemp::IgnoreTargetForWaveAmbientDamage && pThis->Target == pObject)
		return IgnoreTarget;

	return 0;
}

// Do not adjust map coordinates for railgun or fire stream particles that are below cell coordinates.
DEFINE_HOOK(0x62B8BC, ParticleClass_CTOR_CoordAdjust, 0x6)
{
	enum { SkipCoordAdjust = 0x62B8CB };

	GET(ParticleClass*, pThis, ESI);
	const auto pParticleSys = pThis->ParticleSystem;

	if (pParticleSys && pParticleSys->Type)
	{
		const auto behavesLike = pParticleSys->Type->BehavesLike;

		if (behavesLike == BehavesLike::Railgun || behavesLike == BehavesLike::Fire)
			return SkipCoordAdjust;
	}

	return 0;
}

DEFINE_HOOK_AGAIN(0x6FD70D, TechnoClass_DrawSth_DrawToInvisoFlakScatterLocation, 0x6) // CreateRBeam
DEFINE_HOOK_AGAIN(0x6FD514, TechnoClass_DrawSth_DrawToInvisoFlakScatterLocation, 0x7) // CreateEBolt
DEFINE_HOOK(0x6FD38D, TechnoClass_DrawSth_DrawToInvisoFlakScatterLocation, 0x7) // CreateLaser
{
	GET(CoordStruct*, pTargetCoords, EAX);

	if (const auto pBullet = FireAtTemp::FireBullet)
	{
		// The weapon may not have been set up
		const auto pWeaponExt = WeaponTypeExt::TryFetch(pBullet->WeaponType);

		if (pWeaponExt && pWeaponExt->VisualScatter)
		{
			const auto& pRulesExt = RulesExt::Global();
			const auto radius = ScenarioClass::Instance->Random.RandomRanged(pRulesExt->VisualScatter_Min.Get(), pRulesExt->VisualScatter_Max.Get());
			*pTargetCoords = MapClass::GetRandomCoordsNear(BulletExt::GetTargetCoordsForFiring(pBullet), radius, false);
		}
		else
		{
			*pTargetCoords = BulletExt::GetTargetCoordsForFiring(pBullet);
		}
	}
	else if (const auto pObstacleCell = FireAtTemp::pObstacleCell)
	{
		*pTargetCoords = pObstacleCell->GetCoordsWithBridge();
	}

	R->EAX(pTargetCoords);
	return 0;
}

// Adjust target for bolt / beam / wave drawing.
DEFINE_HOOK(0x6FF43F, TechnoClass_FireAt_TargetSet, 0x6)
{
	LEA_STACK(CoordStruct*, pTargetCoords, STACK_OFFSET(0xB0, -0x28));
	GET_BASE(AbstractClass*, pOriginalTarget, 0x8);

	// Store original target & coords
	FireAtTemp::OriginalTargetCoords = *pTargetCoords;
	FireAtTemp::pOriginalTarget = pOriginalTarget;

	if (FireAtTemp::pObstacleCell)
	{
		*pTargetCoords = FireAtTemp::pObstacleCell->GetCoordsWithBridge();
		R->Base(8, FireAtTemp::pObstacleCell); // Replace original target so it gets used by Ares sonic wave stuff etc. as well.
	}

	return 0;
}

// Restore original target values and unset obstacle cell.
DEFINE_HOOK(0x6FF660, TechnoClass_FireAt_ObstacleCellUnset, 0x6)
{
	LEA_STACK(CoordStruct*, pTargetCoords, STACK_OFFSET(0xB0, -0x28));

	// Restore original target & coords
	*pTargetCoords = FireAtTemp::OriginalTargetCoords;
	R->Base(8, FireAtTemp::pOriginalTarget);
	R->EDI(FireAtTemp::pOriginalTarget);

	// Reset temp values
	FireAtTemp::FireBullet = nullptr;
	FireAtTemp::OriginalTargetCoords = CoordStruct::Empty;
	FireAtTemp::pObstacleCell = nullptr;
	FireAtTemp::pOriginalTarget = nullptr;
	FireAtTemp::SweepFireAimCell = nullptr;

	return 0;
}

// Allow drawing single color lasers with thickness.
DEFINE_HOOK(0x6FD446, TechnoClass_LaserZap_IsSingleColor, 0x7)
{
	GET(WeaponTypeClass* const, pWeapon, ECX);
	GET(LaserDrawClass* const, pLaser, EAX);

	if (auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon))
	{
		if (!pLaser->IsHouseColor && pWeaponExt->Laser_IsSingleColor)
			pLaser->IsHouseColor = true;
	}

	// Fixes drawing thick lasers for non-PrismSupport building-fired lasers.
	pLaser->IsSupported = pLaser->Thickness > 3;

	return 0;
}

// WaveClass requires the firer's target and wave's target to match so it needs bit of extra handling here for obstacle cell targets.
DEFINE_HOOK(0x762AFF, WaveClass_AI_TargetSet, 0x6)
{
	GET(WaveClass*, pThis, ESI);

	if (pThis->Target)
	{
		if (auto const pOwner = pThis->Owner)
		{
			auto const pObstacleCell = TechnoExt::Fetch(pOwner)->FiringObstacleCell;

			if (pObstacleCell == pThis->Target && pOwner->Target)
			{
				FireAtTemp::pWaveOwnerTarget = pOwner->Target;
				pOwner->Target = pThis->Target;
			}
		}
	}

	return 0;
}

DEFINE_HOOK(0x762D57, WaveClass_AI_TargetUnset, 0x6)
{
	GET(WaveClass*, pThis, ESI);

	if (FireAtTemp::pWaveOwnerTarget)
	{
		if (pThis->Owner->Target)
			pThis->Owner->Target = FireAtTemp::pWaveOwnerTarget;

		FireAtTemp::pWaveOwnerTarget = nullptr;
	}

	return 0;
}

#include "Body.h"
#include <Ext/Anim/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/WeaponType/Body.h>
#include <Utilities/AresHelper.h>
#include <Utilities/Debug.h>
#include <cstring>
#include <unordered_set>

// has everything inited except SpawnNextAnim at this point
DEFINE_HOOK(0x466556, BulletClass_Init, 0x6)
{
	GET(BulletClass*, pThis, ECX);

	// No extension while a savegame is loading: either it comes from the extension
	// stream, or the bullet is one the game is creating as part of the load and its
	// extension - and this initialization with it - follows once the load settles.
	if (auto const pExt = BulletExt::TryFetch(pThis))
		pExt->InitializeState();

	return 0;
}

// The state a bullet's extension gets when the bullet itself is initialized. Also run
// for extensions allocated after the fact, for bullets the game created while a
// savegame was loading.
void BulletExt::InitializeState()
{
	auto const pThis = this->OwnerObject();

	if (pThis->Owner)
	{
		this->FirerHouse = pThis->Owner->Owner;
		this->FirepowerMult = TechnoExt::GetCurrentFirepowerMultiplier(pThis->Owner);
	}

	auto const pType = pThis->Type;

	if (!pType)
		return;

	this->CurrentStrength = pType->Strength;
	this->TypeExtData = BulletTypeExt::Fetch(pType);

	if (!pType->Inviso)
		this->InitializeLaserTrails();
}

// Set in BulletClass::AI and guaranteed to be valid within it.
namespace BulletAITemp
{
	BulletExt* ExtData;
	BulletTypeExt* TypeExtData;
}

DEFINE_HOOK(0x4666F7, BulletClass_AI, 0x6)
{
	enum { Detonate = 0x467E53 };

	GET(BulletClass*, pThis, EBP);

	const auto pBulletExt = BulletExt::Fetch(pThis);
	const auto pBulletTypeExt = pBulletExt->TypeExtData;
	BulletAITemp::ExtData = pBulletExt;
	BulletAITemp::TypeExtData = pBulletTypeExt;

	if (pBulletExt->InterceptedStatus & InterceptedStatus::Targeted)
	{
		if (const auto pTarget = abstract_cast<BulletClass*>(pThis->Target))
		{
			const auto pTargetExt = BulletExt::Fetch(pTarget);

			if (!pTargetExt->TypeExtData->Armor.isset())
				pTargetExt->InterceptedStatus |= InterceptedStatus::Locked;
		}
	}

	if (pBulletExt->InterceptedStatus & InterceptedStatus::Intercepted)
	{
		if (const auto pTarget = abstract_cast<BulletClass*>(pThis->Target))
			BulletExt::Fetch(pTarget)->InterceptedStatus &= ~InterceptedStatus::Locked;

		if (pBulletExt->DetonateOnInterception)
			pThis->Detonate(pThis->GetCoords());

		pThis->Limbo();
		pThis->UnInit();

		const auto pTechno = pThis->Owner;
		const auto pWeapon = pThis->WeaponType;

		if (pTechno
			&& pTechno->InLimbo
			&& pWeapon
			&& pWeapon->LimboLaunch)
		{
			pThis->SetTarget(nullptr);
			auto damage = pTechno->Health * 2;
			pTechno->SetLocation(pThis->GetCoords());
			pTechno->ReceiveDamage(&damage, 0, RulesClass::Instance->C4Warhead, nullptr, true, false, nullptr);
		}
	}

	// Because the laser trails will be drawn before the calculation of changing the velocity direction in each frame.
	// This will cause the laser trails to be drawn in the wrong position too early, resulting in a visual appearance resembling a "bouncing".
	// Let trajectories draw their own laser trails after the Trajectory's OnEarlyUpdate() to avoid predicting incorrect positions or pass through targets.
	if (const auto pTraj = pBulletExt->Trajectory.get())
	{
		if (pTraj->OnEarlyUpdate() && !pThis->SpawnNextAnim)
			return Detonate;
	}
	else
	{
		if (pBulletExt->CheckOnEarlyUpdate() && !pThis->SpawnNextAnim)
			return Detonate;

		if (pBulletExt->LaserTrails.size())
		{
			const CoordStruct location = pThis->GetCoords();
			const BulletVelocity& velocity = pThis->Velocity;

			// We adjust LaserTrails to account for vanilla bug of drawing stuff one frame ahead.
			// Pretty meh solution but works until we fix the bug - Kerbiter
			const CoordStruct drawnCoords
			{
				(int)(location.X + velocity.X),
				(int)(location.Y + velocity.Y),
				(int)(location.Z + velocity.Z)
			};

			for (const auto& pTrail : pBulletExt->LaserTrails)
			{
				// We insert initial position so the first frame of trail doesn't get skipped - Kerbiter
				// TODO move hack to BulletClass creation
				if (!pTrail->LastLocation.isset())
					pTrail->LastLocation = location;

				pTrail->Update(drawnCoords);
			}
		}
	}

	if (pThis->HasParachute)
	{
		int fallRate = pBulletExt->ParabombFallRate - pBulletTypeExt->Parachuted_FallRate;
		const int maxFallRate = pBulletTypeExt->Parachuted_MaxFallRate.Get(RulesClass::Instance->ParachuteMaxFallRate);

		if (fallRate < maxFallRate)
			fallRate = maxFallRate;

		pBulletExt->ParabombFallRate = fallRate;
		pThis->FallRate = fallRate;
	}

	return 0;
}

DEFINE_HOOK(0x466897, BulletClass_AI_Trailer, 0x6)
{
	enum { SkipGameCode = 0x4668BD };

	GET(BulletClass*, pThis, EBP);
	REF_STACK(const CoordStruct, coords, STACK_OFFSET(0x1A8, -0x184));

	auto const pTrailerAnim = GameCreate<AnimClass>(pThis->Type->Trailer, coords, 1, 1);
	auto const pTrailerAnimExt = AnimExt::Fetch(pTrailerAnim);
	auto const pOwner = pThis->Owner ? pThis->Owner->Owner : BulletAITemp::ExtData->FirerHouse;
	AnimExt::SetAnimOwnerHouseKind(pTrailerAnim, pOwner, nullptr, false, true);
	pTrailerAnimExt->SetInvoker(pThis->Owner);

	return SkipGameCode;
}

// Inviso bullets behave differently in BulletClass::AI when their target is bullet and
// seemingly (at least partially) adopt characteristics of a vertical projectile.
// This is a potentially slightly hacky solution to that, as proper solution
// would likely require making sense of BulletClass::AI and ain't nobody got time for that.
DEFINE_HOOK(0x4668BD, BulletClass_AI_Interceptor_InvisoSkip, 0x6)
{
	enum { DetonateBullet = 0x467F9B };

	GET(BulletClass*, pThis, EBP);

	if (auto const pExt = BulletAITemp::ExtData)
	{
		if (pThis->Type->Inviso && pExt->InterceptorTechnoType)
			return DetonateBullet;
	}

	return 0;
}

#pragma region Gravity

#define APPLYGRAVITY(pType)\
auto const nGravity = BulletTypeExt::GetAdjustedGravity(pType);\
__asm { fld nGravity };\

DEFINE_HOOK(0x4671B9, BulletClass_AI_ApplyGravity, 0x6)
{
	GET(BulletTypeClass* const, pType, EAX);

	APPLYGRAVITY(pType);

	return 0x4671BF;
}

DEFINE_HOOK(0x6F7481, TechnoClass_Targeting_ApplyGravity, 0x6)
{
	GET(WeaponTypeClass* const, pWeaponType, EDX);

	APPLYGRAVITY(pWeaponType->Projectile);

	return 0x6F74A4;
}

DEFINE_HOOK(0x6FDAA6, TechnoClass_FireAngle_6FDA00_ApplyGravity, 0x5)
{
	GET(WeaponTypeClass* const, pWeaponType, EDI);

	APPLYGRAVITY(pWeaponType->Projectile);

	return 0x6FDACE;
}

DEFINE_HOOK(0x6FECB2, TechnoClass_FireAt_ApplyGravity, 0x6)
{
	GET(BulletTypeClass* const, pType, EAX);

	APPLYGRAVITY(pType);

	return 0x6FECD1;
}

DEFINE_HOOK_AGAIN(0x44D2AE, BuildingClass_Mission_Missile_ApplyGravity, 0x6)
DEFINE_HOOK_AGAIN(0x44D264, BuildingClass_Mission_Missile_ApplyGravity, 0x6)
DEFINE_HOOK(0x44D074, BuildingClass_Mission_Missile_ApplyGravity, 0x6)
{
	GET(WeaponTypeClass* const, pWeaponType, EBP);

	APPLYGRAVITY(pWeaponType->Projectile);

	switch (R->Origin())
	{
	case 0x44D074:
		return 0x44D07A;
		break;
	case 0x44D264:
		return 0x44D26A;
		break;
	case 0x44D2AE:
		return 0x44D2B4;
		break;
	default:
		__assume(0);
	}
}

#pragma endregion

namespace ShrapnelTemp
{
	BuildingClass* InitialTargetBuilding = nullptr;
	std::unordered_set<ObjectClass*> TargetsToIgnore;
}

DEFINE_HOOK(0x46A3D6, BulletClass_Shrapnel_Forced, 0xA)
{
	enum { Shrapnel = 0x46A40C, Skip = 0x46ADCD };

	GET(BulletClass*, pThis, EDI);

	auto const pTypeExt = BulletTypeExt::Fetch(pThis->Type);
	ShrapnelTemp::InitialTargetBuilding = nullptr;
	ShrapnelTemp::TargetsToIgnore.clear();

	if (auto const pObject = pThis->GetCell()->FirstObject)
	{
		auto const rtti = pObject->WhatAmI();

		if (rtti != AbstractType::Building)
		{
			return Shrapnel;
		}
		else if (pTypeExt->Shrapnel_AffectsBuildings.Get(RulesExt::Global()->Shrapnel_AffectsBuildings))
		{
			ShrapnelTemp::InitialTargetBuilding = static_cast<BuildingClass*>(pObject);
			return Shrapnel;
		}
	}
	else if (pTypeExt->Shrapnel_AffectsGround.Get(RulesExt::Global()->Shrapnel_AffectsGround))
	{
		return Shrapnel;
	}

	return Skip;
}

DEFINE_HOOK(0x46A4FB, BulletClass_Shrapnel_Targeting, 0x6)
{
	enum { SkipObject = 0x46A8EA, Continue = 0x46A50F };

	GET(BulletClass*, pThis, EDI);
	GET(ObjectClass*, pObject, EBP);
	GET(TechnoClass*, pSource, EAX);
	GET(WeaponTypeClass*, pShrapnelWeapon, ESI);

	auto const pTypeExt = BulletTypeExt::Fetch(pThis->Type);
	const bool isBuilding = pObject->WhatAmI() == AbstractType::Building;
	const bool ignorePreviouslyHit = pTypeExt->Shrapnel_IgnoreHitBuildings.Get(RulesExt::Global()->Shrapnel_IgnoreHitBuildings);

	if (isBuilding)
	{
		// Do not fire shrapnels on the building itself if bouncing off one.
		if (pObject == ShrapnelTemp::InitialTargetBuilding)
			return SkipObject;

		if (ignorePreviouslyHit && ShrapnelTemp::TargetsToIgnore.contains(pObject))
			return SkipObject;
	}

	auto const pOwner = pSource->Owner;

	if (pTypeExt->Shrapnel_UseWeaponTargeting.Get(RulesExt::Global()->Shrapnel_UseWeaponTargeting))
	{
		auto const pWeaponExt = WeaponTypeExt::Fetch(pShrapnelWeapon);
		auto const pType = pObject->GetType();

		if (!pType->LegalTarget)
			return SkipObject;

		if (!pWeaponExt->SkipWeaponPicking && !EnumFunctions::IsCellEligible(pObject->GetCell(), pWeaponExt->CanTarget, true, true))
			return SkipObject;

		auto const pWH = pShrapnelWeapon->Warhead;
		auto armorType = pType->Armor;

		if (auto const pTechno = abstract_cast<TechnoClass*, true>(pObject))
		{
			if (!pWeaponExt->SkipWeaponPicking)
			{
				if (!EnumFunctions::CanTargetHouse(pWeaponExt->CanTargetHouses, pOwner, pTechno->Owner) || !EnumFunctions::IsTechnoEligible(pTechno, pWeaponExt->CanTarget)
					|| !pWeaponExt->IsHealthInThreshold(pTechno) || !pWeaponExt->IsVeterancyInThreshold(pTechno) || !pWeaponExt->HasRequiredAttachedEffects(pTechno, pSource))
				{
					return SkipObject;
				}
			}

			auto const pShield = TechnoExt::Fetch(pTechno)->Shield.get();

			if (pShield && pShield->IsActive() && !pShield->CanBePenetrated(pWH))
				armorType = pShield->GetArmorType();
		}

		if (GeneralUtils::GetWarheadVersusArmor(pWH, armorType) == 0.0)
			return SkipObject;
	}
	else if (pOwner->IsAlliedWith(pObject))
	{
		return SkipObject;
	}

	if (isBuilding && ignorePreviouslyHit)
		ShrapnelTemp::TargetsToIgnore.insert(pObject);

	return Continue;
}

DEFINE_HOOK(0x46902C, BulletClass_Explode_Cluster, 0x6)
{
	enum { SkipGameCode = 0x469091 };

	GET(BulletClass*, pThis, ESI);
	REF_STACK(const CoordStruct, origCoords, STACK_OFFSET(0x3C, -0x30));

	auto const pTypeExt = BulletTypeExt::Fetch(pThis->Type);
	const int min = pTypeExt->ClusterScatter_Min.Get();
	const int max = pTypeExt->ClusterScatter_Max.Get();
	const double sigma = pTypeExt->Scatter_Sigma;
	const double aspect = pTypeExt->Scatter_Aspect;
	const CoordStruct flightDir = pThis->TargetCoords - pThis->SourceCoords;
	auto coords = origCoords;
	auto& random = ScenarioClass::Instance->Random;

	for (int i = 0; i < pThis->Type->Cluster; i++)
	{
		pThis->Detonate(coords);

		if (!pThis->IsAlive)
			break;

		const int raw = random.RandomRanged(min, max);
		const int distance = BulletExt::ShapeScatterRadius(raw, min, max, sigma);
		coords = MapClass::GetRandomCoordsNear(origCoords, distance, false);
		BulletExt::ApplyScatterAspect(coords, origCoords, flightDir, aspect);
	}

	return SkipGameCode;
}

constexpr bool CheckTrajectoryCanNotAlwaysSnap(const TrajectoryFlag flag)
{
	return flag != TrajectoryFlag::Invalid;
/*	return flag == TrajectoryFlag::Straight
		|| flag == TrajectoryFlag::Bombard
		|| flag == TrajectoryFlag::Missile
		|| flag == TrajectoryFlag::Engrave
		|| flag == TrajectoryFlag::Parabola
		|| flag == TrajectoryFlag::Tracing;*/
}

DEFINE_HOOK(0x467CCA, BulletClass_AI_TargetSnapChecks, 0x6)
{
	enum { SkipChecks = 0x467CDE };

	GET(BulletClass*, pThis, EBP);

	auto const pType = pThis->Type;

	// Do not require Airburst=no to check target snapping for Inviso / Trajectory=Straight projectiles
	if (pType->Inviso)
	{
		R->EAX(pType);
		return SkipChecks;
	}
	else if (auto const pExt = BulletAITemp::ExtData)
	{
		if (pExt->Trajectory && CheckTrajectoryCanNotAlwaysSnap(pExt->Trajectory->Flag()))
		{
			R->EAX(pType);
			return SkipChecks;
		}
	}

	return 0;
}

DEFINE_HOOK(0x468E61, BulletClass_Explode_TargetSnapChecks1, 0x6)
{
	enum { Snap = 0x468E7B, SkipChecks = 0x468FF4 };

	GET(BulletClass*, pThis, ESI);

	auto const pExt = BulletExt::Fetch(pThis);

	if (pExt->IsInstantDetonation)
		return SkipChecks;

	auto const pType = pThis->Type;

	// Do not require Airburst=no to check target snapping for Inviso / Trajectory=Straight projectiles
	if (pType->Inviso)
	{
		R->EAX(pType);
		return Snap;
	}
	else if (pType->Arcing || pType->ROT > 0)
	{
		return 0;
	}

	if (pExt->Trajectory && CheckTrajectoryCanNotAlwaysSnap(pExt->Trajectory->Flag()) && !pExt->SnappedToTarget)
	{
		R->EAX(pType);
		return Snap;
	}

	return 0;
}

DEFINE_HOOK(0x468E9F, BulletClass_Explode_TargetSnapChecks2, 0x6)
{
	enum { SkipInitialChecksOnly = 0x468EC7, SkipSetCoordinate = 0x468F23, SkipChecks = 0x468FF4 };

	GET(BulletClass*, pThis, ESI);

	auto const pExt = BulletExt::Fetch(pThis);

	if (pExt->IsInstantDetonation)
		return SkipChecks;

	auto const pType = pThis->Type;

	// Do not require EMEffect=no & Airburst=no to check target coordinate snapping for Inviso projectiles.
	if (pType->Inviso)
	{
		R->EAX(pType);
		return SkipInitialChecksOnly;
	}
	else if (pType->Arcing || pType->ROT > 0)
	{
		return 0;
	}

	// Do not force Trajectory=Straight projectiles to detonate at target coordinates under certain circumstances.
	// Fixes issues with walls etc.
	if (pExt->Trajectory && CheckTrajectoryCanNotAlwaysSnap(pExt->Trajectory->Flag()) && !pExt->SnappedToTarget)
		return SkipSetCoordinate;

	return 0;
}

DEFINE_HOOK(0x468D3F, BulletClass_ShouldExplode_AirTarget, 0x6)
{
	enum { SkipCheck = 0x468D73 };

	GET(BulletClass*, pThis, ESI);

	auto const pExt = BulletExt::Fetch(pThis);

	if (pExt->Trajectory && CheckTrajectoryCanNotAlwaysSnap(pExt->Trajectory->Flag()))
		return SkipCheck;

	return 0;
}

DEFINE_HOOK(0x4687F8, BulletClass_Unlimbo_FlakScatter, 0x6)
{
	GET(BulletClass*, pThis, EBX);
	GET_STACK(const float, mult, STACK_OFFSET(0x5C, -0x44));

	if (pThis->WeaponType)
	{
		auto const pTypeExt = BulletTypeExt::Fetch(pThis->Type);

		if (!(ScenarioClass::Instance->Random.RandomRanged(0, 100) <= pTypeExt->BallisticScatter_Chance * 100))
		{
			R->EAX(0);
			return 0;
		}

		const int defaultValue = RulesClass::Instance->BallisticScatter;
		int min = pTypeExt->BallisticScatter_Min.Get(Leptons(0));
		int max = pTypeExt->BallisticScatter_Max.Get(Leptons(defaultValue));
		int result = 0;

		if (pTypeExt->BallisticScatter_IncreaseByRange)
		{
			auto const pWeapon = pThis->WeaponType;
			const int minInMinRange = pTypeExt->BallisticScatter_Min_InMinRange.Get(Leptons(min));
			const int minInMaxRange = pTypeExt->BallisticScatter_Min_InMaxRange.Get(Leptons(min));
			const int maxInMinRange = pTypeExt->BallisticScatter_Max_InMinRange.Get(Leptons(max));
			const int maxInMaxRange = pTypeExt->BallisticScatter_Max_InMaxRange.Get(Leptons(max));
			const int minRange = pTypeExt->BallisticScatter_MinRange.Get(Leptons(pWeapon->MinimumRange));
			const int maxRange = pTypeExt->BallisticScatter_MaxRange.Get(Leptons(pWeapon->Range));
			const int deltaRange = maxRange - minRange;
			const int deltaRangeReal = static_cast<int>(mult) - minRange;
			const double rangePercent = Math::clamp((deltaRange == 0 ? 0.5 : deltaRangeReal / static_cast<double>(deltaRange)), 0, 1);
			min = minInMinRange + static_cast<int>(rangePercent * (minInMaxRange - minInMinRange));
			max = maxInMinRange + static_cast<int>(rangePercent * (maxInMaxRange - maxInMinRange));
			result = BulletExt::ShapeScatterRadius(ScenarioClass::Instance->Random.RandomRanged(min, max), min, max, pTypeExt->Scatter_Sigma);
		}
		else
		{
			const int raw = BulletExt::ShapeScatterRadius(ScenarioClass::Instance->Random.RandomRanged(2 * min, 2 * max), 2 * min, 2 * max, pTypeExt->Scatter_Sigma);
			result = static_cast<int>((mult * raw) / pThis->WeaponType->Range);
		}

		R->EAX(result);
	}

	return 0;
}

#pragma region UniversalScatter_FireAt

// TechnoClass::Fire_At holds a second, independent scatter implementation that the engine
// uses for *visible* projectiles (Inaccurate + Arcing, see 0x6FE663). The universal Sigma /
// Aspect shaping has to be applied there as well, otherwise visible projectiles would be
// left out. Projectiles driven by a Phobos trajectory are deliberately left alone here:
// their scatter is shaped by the trajectory code (ActualTrajectory::GetInaccurateTargetCoords).

namespace UniversalScatterFireAtTemp
{
	double Sigma = 1.0;
	double Aspect = 1.0;
	double Divergence = 1.0;
	double Distance = 0.0;      // 3D muzzle -> aim point distance of this shot
	int BaseX = 0;              // unscattered aim offset (muzzle -> target), X
	int BaseY = 0;              // ... Y
	int BaseZ = 0;              // ... Z
	int RawDraw = 0;            // radius after the Sigma / Min-Max remap, before distance scaling
	int Global = 0;             // [CombatDamage] -> BallisticScatter at capture time
	int VisibleMin = 0;         // radius interval for the FlakScatter (visible) branch
	int VisibleMax = 0;
	int LegacyMin = 0;          // radius interval for the legacy Inaccurate branch
	int LegacyMax = 0;
	bool ApplyDivergence = false;
}

// Range of the weapon that is actually firing the shot, in leptons, or 0 when it cannot be
// resolved. Called from the Fire_At scatter handlers, where ESI holds the firing TechnoClass.
//
// The engine normalises its own ramp by TechnoClass::GetWeaponRange(weaponIndex), but that is
// not always the firing weapon's reach: GetWeaponRange has a branch (taken for units whose
// GetTechnoType()+0x5E4 flag is set) that walks a list of attached turret weapons and returns
// the MINIMUM of their ranges. Measured in game with [LWBB] firing [LW380mm] (Range=19 cells):
// GetWeaponRange returned 1536 leptons (6 cells) while the firing weapon's Range is 4864, so
// 'distance / range' exceeded 1 on every shot and the clamp in ScatterDistanceFactor silently
// disabled Divergence for the whole unit. Use the weapon that is actually firing instead.
//
// The weapon index is read from exactly where the engine reads it for its own call, so no
// assumption about the frame layout is made beyond what the engine itself relies on.
static int GetFiringWeaponRange(REGISTERS* R)
{
	GET(TechnoClass*, pFirer, ESI);

	if (!pFirer)
		return 0;

	const int ebp = R->EBP<int>();
	const int weaponIndex = ebp ? *reinterpret_cast<const int*>(ebp + 0xc) : 0;

	if (auto const pWeapon = pFirer->GetWeapon(weaponIndex); pWeapon && pWeapon->WeaponType)
		return pWeapon->WeaponType->Range;

	return 0;
}

// Capture this projectile's scatter configuration for the remainder of the Fire_At call.
// ECX = BulletTypeClass*, overridden size 6: je 0x6FE8EE  (the !Inaccurate early-out)
//
// Placed at 0x6FE67D rather than 0x6FE663 because the Z component of the unscattered aim
// offset ([esp+0x38]) is only written at 0x6FE669, and all three components are needed for the
// 3D distance that drives Divergence. The engine writes that offset at 0x6FE643 / 0x6FE65D /
// 0x6FE669 and copies the very same values into crdOffset ([esp+0x94..0x9C]) at 0x6FE650 /
// 0x6FE66F / 0x6FE676, so these slots are the authoritative scatter centre - independent of
// BulletClass::TargetCoords and of whether Unlimbo has already run.
DEFINE_HOOK(0x6FE67D, TechnoClass_FireAt_ScatterCapture, 0x6)
{
	GET(BulletTypeClass*, pType, ECX);
	GET_STACK(int, baseX, STACK_OFFSET(0x30, 0));
	GET_STACK(int, baseY, STACK_OFFSET(0x34, 0));
	GET_STACK(int, baseZ, STACK_OFFSET(0x38, 0));

	const int global = RulesClass::Instance->BallisticScatter;

	UniversalScatterFireAtTemp::BaseX = baseX;
	UniversalScatterFireAtTemp::BaseY = baseY;
	UniversalScatterFireAtTemp::BaseZ = baseZ;
	UniversalScatterFireAtTemp::Global = global;

	const double dx = static_cast<double>(baseX);
	const double dy = static_cast<double>(baseY);
	const double dz = static_cast<double>(baseZ);
	UniversalScatterFireAtTemp::Distance = Math::sqrt(dx * dx + dy * dy + dz * dz);

	const auto pTypeExt = BulletTypeExt::Fetch(pType);

	if (pTypeExt->TrajectoryType)
	{
		// Handled by the trajectory path (ActualTrajectory::GetInaccurateTargetCoords);
		// leave the engine's own Fire_At scatter exactly as it was.
		UniversalScatterFireAtTemp::Sigma = 1.0;
		UniversalScatterFireAtTemp::Aspect = 1.0;
		UniversalScatterFireAtTemp::ApplyDivergence = false;
		UniversalScatterFireAtTemp::VisibleMin = 0;
		UniversalScatterFireAtTemp::VisibleMax = global;
		UniversalScatterFireAtTemp::LegacyMin = global / 2;
		UniversalScatterFireAtTemp::LegacyMax = global;
	}
	else
	{
		UniversalScatterFireAtTemp::Sigma = pTypeExt->Scatter_Sigma;
		UniversalScatterFireAtTemp::Aspect = pTypeExt->Scatter_Aspect;
		UniversalScatterFireAtTemp::Divergence = pTypeExt->GetScatterDivergence();

		// Divergence reshapes the distance ramp of the visible branch only. The legacy
		// Inaccurate branch is not distance-scaled by the engine at all, so it is left out.
		UniversalScatterFireAtTemp::ApplyDivergence = true;

		// BallisticScatter.Min / .Max now reach this path too, with the same meaning they
		// already have on the Unlimbo and trajectory paths: the FlakScatter branch defaults
		// to [0, global], the legacy Inaccurate branch to [global / 2, global]. The engine's
		// own draw interval is left untouched (the result is remapped afterwards), so with
		// both keys unset this branch stays bit-identical to the unmodified engine.
		UniversalScatterFireAtTemp::VisibleMin = pTypeExt->BallisticScatter_Min.Get(Leptons(0));
		UniversalScatterFireAtTemp::VisibleMax = pTypeExt->BallisticScatter_Max.Get(Leptons(global));
		UniversalScatterFireAtTemp::LegacyMin = pTypeExt->BallisticScatter_Min.Get(Leptons(global / 2));
		UniversalScatterFireAtTemp::LegacyMax = pTypeExt->BallisticScatter_Max.Get(Leptons(global));
	}

	return 0;
}

// Visible-projectile branch. Overridden size 6: covers 'fld DWORD PTR [esp+0x18]' (4 bytes)
// and 'mov ebx, eax' (2 bytes), so the trampoline resumes at 0x6FE722, an instruction
// boundary. A size of 4 would resume at 0x6FE721, i.e. inside 'mov ebx, eax' - that is the
// crash this hook is documented with.
//
// The draw that arrives in EAX depends on whether Ares is loaded, because Ares 3.0 / 3.0p1
// already hooks the same site (0x6FE709, size 6) and returns to this very address:
//
//   * without Ares: EAX = RandomRanged(0, global), so the engine's own window has to be
//     remapped onto the configured interval (with Sigma applied afterwards);
//   * with Ares:    EAX = RandomRanged(BallisticScatter.Min, BallisticScatter.Max) - Ares
//     reads the very same INI keys with the very same defaults. Remapping that draw onto the
//     configured interval a second time collapses the radius onto the interval endpoints and
//     degrades Sigma into a plain "go to Min or go to Max" selector, so only the shaping is
//     applied there.
//
// The resulting radius is kept in the temp so the distance shaping below can reuse it.
DEFINE_HOOK(0x6FE71C, TechnoClass_FireAt_ScatterSigma_Visible, 0x6)
{
	GET(int, raw, EAX);

	const int shaped = AresHelper::CanUseAres
		? BulletExt::ShapeScatterRadius(raw, UniversalScatterFireAtTemp::VisibleMin,
			UniversalScatterFireAtTemp::VisibleMax, UniversalScatterFireAtTemp::Sigma)
		: BulletExt::RemapScatterRadius(raw, 0, UniversalScatterFireAtTemp::Global,
			UniversalScatterFireAtTemp::VisibleMin, UniversalScatterFireAtTemp::VisibleMax,
			UniversalScatterFireAtTemp::Sigma);

	UniversalScatterFireAtTemp::RawDraw = shaped;
	R->EAX(shaped);

	return 0;
}

// Distance shaping for the visible branch. Reached only from there - the legacy branch jumps
// straight past it - and sits right after the engine divided by its own notion of the weapon
// range: EAX = RawDraw * distance / range, with ECX still holding that value because
// 'idiv ecx' leaves ECX untouched. The engine's result is discarded and recomputed with the
// Divergence ramp, which also caps the scale at 1 so shooting past the range cannot exceed Max.
//
// ECX is the return value of the virtual call at 0x6FE732, i.e. TechnoClass::GetWeaponRange,
// in leptons (the engine itself uses it as the divisor of 'radius * distance / range', and
// TechnoClass_GetGuardRange compares it against 1792).
//
// It is NOT, however, always the range of the weapon being fired: GetWeaponRange has a branch
// (taken for units whose GetTechnoType()+0x5E4 flag is set) that walks a list of attached
// turret weapons and returns the MINIMUM of their ranges. Measured in game: for a unit firing
// a projectile whose weapon range is 4864 leptons, GetWeaponRange returned 1536, which made
// 'distance / range' exceed 1 on every shot and silently disabled Divergence entirely. The ramp
// is therefore normalised by the range of the weapon that is actually firing, and ECX is only
// the fallback for when that cannot be resolved.
// Overridden size 6: mov edx, DWORD PTR ds:0x00A8B230
DEFINE_HOOK(0x6FE73F, TechnoClass_FireAt_ScatterDivergence, 0x6)
{
	if (!UniversalScatterFireAtTemp::ApplyDivergence)
		return 0;

	GET(int, rangeECX, ECX);
	int range = GetFiringWeaponRange(R);

	if (range <= 0)
		range = rangeECX;

	const double factor = BulletExt::ScatterDistanceFactor(
		UniversalScatterFireAtTemp::Distance, range, UniversalScatterFireAtTemp::Divergence);

	const int shaped = static_cast<int>(std::lround(UniversalScatterFireAtTemp::RawDraw * factor));

	R->EAX(shaped);

	return 0;
}

// Legacy 'Inaccurate' branch. Overridden size 6: mov edx, ds:0x00A8B230
//
// Same Ares caveat as the visible branch above, except that the engine's own window here is
// [global / 2, global] and Ares hooks 0x6FE7FE (size 5) returning to this address. With Ares
// loaded the draw already comes from BallisticScatter.Min / .Max, so only shaping is applied.
//
// This branch has no distance term anywhere between the draw and the polar offset (the engine
// never multiplies by the distance nor divides by the range here), so without the ramp added
// below the radius it applies would be the same at every range - the handover manual's P0-2.
// The ramp uses the same normaliser as the visible branch (the firing weapon's Range, via
// GetFiringWeaponRange) and is skipped for projectiles driven by a Phobos trajectory, which
// are handled by the trajectory code and must keep the engine's own scatter untouched.
DEFINE_HOOK(0x6FE821, TechnoClass_FireAt_ScatterSigma_Legacy, 0x6)
{
	GET(int, raw, EAX);

	const int shaped = AresHelper::CanUseAres
		? BulletExt::ShapeScatterRadius(raw, UniversalScatterFireAtTemp::LegacyMin,
			UniversalScatterFireAtTemp::LegacyMax, UniversalScatterFireAtTemp::Sigma)
		: BulletExt::RemapScatterRadius(raw, UniversalScatterFireAtTemp::Global / 2,
			UniversalScatterFireAtTemp::Global,
			UniversalScatterFireAtTemp::LegacyMin, UniversalScatterFireAtTemp::LegacyMax,
			UniversalScatterFireAtTemp::Sigma);

	// No distance term in the engine on this branch, so add one. The interval keeps its
	// existing defaults ([global / 2, global] without BallisticScatter.Min / .Max): at the
	// weapon's full range the result is exactly what this branch produced before.
	double factor = 1.0;
	int result = shaped;

	if (UniversalScatterFireAtTemp::ApplyDivergence)
	{
		if (const int range = GetFiringWeaponRange(R); range > 0)
		{
			factor = BulletExt::ScatterDistanceFactor(
				UniversalScatterFireAtTemp::Distance, range, UniversalScatterFireAtTemp::Divergence);

			result = static_cast<int>(std::lround(shaped * factor));
		}
	}

	R->EAX(result);

	return 0;
}

#pragma endregion

// Ellipse shaping for the Fire_At scatter. Applied at the exit of the engine's scatter block,
// i.e. BEFORE the initial facing (0x6FE902 reads crdOffset) and the firing solution are
// derived from crdOffset. That ordering is what makes the change seamless: facing, velocity
// and destination all end up agreeing with the elliptical offset.
//
// It must NOT be done later, e.g. in TechnoClass_Fire_BeforeMoveTo (0x6FF008): the only
// consumer there is ApplyArcingFix, which is skipped whenever Arcing.AllowElevationInaccuracy
// is true - and that is the global default - so the correction would silently do nothing.
//
// The scatter block is entered only for Inaccurate+Arcing. Every other path jumps straight to
// this address with crdOffset still equal to the captured base, so the call degenerates to a
// no-op on its own; the geometry is the gate. Inviso projectiles are excluded on purpose:
// they scatter again inside BulletClass::Unlimbo, and that site applies the ellipse instead,
// so no bullet ever gets the aspect ratio applied twice.
// Overridden size 6: mov edx, DWORD PTR [ecx+0x2DC]
DEFINE_HOOK(0x6FE8EE, TechnoClass_FireAt_ScatterAspect, 0x6)
{
	GET(BulletTypeClass*, pType, ECX);

	// Skip exactly the combination that BulletClass::Unlimbo scatters (and therefore
	// shapes) itself, so that no bullet ever gets the aspect ratio applied twice.
	if ((pType->FlakScatter && pType->Inviso) || UniversalScatterFireAtTemp::Aspect == 1.0)
		return 0;

	REF_STACK(CoordStruct, crdOffset, STACK_OFFSET(0x94, 0));

	const CoordStruct base { UniversalScatterFireAtTemp::BaseX, UniversalScatterFireAtTemp::BaseY, crdOffset.Z };

	// base doubles as the flight direction: it is the muzzle -> aim point vector.
	BulletExt::ApplyScatterAspect(crdOffset, base, base, UniversalScatterFireAtTemp::Aspect);

	return 0;
}

// Skip a forced detonation check for Level=true projectiles that is now handled in Hooks.Obstacles.cpp.
DEFINE_JUMP(LJMP, 0x468D08, 0x468D2F);

DEFINE_HOOK(0x6FF008, TechnoClass_Fire_BeforeMoveTo, 0x8)
{
	GET(BulletClass* const, pBullet, EBX);

	const auto pBulletType = pBullet->Type;

	if (pBulletType->Arcing && !BulletTypeExt::Fetch(pBulletType)->Arcing_AllowElevationInaccuracy.Get(RulesExt::Global()->Arcing_AllowElevationInaccuracy))
	{
		REF_STACK(BulletVelocity, velocity, STACK_OFFSET(0xB0, -0x60));
		REF_STACK(const CoordStruct, crdSrc, STACK_OFFSET(0xB0, -0x6C));
		REF_STACK(const CoordStruct, crdOffset, STACK_OFFSET(0xB0, -0x1C));
		REF_STACK(const CoordStruct, fireCoords, STACK_OFFSET(0xB0, -0x6C));

		const auto crdTgt = crdOffset + fireCoords;
		BulletExt::ApplyArcingFix(pBullet, crdSrc, crdTgt, velocity);
	}

	return 0;
}

DEFINE_HOOK(0x44D46E, BuildingClass_Mission_Missile_BeforeMoveTo, 0x8)
{
	GET(BulletClass* const, pBullet, EDI);

	const auto pBulletType = pBullet->Type;

	if (pBulletType->Arcing && !BulletTypeExt::Fetch(pBulletType)->Arcing_AllowElevationInaccuracy.Get(RulesExt::Global()->Arcing_AllowElevationInaccuracy))
	{
		REF_STACK(BulletVelocity, velocity, STACK_OFFSET(0xE8, -0xD0));
		REF_STACK(const CoordStruct, crdSrc, STACK_OFFSET(0xE8, -0x8C));
		REF_STACK(const CoordStruct, crdTgt, STACK_OFFSET(0xE8, -0x4C));

		BulletExt::ApplyArcingFix(pBullet, crdSrc, crdTgt, velocity);
	}

	return 0;
}

#pragma region Parabombs

// Patch out Ares parabomb implementation.
DEFINE_PATCH(0x46867F, 0x6A, 0x00, 0x8B, 0xD9, 0x50);

// Add in our own.
static bool __fastcall ObjectClass_Unlimbo_Parachuted_Wrapper(BulletClass* pThis, void*, const CoordStruct& coords, DirType facing)
{
	auto const pTypeExt = BulletTypeExt::Fetch(pThis->Type);

	if (pTypeExt->Parachuted)
		return pThis->SpawnParachuted(coords);

	return pThis->Unlimbo(coords, facing);
}

DEFINE_FUNCTION_JUMP(CALL, 0x468684, ObjectClass_Unlimbo_Parachuted_Wrapper);

// Set parachute animation on projectile.
DEFINE_HOOK(0x5F5A62, ObjectClass_SpawnParachuted_BombParachute, 0x5)
{
	enum { SkipGameCode = 0x5F5A99 };

	GET(BulletClass*, pThis, ESI);
	GET(CoordStruct*, coords, EDI);

	auto const pTypeExt = BulletTypeExt::Fetch(pThis->Type);
	auto const pAnimType = pTypeExt->BombParachute.Get(RulesClass::Instance->BombParachute);
	AnimClass* pAnim = nullptr;

	if (pAnimType)
	{
		pAnim = GameCreate<AnimClass>(pAnimType, *coords);
		pAnim->Owner = pThis->Owner ? pThis->Owner->Owner : BulletExt::Fetch(pThis)->FirerHouse;
		const int schemeIndex = pAnim->Owner ? pAnim->Owner->ColorSchemeIndex : RulesExt::Global()->AnimRemapDefaultColorScheme;
		pAnim->LightConvert = ColorScheme::Array[schemeIndex]->LightConvert;
		pThis->Parachute = pAnim;
	}

	R->EAX(pAnim);
	return SkipGameCode;
}

DEFINE_HOOK(0x467AB2, BulletClass_AI_Parabomb, 0x7)
{
	enum { SkipGameCode = 0x467B1A };

	GET(BulletClass*, pThis, EBP);

	if (pThis->HasParachute)
		return SkipGameCode;

	return 0;
}

#pragma endregion

DEFINE_HOOK(0x4683F2, BulletClass_Draw_ZAdjust, 0x5)
{
	GET(BulletClass*, pThis, ESI);
	GET(const int, height, ECX);

	auto const pTypeExt = BulletTypeExt::Fetch(pThis->Type);

	R->EAX(TacticalClass::AdjustForZ(height) - pTypeExt->ZAdjust);

	return 0x4683F7;
}

// Replaces Ares' handling of Ranged=true projectiles.
DEFINE_HOOK(0x467B8E, BulletClass_AI_Ranged, 0x6)
{
	GET(BulletClass*, pThis, EBP);
	REF_STACK(CoordStruct, coordNew, STACK_OFFSET(0x1AC, -0x184));
	REF_STACK(bool, shouldExplode, STACK_OFFSET(0x1AC, -0x190));

	if (pThis->Type->Ranged)
	{
		auto const pExt = BulletExt::Fetch(pThis);
		pExt->DistanceTraveled += Game::F2I(coordNew.DistanceFrom(pThis->GetCoords()));
		int maxRange = pThis->Range;

		if (maxRange > 0 && pThis->WeaponType && pThis->Owner
			&& WeaponTypeExt::Fetch(pThis->WeaponType)->ProjectileRange_ApplyModifiers)
		{
			maxRange = WeaponTypeExt::GetRangeWithModifiers(pThis->WeaponType, pThis->Owner, maxRange);
		}

		shouldExplode |= pExt->DistanceTraveled >= maxRange;
	}

	pThis->SetLocation(coordNew);
	return 0;
}

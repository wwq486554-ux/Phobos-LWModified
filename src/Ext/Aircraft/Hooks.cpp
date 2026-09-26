#include "Body.h"

#include <Kamikaze.h>
#include <cmath>

#include <EventClass.h>

#include <Ext/Aircraft/AdvancedMissions.h>
#include <Ext/AircraftType/Body.h>
#include <Ext/Anim/Body.h>
#include <Ext/WeaponType/Body.h>
#include <Ext/BulletType/Body.h>

#pragma region Mission_Attack

DEFINE_HOOK(0x417FF1, AircraftClass_Mission_Attack_StrafeShots, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	auto const pExt = AircraftExt::Fetch(pThis);
	AirAttackStatus const state = (AirAttackStatus)pThis->MissionStatus;

	// Re-evaluate weapon choice due to potentially changing targeting conditions here
	// only when aircraft is adjusting position or picking attack location.
	// This choice is also re-evaluated again every time just before firing UNLESS mid strafing run.
	// See AircraftClass_SelectWeapon_Wrapper and the call redirects below for this.
	if (state > AirAttackStatus::ValidateAZ && state < AirAttackStatus::FireAtTarget)
		pExt->CurrentAircraftWeaponIndex = Math::max(pThis->SelectWeapon(pThis->Target), 0);

	if (state < AirAttackStatus::FireAtTarget2_Strafe
		|| state > AirAttackStatus::FireAtTarget5_Strafe)
	{
		pExt->Strafe_BombsDroppedThisRound = 0;
	}

	// No need to evaluate this before any strafing shots have been fired.
	if (pExt->Strafe_BombsDroppedThisRound)
	{
		auto const pWeapon = pThis->GetWeapon(pExt->CurrentAircraftWeaponIndex)->WeaponType;
		auto const pWeaponExt = WeaponTypeExt::Fetch(pWeapon);
		int const strafingShots = pWeaponExt->Strafing_Shots.Get(5);

		if (strafingShots > 5)
		{
			if (state == AirAttackStatus::FireAtTarget3_Strafe)
			{
				int const remainingShots = strafingShots - 3 - pExt->Strafe_BombsDroppedThisRound;

				if (remainingShots > 0)
					pThis->MissionStatus = (int)AirAttackStatus::FireAtTarget2_Strafe;
			}
		}
	}

	return 0;
}

DEFINE_HOOK(0x4197FC, AircraftClass_GetFireLocation_WeaponRange, 0x6)
{
	enum { SkipGameCode = 0x419808 };

	GET(AircraftClass*, pThis, EDI);

	auto const pExt = AircraftExt::Fetch(pThis);
	R->EAX(pThis->GetWeaponRange(pExt->CurrentAircraftWeaponIndex));

	return SkipGameCode;
}

// If strafing weapon target is in air, consider the cell it is on as the firing position instead of the object itself if can fire at it.
DEFINE_HOOK(0x4197F3, AircraftClass_GetFireLocation_Strafing, 0x5)
{
	GET(AircraftClass*, pThis, EDI);
	GET(AbstractClass*, pTarget, EAX);

	// pTarget can be nullptr
	auto const pObject = abstract_cast<ObjectClass*>(pTarget);

	if (!pObject || !pObject->IsInAir())
		return 0;

	auto const fireError = pThis->GetFireError(pTarget, AircraftExt::Fetch(pThis)->CurrentAircraftWeaponIndex, false);

	if (fireError == FireError::ILLEGAL || fireError == FireError::CANT)
		return 0;

	R->EAX(MapClass::Instance.GetCellAt(pObject->GetCoords()));

	return 0;
}

static long __stdcall AircraftClass_IFlyControl_IsStrafe(IFlyControl const* ifly)
{
	__assume(ifly != nullptr);

	auto const pThis = static_cast<AircraftClass const*>(ifly);
	auto const pExt = AircraftExt::Fetch(pThis);
	WeaponTypeClass* pWeapon = nullptr;

	pWeapon = pThis->GetWeapon(pExt->CurrentAircraftWeaponIndex)->WeaponType;

	if (pWeapon)
	{
		auto const pWeaponExt = WeaponTypeExt::Fetch(pWeapon);
		auto const pBulletType = pWeapon->Projectile;
		return pWeaponExt->Strafing.isset() ? pWeaponExt->Strafing.Get() : (pBulletType->ROT <= 1 && !pBulletType->Inviso && !BulletTypeExt::Fetch(pBulletType)->TrajectoryType);
	}

	return false;
}

DEFINE_FUNCTION_JUMP(VTABLE, 0x7E2268, AircraftClass_IFlyControl_IsStrafe);

DEFINE_HOOK(0x4180F4, AircraftClass_Mission_Attack_WeaponRange, 0x5)
{
	enum { SkipGameCode = 0x4180FF };

	GET(AircraftClass*, pThis, ESI);

	R->EAX(pThis->GetWeapon(AircraftExt::Fetch(pThis)->CurrentAircraftWeaponIndex));

	return SkipGameCode;
}

DEFINE_HOOK(0x418403, AircraftClass_Mission_Attack_FireAtTarget_BurstFix, 0x8)
{
	GET(AircraftClass*, pThis, ESI);

	pThis->ShouldLoseAmmo = true;

	AircraftExt::FireWeapon(pThis, pThis->Target);

	return 0x418478;
}

DEFINE_HOOK(0x4186B6, AircraftClass_Mission_Attack_FireAtTarget2_BurstFix, 0x8)
{
	GET(AircraftClass*, pThis, ESI);

	AircraftExt::FireWeapon(pThis, pThis->Target);

	return 0x4186D7;
}

DEFINE_HOOK(0x418805, AircraftClass_Mission_Attack_FireAtTarget2Strafe_BurstFix, 0x8)
{
	GET(AircraftClass*, pThis, ESI);

	AircraftExt::FireWeapon(pThis, pThis->Target);

	return 0x418826;
}

DEFINE_HOOK(0x418914, AircraftClass_Mission_Attack_FireAtTarget3Strafe_BurstFix, 0x8)
{
	GET(AircraftClass*, pThis, ESI);

	AircraftExt::FireWeapon(pThis, pThis->Target);

	return 0x418935;
}

DEFINE_HOOK(0x418A23, AircraftClass_Mission_Attack_FireAtTarget4Strafe_BurstFix, 0x8)
{
	GET(AircraftClass*, pThis, ESI);

	AircraftExt::FireWeapon(pThis, pThis->Target);

	return 0x418A44;
}

DEFINE_HOOK(0x418B1F, AircraftClass_Mission_Attack_FireAtTarget5Strafe_BurstFix, 0x8)
{
	GET(AircraftClass*, pThis, ESI);

	AircraftExt::FireWeapon(pThis, pThis->Target);

	return 0x418B40;
}

static int __fastcall AircraftClass_SelectWeapon_Wrapper(AircraftClass* pThis, void* _, AbstractClass* pTarget)
{
	auto const pExt = AircraftExt::Fetch(pThis);

	// Re-evaluate weapon selection only if not mid-strafing run before firing.
	if (!pExt->Strafe_BombsDroppedThisRound)
		pExt->CurrentAircraftWeaponIndex = Math::max(pThis->SelectWeapon(pTarget), 0);

	return pExt->CurrentAircraftWeaponIndex;
}

DEFINE_FUNCTION_JUMP(CALL6, 0x41831E, AircraftClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(CALL6, 0x4185F5, AircraftClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(CALL6, 0x4187C4, AircraftClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(CALL6, 0x4188D3, AircraftClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(CALL6, 0x4189E2, AircraftClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(CALL6, 0x418AF1, AircraftClass_SelectWeapon_Wrapper);

DEFINE_HOOK_AGAIN(0x41874E, AircraftClass_Mission_Attack_StrafingDestinationFix, 0x6)
DEFINE_HOOK(0x418544, AircraftClass_Mission_Attack_StrafingDestinationFix, 0x6)
{
	GET(const FireError, fireError, EAX);
	GET(AircraftClass*, pThis, ESI);

	// The aircraft managed by the spawn manager will not update destination after changing target
	if (fireError == FireError::RANGE && pThis->Is_Strafe())
		pThis->SetDestination(pThis->Target, true);

	return 0;
}

#pragma region After_Shot_Delays

static inline int GetDelay(AircraftClass* pThis, bool isLastShot)
{
	auto const pExt = AircraftExt::Fetch(pThis);
	auto const pWeapon = pThis->GetWeapon(pExt->CurrentAircraftWeaponIndex)->WeaponType;
	auto const pWeaponExt = WeaponTypeExt::Fetch(pWeapon);
	int delay = pWeapon->ROF;

	if (isLastShot || pExt->Strafe_BombsDroppedThisRound == pWeaponExt->Strafing_Shots.Get(5)
		|| (pWeaponExt->Strafing_UseAmmoPerShot.Get(RulesExt::Global()->Strafing_UseAmmoPerShot) && !pThis->Ammo))
	{
		pExt->Strafe_TargetCell = nullptr;
		pThis->MissionStatus = (int)AirAttackStatus::FlyToPosition;
		delay = pWeaponExt->Strafing_EndDelay.isset() ? pWeaponExt->Strafing_EndDelay.Get() : ((pWeapon->Range + (Unsorted::LeptonsPerCell * 4)) / pThis->Type->Speed);
	}

	return delay;
}

DEFINE_HOOK(0x4184CC, AircraftClass_Mission_Attack_Delay1A, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	auto const pExt = AircraftExt::Fetch(pThis);

	if (WeaponTypeExt::Fetch(pThis->GetWeapon(pExt->CurrentAircraftWeaponIndex)->WeaponType)->Strafing_TargetCell.Get(RulesExt::Global()->Strafing_TargetCell))
		pExt->Strafe_TargetCell = MapClass::Instance.GetCellAt(pThis->Target->GetCoords());

	pThis->IsLocked = true;
	pThis->MissionStatus = (int)AirAttackStatus::FireAtTarget2_Strafe;
	R->EAX(GetDelay(pThis, false));

	return 0x4184F1;
}

DEFINE_HOOK(0x418506, AircraftClass_Mission_Attack_Delay1B, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	pThis->IsLocked = true;
	pThis->MissionStatus = pThis->Ammo > 0 ? (int)AirAttackStatus::PickAttackLocation : (int)AirAttackStatus::ReturnToBase;
	R->EAX(GetDelay(pThis, false));

	return 0x418539;
}

DEFINE_HOOK(0x418883, AircraftClass_Mission_Attack_Delay2, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	pThis->MissionStatus = (int)AirAttackStatus::FireAtTarget3_Strafe;
	R->EAX(GetDelay(pThis, false));

	return 0x4188A1;
}

DEFINE_HOOK(0x418992, AircraftClass_Mission_Attack_Delay3, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	pThis->MissionStatus = (int)AirAttackStatus::FireAtTarget4_Strafe;
	R->EAX(GetDelay(pThis, false));

	return 0x4189B0;
}

DEFINE_HOOK(0x418AA1, AircraftClass_Mission_Attack_Delay4, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	pThis->MissionStatus = (int)AirAttackStatus::FireAtTarget5_Strafe;
	R->EAX(GetDelay(pThis, false));

	return 0x418ABF;
}

DEFINE_HOOK(0x418B8A, AircraftClass_Mission_Attack_Delay5, 0x6)
{
	GET(AircraftClass*, pThis, ESI);

	R->EAX(GetDelay(pThis, true));

	return 0x418BBA;
}

#pragma endregion

#pragma region StrafeCell

DEFINE_HOOK_AGAIN(0x4188AC, AircraftClass_Mission_Attack_StrafeCell, 0x6)
DEFINE_HOOK_AGAIN(0x4189BB, AircraftClass_Mission_Attack_StrafeCell, 0x6)
DEFINE_HOOK_AGAIN(0x418ACA, AircraftClass_Mission_Attack_StrafeCell, 0x6)
DEFINE_HOOK(0x41879D, AircraftClass_Mission_Attack_StrafeCell, 0x6)
{
	enum { CannotFireNow = 0x418BC5, SkipGameCode = 0x418BBA };

	GET(AircraftClass*, pThis, ESI);

	const auto pExt = AircraftExt::Fetch(pThis);

	if (const auto pTargetCell = pExt->Strafe_TargetCell)
	{
		switch (pThis->GetFireError(pTargetCell, pExt->CurrentAircraftWeaponIndex, true))
		{
		case FireError::OK:
		case FireError::FACING:
		case FireError::CLOAKED:
		case FireError::RANGE:
			break;
		default:
			return CannotFireNow;
		}

		AircraftExt::FireWeapon(pThis, pTargetCell);

		if (AircraftTypeExt::Fetch(pThis->Type)->FiringForceScatter.Get(RulesExt::Global()->AircraftFiringForceScatter))
			pTargetCell->ScatterContent(pThis->Location, true, false, false);

		pThis->SetDestination(pTargetCell, true);
		pThis->MissionStatus++;

		R->EAX(GetDelay(pThis, pThis->MissionStatus > static_cast<int>(AirAttackStatus::FireAtTarget5_Strafe)));
		return SkipGameCode;
	}

	return 0;
}

#pragma endregion

#pragma region ScatterCell

DEFINE_HOOK_AGAIN(0x41882C, AircraftClass_MissionAttack_ScatterCell1, 0x6)
DEFINE_HOOK_AGAIN(0x41893B, AircraftClass_MissionAttack_ScatterCell1, 0x6)
DEFINE_HOOK_AGAIN(0x418A4A, AircraftClass_MissionAttack_ScatterCell1, 0x6)
DEFINE_HOOK_AGAIN(0x418B46, AircraftClass_MissionAttack_ScatterCell1, 0x6)
DEFINE_HOOK(0x41847E, AircraftClass_MissionAttack_ScatterCell1, 0x6)
{
	GET(AircraftClass*, pThis, ESI);
	return AircraftTypeExt::Fetch(pThis->Type)->FiringForceScatter.Get(RulesExt::Global()->AircraftFiringForceScatter) ? 0 : (R->Origin() + 0x44);
}

DEFINE_HOOK(0x4186DD, AircraftClass_MissionAttack_ScatterCell2, 0x5)
{
	GET(AircraftClass*, pThis, ESI);
	return AircraftTypeExt::Fetch(pThis->Type)->FiringForceScatter.Get(RulesExt::Global()->AircraftFiringForceScatter) ? 0 : (R->Origin() + 0x43);
}

#pragma endregion

DEFINE_HOOK(0x414F10, AircraftClass_AI_Trailer, 0x5)
{
	enum { SkipGameCode = 0x414F47 };

	GET(AircraftClass*, pThis, ESI);
	REF_STACK(const CoordStruct, coords, STACK_OFFSET(0x40, -0xC));

	auto const pTrailerAnim = GameCreate<AnimClass>(pThis->Type->Trailer, coords, 1, 1);
	auto const pTrailerAnimExt = AnimExt::Fetch(pTrailerAnim);
	AnimExt::SetAnimOwnerHouseKind(pTrailerAnim, pThis->Owner, nullptr, false, true);
	pTrailerAnimExt->SetInvoker(pThis);
	pTrailerAnimExt->IsTechnoTrailerAnim = true;

	return SkipGameCode;
}

DEFINE_HOOK(0x414C0B, AircraftClass_ChronoSparkleDelay, 0x5)
{
	R->ECX(RulesExt::Global()->ChronoSparkleDisplayDelay);
	return 0x414C10;
}

#pragma region LandingDir

DEFINE_HOOK(0x4CF31C, FlyLocomotionClass_FlightUpdate_LandingDir, 0x9)
{
	enum { SkipGameCode = 0x4CF3D0, SetSecondaryFacing = 0x4CF351 };

	GET(FootClass** const, pFootPtr, ESI);
	GET_STACK(IFlyControl* const, iFly, STACK_OFFSET(0x48, -0x38));
	REF_STACK(unsigned int, dir, STACK_OFFSET(0x48, 0x8));

	const auto pFoot = *pFootPtr;
	dir = 0;

	if (!iFly)
		return SetSecondaryFacing;

	if (iFly->Is_Locked())
		return SkipGameCode;

	if (const auto pAircraft = abstract_cast<AircraftClass*, true>(pFoot))
		dir = DirStruct(AircraftExt::GetLandingDir(pAircraft)).Raw;
	else
		dir = (iFly->Landing_Direction() << 13);

	return SetSecondaryFacing;
}

namespace SeparateAircraftTemp
{
	BuildingClass* pBuilding = nullptr;
}

DEFINE_HOOK(0x446F57, BuildingClass_GrandOpening_PoseDir_SetContext, 0x6)
{
	GET(BuildingClass*, pThis, EBP);

	SeparateAircraftTemp::pBuilding = pThis;

	return 0;
}

static DirType __fastcall AircraftClass_PoseDir_Wrapper(AircraftClass* pThis)
{
	return AircraftExt::GetLandingDir(pThis, SeparateAircraftTemp::pBuilding, true);
}
DEFINE_FUNCTION_JUMP(CALL, 0x446F67, AircraftClass_PoseDir_Wrapper); // BuildingClass_GrandOpening

DEFINE_HOOK(0x443FC7, BuildingClass_ExitObject_PoseDir1, 0x8)
{
	GET(BuildingClass*, pThis, ESI);
	GET(AircraftClass*, pAircraft, EBP);

	R->EAX(AircraftExt::GetLandingDir(pAircraft, pThis));

	return 0;
}

DEFINE_HOOK(0x44402E, BuildingClass_ExitObject_PoseDir2, 0x5)
{
	GET(BuildingClass*, pThis, ESI);
	GET(AircraftClass*, pAircraft, EBP);

	auto const dir = DirStruct(AircraftExt::GetLandingDir(pAircraft, pThis, true));

	if (AircraftTypeExt::Fetch(pAircraft->Type)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions))
		pAircraft->PrimaryFacing.SetCurrent(dir);

	pAircraft->SecondaryFacing.SetCurrent(dir);

	return 0;
}

#pragma endregion

DEFINE_HOOK(0x415EEE, AircraftClass_Fire_KickOutPassengers, 0x6)
{
	enum { SkipKickOutPassengers = 0x415F08 };

	GET(AircraftClass*, pThis, EDI);
	GET_BASE(const int, weaponIdx, 0xC);

	auto const pWeapon = pThis->GetWeapon(weaponIdx)->WeaponType;

	if (!pWeapon)
		return 0;

	auto const pWeaponExt = WeaponTypeExt::Fetch(pWeapon);

	if (pWeaponExt->KickOutPassengers.Get(RulesExt::Global()->AircraftWeapon_KickOutPassengers))
		return 0;

	return SkipKickOutPassengers;
}

// Aircraft mission hard code are all disposable that no ammo, target died or arrived destination all will call the aircraft return airbase
#pragma region ExtendedAircraftMissions

// Waypoint: enable and smooth moving action
static bool __fastcall AircraftTypeClass_CanUseWaypoint(AircraftTypeClass* pThis)
{
	return AircraftTypeExt::Fetch(pThis)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions);
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E2908, AircraftTypeClass_CanUseWaypoint)

// 机体自然转弯半径的公式只保留一处：AdvancedMissions::GetTurningRadius()
// （定义在 AdvancedMissions.cpp），下面三处调用直接写全名。

// Move: smooth the planning paths and returning route
DEFINE_HOOK_AGAIN(0x4168C7, AircraftClass_Mission_Move_SmoothMoving, 0x5)
DEFINE_HOOK(0x416A0A, AircraftClass_Mission_Move_SmoothMoving, 0x5)
{
	enum { EnterIdleAndReturn = 0x416AC0, ContinueMoving1 = 0x416908, ContinueMoving2 = 0x416A47 };

	GET(AircraftClass* const, pThis, ESI);
	GET(CoordStruct const* const, pCoords, EAX);

	if (pThis->Team || pThis->Airstrike || pThis->IsALoaner)
		return 0;

	const auto pType = pThis->Type;

	if (!pType->AirportBound)
		return 0;

	const bool extendedMissions = AircraftTypeExt::Fetch(pType)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions);

	if (!AircraftTypeExt::Fetch(pType)->ExtendedAircraftMissions_SmoothMoving.Get(extendedMissions))
		return 0;

	const int distance = static_cast<int>(Point2D { pCoords->X, pCoords->Y }.DistanceFrom(Point2D { pThis->Location.X, pThis->Location.Y }));

	// When the horizontal distance between the aircraft and its destination is greater than half of its deceleration distance
	// or its turning radius, continue to move forward, otherwise return to airbase or execute the next planning waypoint
	const int turningRadius = AdvancedMissions::GetTurningRadius(pThis);

	if (distance > std::max((pType->SlowdownDistance / 2), turningRadius))
		return (R->Origin() == 0x4168C7 ? ContinueMoving1 : ContinueMoving2);

	// AdvancedAircraftMissions: loiter on arrival instead of returning to base.
	//
	// Only reached when the aircraft is already within max(SlowdownDistance/2, turning radius)
	// of its destination. A planning waypoint chain is still followed first: the next node has
	// to be consumed exactly once here (the vanilla code below relies on the same short-circuit
	// order), and only the true end of the chain - or a plain single move order - starts the
	// loiter. Everything else keeps the pre-existing behaviour untouched.
	if (extendedMissions && AdvancedMissions::Enabled(pType))
	{
		const bool hasNextPlanningNode = pThis->TryNextPlanningTokenNode();
		const bool chainFinished = !hasNextPlanningNode && pThis->QueuedMission != Mission::Area_Guard;

		if (chainFinished)
		{
			// AdvancedAircraftMissions: "把飞机已经停在的那个格子再点一次"是一次
			// 零距离移动订单。引擎可能把目的地改成飞机自身、或在到位后不再保留
			// 目的地，于是 TryBeginLoiter(pThis->Destination) 失败，代码就掉进
			// EnterIdleMode —— 也就是"已经悬停的飞机被再点一次就自己飞回家"
			// （实机症状）。盘旋机型在这种原地订单下应当继续盘旋，所以目的地
			// 不合法时依次回退到"原盘旋圆心"和"当前所在格"，绝不让它返航。
			AbstractClass* pCenter = pThis->Destination;

			if (!pCenter || pCenter == pThis)
				pCenter = pThis->ArchiveTarget;

			if (!pCenter || pCenter == pThis)
				pCenter = MapClass::Instance.TryGetCellAt(pThis->GetCoords());

			if (pCenter && AdvancedMissions::TryBeginLoiter(pThis, pCenter))
				return EnterIdleAndReturn;

			pThis->EnterIdleMode(false, true);
		}

		return EnterIdleAndReturn;
	}

	// Try next planning waypoint first, then return to air base if it does not exist or cannot be taken
	if (!extendedMissions || (!pThis->TryNextPlanningTokenNode() && pThis->QueuedMission != Mission::Area_Guard))
		pThis->EnterIdleMode(false, true);

	return EnterIdleAndReturn;
}

// AdvancedAircraftMissions: Mission_Move 的到位收尾（MissionStatus == 3）。
//
// 与 0x4168C7 / 0x416A0A 那两个钩子同一套判据，但覆盖另一条**绕过它们**的路径：
// status 2 里引擎先判"飞控还在动吗"（0x4168A7），**不动就把 MissionStatus 直接置 3**
// （0x4168AC → 0x416AB6），于是 status 3 再判一次不动就 `EnterIdleMode`（0x4169B2）
// → 目的地改写成机场 → 回家。目的地 == 当前所在格时飞控不会动，正好落进这条。
//
// 它和飞控里那个"放弃判定"（见下面的 FlyLocomotion_MoveTo_AbortReturn）是同一个
// 实机症状的两条腿：飞控那条先被拦住，拦完之后流程走到这里，由这个钩子重新开盘旋。
// 跳到 0x416AC0 是 Mission_Move 自己的收尾（`pop edi/pop esi/pop ebp/mov eax,1/.../ret`），
// 栈是平衡的 —— 这一点和上面那个飞控钩子不同，别照抄。
DEFINE_HOOK(0x41698E, AircraftClass_Mission_Move_StoppedArrival, 0x6)
{
	enum { EnterIdleAndReturn = 0x416AC0 };

	GET(AircraftClass* const, pThis, ESI);

	if (pThis->Team || pThis->Airstrike || pThis->IsALoaner)
		return 0;

	const auto pType = pThis->Type;

	if (!pType->AirportBound)
		return 0;

	const bool extendedMissions = AircraftTypeExt::Fetch(pType)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions);

	if (!AircraftTypeExt::Fetch(pType)->ExtendedAircraftMissions_SmoothMoving.Get(extendedMissions))
		return 0;

	if (extendedMissions && AdvancedMissions::Enabled(pType) && pThis->Destination)
	{
		// 与 0x4168C7 相同的"到位"判据：只有确实在目的地附近才接管；
		// status 3 也可能因为"被挡住动不了"而到达，那种情况保持原版。
		const auto coords = pThis->Destination->GetCoords();
		const int distance = static_cast<int>(Point2D { coords.X, coords.Y }
			.DistanceFrom(Point2D { pThis->Location.X, pThis->Location.Y }));

		if (distance <= Math::max((pType->SlowdownDistance / 2), AdvancedMissions::GetTurningRadius(pThis)))
		{
			// 先推进路径点（和 0x4168C7 一致），链没走完就让引擎继续飞。
			if (pThis->TryNextPlanningTokenNode())
				return EnterIdleAndReturn;

			AbstractClass* const pCenter = pThis->Destination ? pThis->Destination : pThis->ArchiveTarget;

			if (pCenter && AdvancedMissions::TryBeginLoiter(pThis, pCenter))
				return EnterIdleAndReturn;
		}
	}

	return 0;
}

// AdvancedAircraftMissions: 飞控里的"这次移动到此为止 → 回机场"分支。
//
// ★★ 这才是"飞机已经停在目标格上，再点一次同一格 → 它自己飞回家"的真正病根
//    （第六轮实机日志定案：EnterIdleMode 的 caller = 0x004CFADC）。
//
// 调用链（全部在 SmoothMoving 的两个钩子 0x4168C7 / 0x416A0A **之前**）：
//
//   AircraftClass::Mission_Move status 1（0x416769）
//     └ 0x4167B4  call [loco+0x44]        = FlyLocomotionClass::Move_To（0x4CCC80）
//         └ 0x4CCDDB call 0x4CFA70        = "还能不能/要不要继续这次移动" 的判定
//             ├ 0x4CFA79 call [obj+0x184] = MissionClass::GetCurrentMission()
//             │    == 7 (Enter) → 继续判；否则先看 0x705D60 = TryNextPlanningTokenNode()
//             │    （有下一个路径点就直接收工，不回家）
//             ├ 0x4CFAA6 test [pType+0xE0D]        （机型标志）
//             ├ 0x4CFAC5 call 0x65AD50            （列表成员判定）
//             └ 0x4CFAD6 call [aircraft+0x484]    ← EnterIdleMode(false,1) → 回家
//
// 目的地 == 飞机当前所在格时，玩家再点同一格 ⇒ Move_To 走到这个"放弃"判定 ⇒
// EnterIdleMode ⇒ TryNearestDockBuilding + SetDestination(机场) —— 实机看到的就是
// "目的地标记从脚下的格子跳到机场上"。因为它在钩子之前，本功能**一行日志都打不到**。
//
// ★ 挂钩位置的选择（踩过坑，别再改回 0x4CFAD6）：
//   0x4CFAD0/0x4CFAD2 先 `push 1` / `push 0` 给 EnterIdleMode（`ret 8` 由被调者弹栈），
//   所以**跳过 0x4CFAD6 那次调用会留下 8 字节垃圾**，0x4CFADC 的
//   `pop edi / pop esi / ret` 全部错位 → 直接跳到 Eip=0（2026-09-25 实测崩过一次，
//   snapshot-20260925-134051：崩溃前最后一条日志就是 `fly MoveTo abort -> re-loiter`）。
//   因此改挂在**栈平衡**的 `0x4CFAB4`（`call [edx+0x1BC]`，此时没有任何临时 push），
//   接管后跳到引擎自带的"不回家"出口 `0x4CFADF`（清 3 个标志 + `pop edi/pop esi/ret`），
//   栈与副作用都与原路一致。
//
// ★ 这里**只阻止放弃**、不写任何本功能状态：本钩子在飞控栈内，若在这里调
//   `TryBeginLoiter` → `SetDestination`，可能再次绕回 Move_To / 本函数造成重入。
//   真正"重开盘旋"交给两道栈安全的路径，覆盖两种可能：
//     * 按 0x41698E（Mission_Move 的到位收尾 status 3）——见下面的钩子；
//     * 否则飞控仍在动，会照常走到 0x4168C7 的原有钩子。
DEFINE_HOOK(0x4CFAB4, FlyLocomotion_MoveTo_AbortReturn, 0x6)
{
	enum { ContinueVanillaMove = 0x4CFADF };

	GET(TechnoClass* const, pTechno, EDI);

	// 飞控的宿主不一定是飞机（VehicleType 也能配 Locomotor=Fly），必须按 WhatAmI 判。
	auto const pAircraft = abstract_cast<AircraftClass*, true>(pTechno);

	if (pAircraft
		&& AdvancedMissions::Enabled(pAircraft->Type)
		&& AdvancedMissions::HasAmmo(pAircraft)
		&& AdvancedMissions::GetLoiterRadius(pAircraft->Type) > 0
		&& pAircraft->Destination
		// 目的地在自家停机设施上时**不要拦**：那正是"回家/降落"流程，
		// 此处 abort（EnterIdleMode → 找最近机场）本来就是它要的结果。
		&& !AdvancedMissions::IsOwnDockBuilding(pAircraft, pAircraft->Destination))
	{
		return ContinueVanillaMove;
	}

	return 0;
}

DEFINE_HOOK(0x4DDD66, FootClass_IsLandZoneClear_ReplaceHardcode, 0x6) // To avoid that the aircraft cannot fly towards the water surface normally
{
	enum { SkipGameCode = 0x4DDD8A };

	GET(FootClass* const, pThis, EBP);
	GET_STACK(const CellStruct, cell, STACK_OFFSET(0x20, 0x4));

	const auto pType = pThis->GetTechnoType();

	// In vanilla, only aircrafts or `foots with fly locomotion` will call this virtual function
	// So I don't know why WW use hard-coded `SpeedType::Track` and `MovementZone::Normal` to check this
	R->AL(MapClass::Instance.GetCellAt(cell)->IsClearToMove(pType->SpeedType, false, false, -1, pType->MovementZone, -1, true));
	return SkipGameCode;
}

// Skip duplicated aircraft check
DEFINE_PATCH(0x4CF033, 0x8B, 0x06, 0xEB, 0x18); // mov eax, [esi] ; jmp short loc_4CF04F ;
DEFINE_JUMP(LJMP, 0x4179E2, 0x417B44);

// Fix enter mission
DEFINE_HOOK(0x419EF6, AircraftClass_Mission_Enter_FixNotCarryall, 0x7)
{
	enum { SkipGameCode = 0x419EFD };

	GET(AircraftClass* const, pThis, ESI);

	return pThis->Type->Carryall ? 0 : SkipGameCode;
}

// Skip set chaotic ArchiveTarget
static void __fastcall AircraftClass_SetArchiveTarget_Wrapper(AircraftClass* pThis, void* _, AbstractClass* pTarget)
{
	if (!AircraftTypeExt::Fetch(pThis->Type)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions))
		pThis->SetArchiveTarget(pTarget);
}
DEFINE_FUNCTION_JUMP(CALL, 0x41AB09, AircraftClass_SetArchiveTarget_Wrapper);
DEFINE_FUNCTION_JUMP(CALL, 0x41AC0A, AircraftClass_SetArchiveTarget_Wrapper);
DEFINE_FUNCTION_JUMP(CALL, 0x41AC2E, AircraftClass_SetArchiveTarget_Wrapper);
DEFINE_FUNCTION_JUMP(CALL, 0x41AC45, AircraftClass_SetArchiveTarget_Wrapper);
DEFINE_FUNCTION_JUMP(CALL, 0x41AC68, AircraftClass_SetArchiveTarget_Wrapper);
DEFINE_FUNCTION_JUMP(CALL, 0x41ACB9, AircraftClass_SetArchiveTarget_Wrapper);

DEFINE_HOOK(0x4CF190, FlyLocomotionClass_FlightUpdate_SetPrimaryFacing, 0x6) // Make aircraft not to fly directly to the airport before starting to land
{
	enum { SkipGameCode = 0x4CF29A };

	GET(IFlyControl* const, iFly, EAX);

	if (!iFly || !iFly->Is_Locked())
	{
		GET(FootClass** const, pFootPtr, ESI);
		// No const because it also need to be used by SecondaryFacing
		REF_STACK(CoordStruct, destination, STACK_OFFSET(0x48, 0x8));

		auto horizontalDistance = [&destination](const CoordStruct& location)
		{
			const auto delta = Point2D { location.X, location.Y } - Point2D { destination.X, destination.Y };
			return static_cast<int>(delta.Magnitude());
		};

		const auto pFoot = *pFootPtr;
		const auto pAircraft = abstract_cast<AircraftClass*, true>(pFoot);

		// Rewrite vanilla implement
		if (!pAircraft || !AircraftTypeExt::Fetch(pAircraft->Type)->ExtendedAircraftMissions_RearApproach
			.Get(AircraftTypeExt::Fetch(pAircraft->Type)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions)))
		{
			const auto footCoords = pFoot->GetCoords();
			const auto desired = DirStruct(Math::atan2(footCoords.Y - destination.Y, destination.X - footCoords.X));

			if (!iFly || !iFly->Is_Strafe() || horizontalDistance(footCoords) > 768 // I don't know why it's 3 cells' length, but its vanilla, keep it
				|| std::abs(static_cast<short>(static_cast<short>(desired.Raw) - static_cast<short>(pFoot->PrimaryFacing.Current().Raw))) <= 8192)
			{
				pFoot->PrimaryFacing.SetDesired(desired);
			}
		}
		else
		{
			const auto footCoords = pAircraft->GetCoords();
			const auto landingDir = DirStruct(AircraftExt::GetLandingDir(pAircraft));

			// Try to land from the rear
			if (pAircraft->Destination && (pAircraft->DockNowHeadingTo == pAircraft->Destination || pAircraft->SpawnOwner == pAircraft->Destination))
			{
				// Like smooth moving
				const int turningRadius = AdvancedMissions::GetTurningRadius(pAircraft);

				// diameter = 2 * radius
				const int cellCounts = Math::max((pAircraft->Type->SlowdownDistance / Unsorted::LeptonsPerCell), (turningRadius / 128));

				// The direction of the airport
				const auto currentDir = DirStruct(Math::atan2(footCoords.Y - destination.Y, destination.X - footCoords.X));

				// Included angle's raw
				const short difference = static_cast<short>(static_cast<short>(currentDir.Raw) - static_cast<short>(landingDir.Raw));

				// Land from this direction of the airport
				const auto landingFace = landingDir.GetFacing<8>(4);
				auto cellOffset = Unsorted::AdjacentCoord[landingFace];

				// When the direction is opposite, moving to the side first, then automatically shorten based on the current distance
				if (std::abs(difference) >= 12288) // 12288 -> 3/16 * 65536 (1/8 < 3/16 < 1/4, so the landing can begin at the appropriate location)
					cellOffset = (cellOffset + Unsorted::AdjacentCoord[((difference > 0) ? (landingFace + 2) : (landingFace - 2)) & 7]) * cellCounts;
				else // 724 -> 512√2
					cellOffset *= Math::min(cellCounts, ((landingFace & 1) ? (horizontalDistance(footCoords) / 724) : (horizontalDistance(footCoords) / 512)));

				// On the way back, increase the offset value of the destination so that it looks like a real airplane
				destination.X += cellOffset.X;
				destination.Y += cellOffset.Y;
			}

			if (footCoords.Y != destination.Y || footCoords.X != destination.X)
				pAircraft->PrimaryFacing.SetDesired(DirStruct(Math::atan2(footCoords.Y - destination.Y, destination.X - footCoords.X)));
			else
				pAircraft->PrimaryFacing.SetDesired(landingDir);
		}
	}

	return SkipGameCode;
}

DEFINE_HOOK(0x4CF3D0, FlyLocomotionClass_FlightUpdate_SetFlightLevel, 0x7) // Make aircraft not have to fly directly above the airport before starting to descend
{
	GET(FootClass** const, pFootPtr, ESI);

	const auto pAircraft = abstract_cast<AircraftClass*, true>(*pFootPtr);

	if (!pAircraft)
		return 0;

	const auto pType = pAircraft->Type;

	// Ares hook
	if (pType->HunterSeeker)
		return 0;

	const auto pTypeExt = AircraftTypeExt::Fetch(pType);
	const bool extendedMissions = pTypeExt->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions);

	if (!pTypeExt->ExtendedAircraftMissions_EarlyDescend.Get(extendedMissions))
		return 0;

	enum { SkipGameCode = 0x4CF4D2 };

	GET_STACK(FlyLocomotionClass* const, pThis, STACK_OFFSET(0x48, -0x28));
	GET(const int, distance, EBX);

	// Restore skipped code
	R->EBP(pThis);

	// Same as vanilla
	if (pThis->IsElevating && distance < 768)
	{
		// Fast descent
		const auto floorHeight = MapClass::Instance.GetCellFloorHeight(pThis->MovingDestination);
		pThis->FlightLevel = pThis->MovingDestination.Z - floorHeight;

		// Bug fix
		if (MapClass::Instance.GetCellAt(pAircraft->Location)->ContainsBridge() && pThis->FlightLevel >= CellClass::BridgeHeight)
			pThis->FlightLevel -= CellClass::BridgeHeight;

		return SkipGameCode;
	}

	const auto flightLevel = pType->GetFlightLevel();

	// Check returning actions
	if (distance < pType->SlowdownDistance && pAircraft->Destination
		&& (pAircraft->DockNowHeadingTo == pAircraft->Destination || pAircraft->SpawnOwner == pAircraft->Destination)
		&& (!pTypeExt->ExtendedAircraftMissions_RearApproach.Get(extendedMissions)
			|| std::abs(static_cast<short>(static_cast<short>(DirStruct(AircraftExt::GetLandingDir(pAircraft)).Raw) - static_cast<short>(pAircraft->PrimaryFacing.Current().Raw))) < 16384))
	{
		// Slow descent
		const auto floorHeight = MapClass::Instance.GetCellFloorHeight(pThis->MovingDestination);
		const auto destinationHeight = pThis->MovingDestination.Z - floorHeight + 1;
		pThis->FlightLevel = static_cast<int>((flightLevel - destinationHeight) * (static_cast<double>(distance) / pType->SlowdownDistance)) + destinationHeight;
	}
	else
	{
		// Horizontal flight
		pThis->FlightLevel = flightLevel;
	}

	return SkipGameCode;
}

DEFINE_HOOK(0x4CE42A, FlyLocomotionClass_StateUpdate_NoLanding, 0x6) // Prevent aircraft from hovering due to cyclic enter Guard and AreaGuard missions when above buildings
{
	enum { SkipGameCode = 0x4CE441 };

	GET(FootClass* const, pLinkTo, EAX);

	const auto pAircraft = abstract_cast<AircraftClass*, true>(pLinkTo);

	if (!pAircraft || !AircraftTypeExt::Fetch(pAircraft->Type)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions))
		return 0;

	if (pAircraft->Airstrike || pAircraft->IsALoaner || pAircraft->GetCurrentMission() == Mission::Enter)
		return 0;

	return SkipGameCode;
}

DEFINE_HOOK(0x414DA8, AircraftClass_Update_UnlandableDamage, 0x6) // After FootClass_Update
{
	GET(AircraftClass* const, pThis, ESI);

	// Missile.Homing: 每帧刷新制导瞄准 / 对空接近自爆
	AircraftExt::UpdateMissileHoming(pThis);

	// AdvancedAircraftMissions: 记录击杀点 + 收尾返航提速窗口（每台机同帧一致）
	AdvancedMissions::Update(pThis);

	const auto pType = pThis->Type;

	if (pThis->IsAlive && pType->AirportBound && !pThis->Airstrike && !pThis->IsALoaner)
	{
		const bool extendedMissions = AircraftTypeExt::Fetch(pType)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions);

		if (extendedMissions)
		{
			// Check area guard range
			if (const auto pArchive = pThis->ArchiveTarget)
			{
				// AdvancedAircraftMissions: 盘旋半径大于 GuardRange 时，原来的 1.1x GuardRange
				// leash 会把飞机从自己盘旋圈的边缘反复拽回圆心。让 leash 至少覆盖盘旋半径。
				const int leashBase = Math::max(pThis->GetGuardRange(1), AdvancedMissions::GetLoiterRadius(pType));

				if (pThis->Target && !pThis->IsFiring && !pThis->IsLocked
					&& pThis->DistanceFrom3D(pArchive) > static_cast<int>(leashBase * 1.1))
				{
					pThis->SetTarget(nullptr);
					pThis->SetDestination(pArchive, true);
				}
			}

			// Check dock building
			pThis->TryNearestDockBuilding(&pType->Dock, 0, 0);
		}

		if (pThis->DockNowHeadingTo)
		{
			// Exit the aimless hovering state and return to the new airport
			if (pThis->GetCurrentMission() == Mission::Area_Guard && pThis->MissionStatus)
			{
				pThis->SetArchiveTarget(nullptr);
				pThis->EnterIdleMode(false, true);
			}
		}
		else if (pThis->IsInAir())
		{
			int damage = AircraftTypeExt::Fetch(pType)->ExtendedAircraftMissions_UnlandDamage.Get(RulesExt::Global()->ExtendedAircraftMissions_UnlandDamage);

			if (damage > 0)
			{
				if (!extendedMissions && !pThis->unknown_bool_6B3 && pThis->TryNearestDockBuilding(&pType->Dock, 0, 0))
					return 0;

				// Injury every four frames
				if (!((Unsorted::CurrentFrame - pThis->LastFireBulletFrame + pThis->UniqueID) & 0x3))
					pThis->ReceiveDamage(&damage, 0, RulesClass::Instance->C4Warhead, nullptr, true, false, nullptr);
			}
			else if (damage < 0)
			{
				// Avoid using circular movement paths to prevent the aircraft from crashing
				if (extendedMissions)
					pThis->Crash(nullptr);
			}
		}
	}

	return 0;
}

// Guard: restart area guard
DEFINE_HOOK(0x41A5C7, AircraftClass_Mission_Guard_StartAreaGuard, 0x6)
{
	enum { SkipGameCode = 0x41A6AC };

	GET(AircraftClass* const, pThis, ESI);

	if (!AircraftTypeExt::Fetch(pThis->Type)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions)
		|| pThis->Team || !pThis->IsArmed() || pThis->Airstrike || pThis->IsALoaner)
	{
		return 0;
	}

	const auto pArchive = pThis->ArchiveTarget;

	if (!pArchive || !pThis->Ammo)
		return 0;

	pThis->SetDestination(pArchive, true);
	pThis->QueueMission(Mission::Area_Guard, false);
	return SkipGameCode;
}

// AreaGuard: Hover over to guard, or return when no ammo
DEFINE_HOOK_AGAIN(0x41A982, AircraftClass_Mission_AreaGuard, 0x6)
DEFINE_HOOK(0x41A96C, AircraftClass_Mission_AreaGuard, 0x6)
{
	enum { SkipGameCode = 0x41A9DA };

	GET(AircraftClass* const, pThis, ESI);

	if (!AircraftTypeExt::Fetch(pThis->Type)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions)
		|| pThis->Team || !pThis->IsArmed() || pThis->Airstrike || pThis->IsALoaner)
	{
		return 0;
	}

	auto enterIdleMode = [pThis]() -> bool
	{
		// Avoid duplicate checks in Update
		if (pThis->MissionStatus)
			return false;

		pThis->EnterIdleMode(false, true);

		if (pThis->DockNowHeadingTo)
			return true;

		// Hovering state without any airport
		pThis->MissionStatus = 1;
		return false;
	};
	auto hoverOverArchive = [pThis](const CoordStruct& coords, AbstractClass* pDest)
	{
		const auto& location = pThis->Location;
		const int turningRadius = AdvancedMissions::GetTurningRadius(pThis);

		// AdvancedAircraftMissions: the loiter radius key and the LoiterMode key both drive
		// the ordinary Ctrl+Alt area guard hover as well - the two deliberately share this
		// one hover driver. Not configured / feature disabled => GetLoiterRadius() is 0,
		// LoiterHover() is false, max() keeps the original turning radius, i.e. behaviour is
		// unchanged verbatim.
		const int radius = Math::max(turningRadius, AdvancedMissions::GetLoiterRadius(pThis->Type));
		const double distance = Math::max(1.0, Point2D { coords.X, coords.Y }.DistanceFrom(Point2D { location.X, location.Y }));

		// AdvancedAircraftMissions: LoiterMode=hover - stay put like a helicopter instead of
		// orbiting. This lambda is the ONLY thing that moves the aircraft during Area_Guard
		// (the upstream function body is skipped wholesale by the hook above, 0x41A9DA is
		// just the epilogue), so "fly back to the hover point" has to live here too: while
		// still farther out than the natural turning radius, head straight for the centre;
		// only once inside it brake to a stop. Without that, a hover + LoiterAutoTarget=yes
		// aircraft would leave its hover point behind at the kill site after one sortie.
		// IsLocked is only raised once parked (it makes the facing update at 0x4CF190 skip
		// outright and kills the parked jitter), the flight back has to keep it clear or the
		// nose stops turning and the aircraft strafes sideways.
		if (AdvancedMissions::LoiterHover(pThis->Type))
		{
			auto const pExt = AircraftExt::Fetch(pThis);

			// 悬停 = 把悬停点当成一个**固定目的地**：一段回程只下一次 Move_To，
			// 之后完全交给飞控（它在到位后自己减速停住）。
			//
			// ★ 两条用实机实测换来的红线，都不能碰：
			//   1) **绝不每帧重下 Move_To**：Move_To 每次调用都执行
			//      `mov BYTE PTR [loco+0x30],1`（0x4CCEAA）置 HasMoveOrder，
			//      而 Is_Moving()（0x4CCA90）读的正是它 —— 每帧重下 = 飞机永远
			//      "到不了位"，只能绕着悬停点打转。绕圈模式靠的恰是这条特性
			//      （每帧送一个转动的偏移点），所以两种模式不能共用写法。
			//   2) **绝不调 Stop_Moving 来"刹车"**：它不是刹车，而是把目的地改写成
			//      "飞机当前所在格"（0x4CCFD0 → SetDestination，实现 0x41AA80）。
			//      飞机一接近悬停点就被改写目的地 → 往旁边格子飞 → 离圆心重新超过
			//      阈值 → 再被拉回圆心 → 来回拉锯。用户实测观察到的
			//      "接近目标格时指令突然跳到附近另一个格子"就是它。
			if (distance > turningRadius)
			{
				// 0 = 首次进入 / 刚离开 AreaGuard（出击归来）；2 = 曾被推离悬停点。
				if (pExt->Loiter_HoverState != 1)
				{
					pThis->Locomotor->Move_To(coords);
					pExt->Loiter_HoverState = 1;
				}
			}
			else
			{
				// 已到悬停点：只下过那一次 Move_To，之后什么都不做，
				// 让飞控持续朝悬停点自我修正。
				pExt->Loiter_HoverState = 2;
			}

			// ★★ 绝不能在悬停时置 `IsLocked = true`（第三版踩过，日志实证）：
			//    `IsLocked` 会让 0x4CF190 的朝向更新**整段跳过** —— 机头被冻住，
			//    飞机无法再朝悬停点修正，只能沿冻结的机头方向直着飞出去；
			//    一飞远 `dist` 变大、降速倍率 `dist/1024` 随之变大 → 速度反而上升，
			//    正反馈一路冲到 ~900 leptons；随后远分支放开锁定 → 转回来 → 靠近又冻住
			//    → 再冲出去。实测 dist 在 0↔990 之间来回冲，就是这条。
			//    （用户观察的"上下动没事、左右动滑出去"= 机头冻结在哪个方向就往哪逃。）
			//    悬停靠的是"降速让转弯半径趋零"，机头必须保持可转向。
			pThis->IsLocked = false;

			return;
		}

		// Random hovering direction
		const double ratio = (((pThis->LastFireBulletFrame + pThis->UniqueID) & 1) ? radius : -radius) / distance;

		// Fly sideways towards the target, and extend the distance to ensure no deceleration
		const CoordStruct destination
		{
			(static_cast<int>(coords.X - ratio * (location.Y - coords.Y)) - location.X) * 4 + location.X,
			(static_cast<int>(coords.Y + ratio * (location.X - coords.X)) - location.Y) * 4 + location.Y,
			coords.Z
		};

		pThis->Locomotor->Move_To(destination);

		// AdvancedAircraftMissions: keep the LOCK threshold on the turning radius, not on
		// the configured loiter radius. AircraftClass::IsLocked is what the fly locomotor's
		// facing update checks (and Phobos' hook at 0x4CF190 skips that update entirely while
		// locked), so locking all the way out to the orbit radius freezes the nose and the
		// model stops banking into the turn. With the turning radius it is byte-equivalent to
		// the upstream expression for every type that has not opted in (GetLoiterRadius() == 0
		// => max() == turningRadius), and for opted-in types the configured radius still drives
		// the sideways offset above.
		pThis->IsLocked = distance < turningRadius;
	};

	if (const auto pArchive = pThis->ArchiveTarget)
	{
		// AdvancedAircraftMissions: an infinite-ammo aircraft (type Ammo<=0) counts as armed
		// while it is loitering, otherwise the loiter would end on the first frame.
		// Scoped to our own loiter so the upstream guard behaviour is untouched.
		const bool hasAmmo = pThis->Ammo
			|| (AircraftExt::Fetch(pThis)->Loiter_Active && AdvancedMissions::HasAmmo(pThis));

		if (hasAmmo)
		{
			auto coords = pArchive->GetCoords();

			// AdvancedAircraftMissions: LoiterAutoTarget=no suppresses the area search for our
			// loiter only - an explicit Ctrl+Alt area guard keeps its vanilla behaviour.
			const bool canSearch = !AircraftExt::Fetch(pThis)->Loiter_Active
				|| AdvancedMissions::LoiterAutoTarget(pThis->Type);

			if (canSearch && !pThis->TargetingTimer.HasTimeLeft() && pThis->TargetAndEstimateDamage(coords, ThreatType::Area))
			{
				// Without an airport, there is no need to record the previous location
				if (pThis->MissionStatus)
					pThis->SetArchiveTarget(nullptr);

				pThis->QueueMission(Mission::Attack, false);
			}
			else
			{
				// Check dock building
				if (!pThis->MissionStatus && !pThis->TryNearestDockBuilding(&pThis->Type->Dock, 0, 0))
					pThis->MissionStatus = 1;

				hoverOverArchive(coords, pArchive);
			}
		}
		else if (!enterIdleMode() && pThis->IsAlive)
		{
			// continue circling
			hoverOverArchive(pArchive->GetCoords(), pArchive);
		}
	}
	else if (!pThis->Destination)
	{
		enterIdleMode();
	}

	R->EAX(1);
	return SkipGameCode;
}

DEFINE_HOOK(0x7001B0, TechnoClass_MouseOverObject_EnableGuardObject, 0x7)
{
	enum { SkipCheckCanGuard = 0x7001E9, ContinueCheckCanGuard = 0x7001BC };

	GET(TechnoClass* const, pThis, ESI);

	return pThis->WhatAmI() == AbstractType::Aircraft
		&& !static_cast<AircraftExt*>(TechnoExt::Fetch(pThis))->GetTypeExtData()->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions)
		? SkipCheckCanGuard : ContinueCheckCanGuard;
}

// Sleep: return to airbase if in incorrect sleep status
static int __fastcall AircraftClass_Mission_Sleep(AircraftClass* pThis)
{
	if (!pThis->Destination || pThis->Destination == pThis->DockNowHeadingTo)
		return 450; // Vanilla MissionClass_Mission_Sleep value

	// AdvancedAircraftMissions: 悬停机型的"待机"语义就是继续悬着。
	// 这里是"飞机一进入待机就被上游送回机场"的咽喉点：只要盘旋条件还在，
	// 就用原盘旋圆心重开盘旋，而不是 EnterIdleMode 回家。
	// （防御性兜底：实机复验期间没有触发过，留作同一类"引擎把悬停中的飞机
	//   送去待机"的保护；判据全部来自 INI + 同步的仿真状态，各机一致。）
	if (AdvancedMissions::Enabled(pThis->Type)
		&& pThis->ArchiveTarget
		&& AdvancedMissions::HasAmmo(pThis)
		&& AdvancedMissions::GetLoiterRadius(pThis->Type) > 0
		&& AdvancedMissions::LoiterHover(pThis->Type)
		&& AdvancedMissions::TryBeginLoiter(pThis, pThis->ArchiveTarget))
	{
		return 1;
	}

	pThis->EnterIdleMode(false, true);
	return 1;
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E24A8, AircraftClass_Mission_Sleep)

// AttackMove: return when no ammo or arrived destination
static bool __fastcall AircraftTypeClass_CanAttackMove(AircraftTypeClass* pThis)
{
	return AircraftTypeExt::Fetch(pThis)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions);
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E290C, AircraftTypeClass_CanAttackMove)

DEFINE_HOOK(0x6FA68B, TechnoClass_Update_AttackMovePaused, 0xA) // To make aircrafts not search for targets while resting at the airport, this is designed to adapt to loop waypoint
{
	enum { SkipGameCode = 0x6FA6F5 };

	GET(TechnoClass* const, pThis, ESI);

	const bool skip = pThis->WhatAmI() == AbstractType::Aircraft
		&& static_cast<AircraftExt*>(TechnoExt::Fetch(pThis))->GetTypeExtData()->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions)
		&& (!pThis->Ammo || !pThis->IsInAir());

	return skip ? SkipGameCode : 0;
}

DEFINE_HOOK(0x4DF3BA, FootClass_UpdateAttackMove_AircraftHoldAttackMoveTarget1, 0x6) // When it have MegaDestination
{
	enum { LoseTarget = 0x4DF3D3, HoldTarget = 0x4DF4AB };

	GET(FootClass* const, pThis, ESI);

	// The aircraft is constantly moving, which may cause its target to constantly enter and leave its range, so it is fixed to hold the target.
	if (pThis->WhatAmI() == AbstractType::Aircraft
		&& static_cast<AircraftExt*>(TechnoExt::Fetch(pThis))->GetTypeExtData()->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions))
	{
		return HoldTarget;
	}

	return pThis->InAuxiliarySearchRange(pThis->Target) ? HoldTarget : LoseTarget;
}

DEFINE_HOOK(0x4DF42A, FootClass_UpdateAttackMove_AircraftHoldAttackMoveTarget2, 0x6) // When it have MegaTarget
{
	enum { ContinueCheck = 0x4DF462, HoldTarget = 0x4DF4AB };

	GET(FootClass* const, pThis, ESI);

	// Although if the target selected by CS is an object rather than cell.
	return (pThis->WhatAmI() == AbstractType::Aircraft
		&& static_cast<AircraftExt*>(TechnoExt::Fetch(pThis))->GetTypeExtData()->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions))
		? HoldTarget : ContinueCheck;
}

DEFINE_HOOK(0x418CD1, AircraftClass_Mission_Attack_ContinueFlyToDestination, 0x6)
{
	enum { Continue = 0x418C43, Return = 0x418CE8 };

	GET(AircraftClass* const, pThis, ESI);

	if (!pThis->Target)
	{
		if (!AircraftTypeExt::Fetch(pThis->Type)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions) || pThis->Airstrike || pThis->IsALoaner)
			return Continue;

		// AdvancedAircraftMissions: target destroyed while ammo remains -> loiter over the
		// target's last known position (recorded every frame while a target existed).
		// Already loitering -> go back to the original loiter centre instead of re-centring,
		// otherwise one kill would drag the aircraft further away every time.
		if (!pThis->Team && AdvancedMissions::Enabled(pThis->Type))
		{
			auto const pExt = AircraftExt::Fetch(pThis);

			if (pExt->Loiter_Active && pThis->ArchiveTarget)
			{
				if (AdvancedMissions::TryBeginLoiter(pThis, pThis->ArchiveTarget))
				{
					R->EAX(1);
					return Return;
				}
			}
			else if (pExt->Loiter_HasKillSpot)
			{
				if (auto const pKillCell = MapClass::Instance.TryGetCellAt(pExt->Loiter_KillSpot))
				{
					if (AdvancedMissions::TryBeginLoiter(pThis, pKillCell))
					{
						R->EAX(1);
						return Return;
					}
				}
			}
		}

		if (pThis->MegaMissionIsAttackMove() && pThis->MegaDestination)
		{
			pThis->SetArchiveTarget(nullptr);
			pThis->SetDestination(pThis->MegaDestination, false);
			pThis->QueueMission(Mission::Move, false);
			pThis->HaveAttackMoveTarget = false;
		}
		else if (!pThis->Team && pThis->ArchiveTarget)
		{
			pThis->SetDestination(pThis->ArchiveTarget, true);
			pThis->QueueMission(Mission::Area_Guard, false);
		}
		else
		{
			return Continue;
		}
	}
	else
	{
		pThis->MissionStatus = 1;
	}

	R->EAX(1);
	return Return;
}

// Jun 10, 2025 - Starkku: This is a bandaid fix to AI scripting problem that causes more issues than it solves so I have disabled it.
/*
// Idle: clear the target if no ammo
DEFINE_HOOK(0x414D4D, AircraftClass_Update_ClearTargetIfNoAmmo, 0x6)
{
	enum { ClearTarget = 0x414D3F };

	GET(AircraftClass* const, pThis, ESI);

	if (AircraftTypeExt::Fetch(pThis->Type)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions)
		&& !pThis->Ammo && !pThis->Airstrike && !pThis->IsALoaner)
	{
		if (!SessionClass::IsCampaign()) // To avoid AI's aircrafts team repeatedly attempting to attack the target when no ammo
		{
			if (const auto pTeam = pThis->Team)
				pTeam->LiberateMember(pThis);
		}

		return ClearTarget;
	}

	return 0;
}*/

// Idle: should not crash immediately
DEFINE_HOOK_AGAIN(0x417B82, AircraftClass_EnterIdleMode_NoCrash, 0x6)
DEFINE_HOOK(0x4179F7, AircraftClass_EnterIdleMode_NoCrash, 0x6)
{
	enum { SkipGameCode = 0x417B69 };

	GET(AircraftClass* const, pThis, ESI);

	if (pThis->Airstrike || pThis->IsALoaner)
		return 0;

	if (AircraftTypeExt::Fetch(pThis->Type)->ExtendedAircraftMissions_UnlandDamage.Get(RulesExt::Global()->ExtendedAircraftMissions_UnlandDamage) < 0)
		return 0;

	if (!pThis->Team && (pThis->CurrentMission != Mission::Area_Guard || !pThis->ArchiveTarget))
	{
		const auto pCell = reinterpret_cast<CellClass*(__thiscall*)(AircraftClass*)>(0x41A160)(pThis);
		pThis->SetDestination(pCell, true);
		pThis->SetArchiveTarget(pCell);
		pThis->QueueMission(Mission::Area_Guard, true);
	}
	else if (!pThis->Destination)
	{
		const auto pCell = reinterpret_cast<CellClass*(__thiscall*)(AircraftClass*)>(0x41A160)(pThis);
		pThis->SetDestination(pCell, true);
	}

	return SkipGameCode;
}

// Stop: clear the mega mission and return to airbase immediately
// (StopEventFix's DEFINE_HOOK(0x4C75DA, EventClass_RespondToEvent_Stop, 0x6) in Hooks.BugFixes.cpp)

// TakeOff: Emergency takeoff when the airport is destroyed
DEFINE_HOOK(0x4425B6, BuildingClass_ReceiveDamage_NoDestroyLink, 0xA)
{
	enum { LetTakeOff = 0x44259D };

	GET(BuildingClass* const, pThis, ESI);
	GET(TechnoClass* const, pTechno, EDI);

	if (!pThis->Type->Helipad)
		return 0;

	const auto pAircraft = abstract_cast<AircraftClass*, true>(pTechno);

	if (!pAircraft)
		return 0;

	const auto pTypeExt = AircraftTypeExt::Fetch(pAircraft->Type);
	const bool extendedMissions = pTypeExt->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions);

	if (!pTypeExt->ExtendedAircraftMissions_FastScramble.Get(extendedMissions))
		return 0;

	return LetTakeOff;
}

// GreatestThreat: for all the mission that should let the aircraft auto select a target
static AbstractClass* __fastcall AircraftClass_GreatestThreat(AircraftClass* pThis, void* _, ThreatType threatType, CoordStruct* pSelectCoords, bool onlyTargetHouseEnemy)
{
	if (AircraftTypeExt::Fetch(pThis->Type)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions)
		&& !pThis->Team && pThis->Ammo && !pThis->Airstrike && !pThis->IsALoaner)
	{
		if (const auto pPrimaryWeapon = pThis->GetWeapon(0)->WeaponType)
			threatType |= pPrimaryWeapon->AllowedThreats();

		if (const auto pSecondaryWeapon = pThis->GetWeapon(1)->WeaponType)
			threatType |= pSecondaryWeapon->AllowedThreats();
	}

	return pThis->FootClass::GreatestThreat(threatType, pSelectCoords, onlyTargetHouseEnemy);
}
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E2668, AircraftClass_GreatestThreat)

// Handle assigning area guard mission to aircraft.
DEFINE_HOOK(0x4C7403, EventClass_Execute_AircraftAreaGuard, 0x6)
{
	enum { SkipGameCode = 0x4C7426 };

	GET(EventClass* const, pThis, ESI);
	GET(TechnoClass* const, pTechno, EDI);

	if (pTechno->WhatAmI() == AbstractType::Aircraft
		&& static_cast<AircraftExt*>(TechnoExt::Fetch(pTechno))->GetTypeExtData()->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions)
		&& !pTechno->Ammo)
	{
		// Skip assigning destination / target here.
		R->ESI(&pThis->MegaMission.Target);
		return SkipGameCode;
	}

	return 0;
}

// Do not untether aircraft when assigning area guard mission by default.
DEFINE_HOOK(0x4C72F2, EventClass_Execute_AircraftAreaGuard_Unlink, 0x6)
{
	enum { SkipGameCode = 0x4C7349 };

	GET(EventClass* const, pThis, ESI);
	GET(TechnoClass* const, pTechno, EDI);

	if (pTechno->WhatAmI() == AbstractType::Aircraft
		&& static_cast<AircraftExt*>(TechnoExt::Fetch(pTechno))->GetTypeExtData()->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions)
		&& pThis->MegaMission.Mission == (char)Mission::Area_Guard
		&& !pTechno->Ammo)
	{
		// If we're on dock reloading but have ammo, untether from dock and try to scan for targets.
		return SkipGameCode;
	}

	return 0;
}

DEFINE_HOOK(0x418CF3, AircraftClass_Mission_Attack_PlanningFix, 0x5)
{
	enum { SkipIdle = 0x418D00 };

	GET(AircraftClass*, pThis, ESI);

	return pThis->Ammo <= 0 || !pThis->TryNextPlanningTokenNode() ? 0 : SkipIdle;
}

#pragma endregion

static __forceinline bool CheckSpyPlaneCameraCount(AircraftClass* pThis)
{
	auto const pWeaponExt = WeaponTypeExt::Fetch(pThis->GetWeapon(0)->WeaponType);

	if (!pWeaponExt->Strafing_Shots.isset())
		return true;

	auto const pExt = AircraftExt::Fetch(pThis);

	if (pExt->Strafe_BombsDroppedThisRound >= pWeaponExt->Strafing_Shots)
		return false;

	pExt->Strafe_BombsDroppedThisRound++;

	return true;
}

DEFINE_HOOK(0x415666, AircraftClass_Mission_SpyPlaneApproach_MaxCount, 0x6)
{
	enum { Skip = 0x41570C };

	GET(AircraftClass*, pThis, ESI);

	if (!CheckSpyPlaneCameraCount(pThis))
		return Skip;

	return 0;
}

DEFINE_HOOK(0x4157EB, AircraftClass_Mission_SpyPlaneOverfly_MaxCount, 0x6)
{
	enum { Skip = 0x415863 };

	GET(AircraftClass*, pThis, ESI);

	if (!CheckSpyPlaneCameraCount(pThis))
		return Skip;

	return 0;
}

#pragma region Rocket

DEFINE_HOOK(0x66295A, RocketLocomotionClass_Process_IsHighEnoughForCruise, 0x8)
{
	GET(AircraftClass*, pLinkedTo, ECX);
	GET(ILocomotion*, pThis, ESI);

	const auto pLoco = locomotion_cast<RocketLocomotionClass*>(pThis);
	const int heightThis = pLinkedTo->GetHeight();
	int heightTarget = pLinkedTo->Location.Z - pLoco->MovingDestination.Z;

	if (MapClass::Instance.GetCellAt(pLoco->MovingDestination)->ContainsBridge())
		heightTarget -= CellClass::BridgeHeight;

	R->EAX(Math::min(heightThis, heightTarget));
	//R->EAX(pLinkedTo->GetHeight()); Vanilla behavior

	return R->Origin() + 0x8;
}

#pragma endregion

#pragma region CurleyShuffle

DEFINE_HOOK(0x4183C3, AircraftClass_CurleyShuffle_FireAtTarget, 0x6)
{
	GET(AircraftClass*, pThis, ESI);
	const auto pTypeExt = AircraftTypeExt::Fetch(pThis->Type);
	R->DL(pTypeExt->CurleyShuffle.Get(RulesClass::Instance->CurleyShuffle));
	return 0x4183C9;
}

DEFINE_HOOK(0x418671, AircraftClass_CurleyShuffle_FireOk, 0x6)
{
	GET(AircraftClass*, pThis, ESI);
	const auto pTypeExt = AircraftTypeExt::Fetch(pThis->Type);
	R->AL(pTypeExt->CurleyShuffle.Get(RulesClass::Instance->CurleyShuffle));
	return 0x418677;
}

DEFINE_HOOK(0x418733, AircraftClass_CurleyShuffle_FireFacing, 0x6)
{
	GET(AircraftClass*, pThis, ESI);
	const auto pTypeExt = AircraftTypeExt::Fetch(pThis->Type);
	R->CL(pTypeExt->CurleyShuffle.Get(RulesClass::Instance->CurleyShuffle));
	return 0x418739;
}

DEFINE_HOOK(0x418782, AircraftClass_CurleyShuffle_Default, 0x6)
{
	GET(AircraftClass*, pThis, ESI);
	const auto pTypeExt = AircraftTypeExt::Fetch(pThis->Type);
	R->DL(pTypeExt->CurleyShuffle.Get(RulesClass::Instance->CurleyShuffle));
	return 0x418788;
}

#pragma endregion

#pragma region ParadropDelay

DEFINE_HOOK(0x415A00, AircraftClass_Mission_Paradrop_Overfly_Delay, 0x5)
{
	GET(AircraftClass*, pThis, ESI);

	const auto pTypeExt = AircraftTypeExt::Fetch(pThis->Type);
	int delay;

	if (pThis->Passengers.NumPassengers)
	{
		delay = pTypeExt->ParadropDelay.Get(RulesExt::Global()->ParadropDelay);
	}
	else
	{
		delay = pTypeExt->ParadropEndDelay.Get(RulesExt::Global()->ParadropEndDelay);

		if (delay < 0)
			delay = INT32_MAX;
	}

	R->EAX(delay);
	return 0x415A05;
}

// Allow edge of the map processing on Paradrop_Overfly mission if paradrop end delay is infinite.
DEFINE_HOOK(0x4CD54C, FlyLocomotionClass_EdgeOfTheWorldAI_Paradrop, 0x8)
{
	enum { Continue = 0x4CD55D, ReturnFromFunction = 0x4CD5F7 };

	GET(AircraftClass*, pLinkedTo, ECX);

	if (pLinkedTo->CurrentMission == Mission::Retreat || (pLinkedTo->CurrentMission == Mission::ParadropOverfly
		&& AircraftTypeExt::Fetch(pLinkedTo->Type)->ParadropEndDelay.Get(RulesExt::Global()->ParadropEndDelay) < 0
		&& !pLinkedTo->Passengers.NumPassengers))
	{
		return Continue;
	}

	return ReturnFromFunction;
}

#pragma endregion

#pragma region CargoPlane

DEFINE_HOOK(0x4143A8, AircraftClass_UnLimbo_CargoPlane, 0x6)
{
	enum { SkipGameCode = 0x4143F2 };

	GET(AircraftClass*, pThis, ESI);

	auto const pType = pThis->Type;
	auto const pTypeExt = AircraftTypeExt::Fetch(pType);

	if (pTypeExt->IsALoaner.isset())
	{
		pThis->IsALoaner = pTypeExt->IsALoaner.Get();
		return SkipGameCode;
	}

	return 0;
}

#pragma endregion

#pragma region MissileHoming

// Missile.Homing: 引擎的 Kamikaze 管理器会周期性地按节点 Cell 给每颗在飞导弹
// 重新定向(约每 2 帧一次)。普通刷新 FootClass::Destination 会被它覆盖, 因此
// 直接钩在原版读取该节点 Cell 的指令之前(0x54E51D 处 EAX=KamikazeControl*),
// 把它刷成目标单位当前所在格 —— 与原版重定向同节奏, 导弹就会持续追踪目标。
DEFINE_HOOK(0x54E51D, KamikazeContainer_Update_MissileHomingCell, 0x5)
{
	GET(Kamikaze::KamikazeControl* const, pNode, EAX);

	auto const pMissile = pNode->Item;
	if (!pMissile)
		return 0;

	auto const pTypeExt = AircraftTypeExt::Fetch(pMissile->Type);
	if (!pTypeExt->Missile_Homing || !pMissile->Type->MissileSpawn)
		return 0;

	auto const pExt = AircraftExt::TryFetch(pMissile);
	if (!pExt)
		return 0;

	// 兜底激活: 若发射钩子未跑到, 且 Kamikaze 节点的 Cell 本身就是目标单位
	if (!pExt->Homing_Active)
	{
		if (auto const pTargetTechno = abstract_cast<TechnoClass*>(pNode->Cell))
			AircraftExt::StartMissileHoming(pMissile, pTargetTechno);
	}

	if (!pExt->Homing_Active)
		return 0;

	auto const pTarget = pExt->Homing_Target;
	if (!pTarget || !AircraftExt::IsTrackedTargetValid(pTarget) || !pTarget->IsAlive || pTarget->InLimbo
		|| pTarget->WhatAmI() == AbstractType::Building
		|| pTarget->CloakState == CloakState::Cloaked)
	{
		// 失锁: 节点 Cell 保持最后位置, 引擎会让导弹飞过去落地引爆
		pExt->Homing_Active = false;
		pExt->Homing_Target = nullptr;
		return 0;
	}

	auto const aimCrd = pTarget->GetCenterCoords();

	if (auto const pCell = MapClass::Instance.TryGetCellAt(aimCrd))
	{
		pNode->Cell = pCell;
		pExt->Homing_LastAim = aimCrd;
	}

	return 0;
}

// Missile.Homing: 引擎本 tick 用节点 Cell 处理完导弹后, 再把 FootClass 目的地
// 拉回目标当前格 —— 与引擎重定向同节奏双保险 (覆盖"飞行导航只认目的地"的情形)。
DEFINE_HOOK(0x54E56D, KamikazeContainer_Update_MissileHomingAfter, 0x6)
{
	GET(AircraftClass* const, pMissile, ESI);

	if (!pMissile)
		return 0;

	auto const pTypeExt = AircraftTypeExt::Fetch(pMissile->Type);
	if (!pTypeExt->Missile_Homing || !pMissile->Type->MissileSpawn)
		return 0;

	auto const pExt = AircraftExt::TryFetch(pMissile);
	if (!pExt || !pExt->Homing_Active)
		return 0;

	auto const pTarget = pExt->Homing_Target;
	if (!pTarget || !AircraftExt::IsTrackedTargetValid(pTarget) || !pTarget->IsAlive || pTarget->InLimbo
		|| pTarget->WhatAmI() == AbstractType::Building
		|| pTarget->CloakState == CloakState::Cloaked)
		return 0;

	if (auto const pCell = MapClass::Instance.TryGetCellAt(pTarget->GetCenterCoords()))
		pMissile->SetDestination(pCell, true);

	return 0;
}

// Missile.Homing: 原版导弹一旦进入"末端俯冲(Step5)"就再也拉不起来, 目标跑远时
// 会直插当时位置落地 —— 这是"只追一小段就插地"的根源。参照 Kratos 同址处理
// (钩 0x662CAC, 命中即跳回 0x662A32 巡航逻辑), 制导中的导弹只要离目标还远,
// 就跳过俯冲提交, 留在巡航段继续追。
DEFINE_HOOK(0x662CAC, RocketLocomotionClass_Process_SkipTerminalDive_Homing, 0x6)
{
	enum { ReEnterCruise = 0x662A32 };

	// 该地址的 ESI 指向火箭飞行器内部对象, 被驱动的 Aircraft 位于 [ESI+8]。
	// 此处不做类型化成员访问(该处对象布局与 YRpp 模型在此不一致), 直接按原始
	// 偏移读取并用 abstract_cast 校验 —— 与 Kratos 同址处理一致。
	GET(DWORD, pContext, ESI);
	if (!pContext)
		return 0;

	auto const pLinkedRaw = *reinterpret_cast<AbstractClass* const*>(reinterpret_cast<const char*>(pContext) + 8);
	auto const pMissile = abstract_cast<AircraftClass*>(pLinkedRaw);
	if (!pMissile)
		return 0;

	auto const pTypeExt = AircraftTypeExt::Fetch(pMissile->Type);
	if (!pTypeExt->Missile_Homing || !pMissile->Type->MissileSpawn)
		return 0;

	auto const pExt = AircraftExt::TryFetch(pMissile);
	if (!pExt || !pExt->Homing_Active)
		return 0;

	auto const pTarget = pExt->Homing_Target;
	if (!pTarget || !AircraftExt::IsTrackedTargetValid(pTarget) || !pTarget->IsAlive || pTarget->InLimbo
		|| pTarget->WhatAmI() == AbstractType::Building
		|| pTarget->CloakState == CloakState::Cloaked)
		return 0;

	// 目标还远(水平距离大于阈值, 默认 2 格): 中止俯冲, 回巡航段继续追
	auto const misCrd = pMissile->GetCoords();
	auto const aimCrd = pTarget->GetCenterCoords();
	const double dx = static_cast<double>(misCrd.X) - aimCrd.X;
	const double dy = static_cast<double>(misCrd.Y) - aimCrd.Y;
	const double distXY = std::sqrt(dx * dx + dy * dy);

	if (distXY > pTypeExt->Homing_CruiseSkipRange.Get(512))
		return ReEnterCruise;

	return 0;
}

#pragma endregion

#include "Body.h"

#include <Kamikaze.h>
#include <RocketLocomotionClass.h>
#include <cmath>

#include <Ext/AircraftType/Body.h>
#include <Ext/BuildingType/Body.h>
#include <Ext/WarheadType/Body.h>
#include <Ext/WeaponType/Body.h>

AircraftExt::ExtContainer AircraftExt::ExtMap;

namespace
{
	// 对空引爆判据 (原版俯冲机制到不了空中单位的高度, 因此导弹追到单位附近时
	// 直接把弹头在单位坐标上空爆):
	//  - 3D 极近距离 (几乎贴脸), 或
	//  - 水平距离 < XY 圈, 且导弹高度已俯冲到不超过目标高度 + Z 容差
	//    (导弹正下降到单位正下方/平齐时引爆)
	// XY/Z 可分别由 AircraftType 上的 Missile.Homing.AirBurstRangeXY/Z 覆盖。
	constexpr int DefaultAirBurstRange3D = 176; // 3D 极近
	constexpr int DefaultAirBurstRangeXY = 384; // 水平命中圈 (约 1.5 格)
	constexpr int DefaultAirBurstRangeZ = 256; // 目标高度上方的容差

	// 在单位坐标处施放导弹载荷, 并移除导弹本体
	void HomingAirBurst(AircraftClass* pMissile, TechnoClass* pTarget, AircraftTypeExt* pTypeExt)
	{
		const bool elite = pMissile->SpawnOwner && pMissile->SpawnOwner->Veterancy.IsElite();
		const int damage = elite
			? pTypeExt->Homing_EliteDamage.Get(pTypeExt->Homing_Damage.Get())
			: pTypeExt->Homing_Damage.Get();
		auto const warhead = elite
			? pTypeExt->Homing_EliteWarhead.Get(pTypeExt->Homing_Warhead.Get())
			: pTypeExt->Homing_Warhead.Get();

		if (!warhead || damage <= 0)
			return; // 该导弹未配置可复用的载荷: 交给原版落地行为兜底

		auto const pExt = AircraftExt::Fetch(pMissile);
		pExt->Homing_Active = false;
		pExt->Homing_Target = nullptr;

		// 在目标单位当前坐标(含高度)引爆
		WarheadTypeExt::DetonateAt(warhead, pTarget->GetCenterCoords(), pMissile, damage, pMissile->Owner);

		// 从俯冲管理器摘除并销毁导弹本体
		Kamikaze::Instance.Remove(pMissile);
		pMissile->ReceiveDamage(&pMissile->Health, 0, RulesClass::Instance->C4Warhead, nullptr, true, false, nullptr);
	}
}

void AircraftExt::FireWeapon(AircraftClass* pThis, AbstractClass* pTarget)
{
	auto const pExt = AircraftExt::Fetch(pThis);
	const int weaponIndex = pExt->CurrentAircraftWeaponIndex;
	auto const pWeapon = pThis->GetWeapon(weaponIndex)->WeaponType;
	auto const pWeaponExt = WeaponTypeExt::Fetch(pWeapon);
	const int burstCount = pWeapon->Burst;
	const bool isStrafe = pThis->Is_Strafe();

	if (burstCount > 0)
	{
		int& bombDropCount = pExt->Strafe_BombsDroppedThisRound;
		int& currentBurstIndex = pThis->CurrentBurstIndex;
		const bool simulateBurst = pWeaponExt->Strafing_SimulateBurst.Get(RulesExt::Global()->Strafing_SimulateBurst);

		for (int i = 0; i < burstCount; i++)
		{
			if (isStrafe && burstCount < 2 && simulateBurst)
				currentBurstIndex = bombDropCount % 2 == 0;

			pThis->Fire(pTarget, weaponIndex);
		}

		if (isStrafe)
		{
			bombDropCount++;

			if (pWeaponExt->Strafing_UseAmmoPerShot.Get(RulesExt::Global()->Strafing_UseAmmoPerShot))
			{
				pThis->Ammo--;
				pThis->ShouldLoseAmmo = false;

				if (!pThis->Ammo)
				{
					pThis->SetTarget(nullptr);
					pThis->SetDestination(nullptr, true);
				}
			}
		}
	}
}

// Missile.Homing: 发射瞬间锁定目标 (由发射器 AI 处调用)
void AircraftExt::StartMissileHoming(AircraftClass* pMissile, TechnoClass* pTarget)
{
	// 建筑目标继续走原版(对象目标)命中路径, 不参与单位制导
	if (!pMissile || !pTarget || pTarget->WhatAmI() == AbstractType::Building)
		return;

	auto const pTypeExt = AircraftTypeExt::Fetch(pMissile->Type);
	if (!pTypeExt->Missile_Homing || !pMissile->Type->MissileSpawn)
		return; // 仅对"导弹型子机"生效 (Missile.Homing 是给导弹类子机用的)

	auto const pExt = AircraftExt::Fetch(pMissile);
	pExt->Homing_Active = true;
	pExt->Homing_Target = pTarget;
	pExt->Homing_LastAim = pTarget->GetCenterCoords();
}

// 目标是否仍在 TechnoClass::Array 中。对象被删除前必先从该数组移除, 因此
// 数组命中即可安全解引用; 未命中说明对象已(或正在)被移除, 不能再碰。
bool AircraftExt::IsTrackedTargetValid(TechnoClass* pTarget)
{
	if (!pTarget)
		return false;

	for (int i = 0; i < TechnoClass::Array.Count; ++i)
	{
		if (TechnoClass::Array.GetItem(i) == pTarget)
			return true;
	}

	return false;
}

// Missile.Homing: 每帧刷新瞄准点 / 对空接近自爆 (由 AircraftClass 每帧更新处调用)
void AircraftExt::UpdateMissileHoming(AircraftClass* pMissile)
{
	auto const pExt = AircraftExt::TryFetch(pMissile);
	if (!pExt || !pExt->Homing_Active)
		return;

	if (!pMissile->IsAlive || pMissile->InLimbo)
	{
		pExt->Homing_Active = false;
		pExt->Homing_Target = nullptr;
		return;
	}

	auto const pTypeExt = pExt->GetTypeExtData();
	if (!pTypeExt || !pTypeExt->Missile_Homing)
	{
		pExt->Homing_Active = false;
		pExt->Homing_Target = nullptr;
		return;
	}

	// 目标死亡/隐形/消失: 飞往最后记录点后由原版落地引爆 (Kratos 同款失锁语义)
	auto const pTarget = pExt->Homing_Target;
	const bool targetLost = !pTarget
		|| !AircraftExt::IsTrackedTargetValid(pTarget)
		|| pTarget->WhatAmI() == AbstractType::Building
		|| !pTarget->IsAlive
		|| pTarget->InLimbo
		|| pTarget->CloakState == CloakState::Cloaked;

	if (targetLost)
	{
		pExt->Homing_Active = false;
		pExt->Homing_Target = nullptr;

		if (auto const pCell = MapClass::Instance.TryGetCellAt(pExt->Homing_LastAim))
			pMissile->SetDestination(pCell, true);

		return;
	}

	const bool targetInAir = pTarget->IsInAir();
	const int dist3d = pMissile->DistanceFrom3D(pTarget);

	if (targetInAir)
	{
		// 2D 水平距离与高度差
		auto const misCrd = pMissile->GetCoords();
		auto const aimCrd = pTarget->GetCenterCoords();
		const double dx = static_cast<double>(misCrd.X) - aimCrd.X;
		const double dy = static_cast<double>(misCrd.Y) - aimCrd.Y;
		const int distXY = static_cast<int>(std::sqrt(dx * dx + dy * dy));

		const int burstRangeXY = pTypeExt->Homing_AirBurstRangeXY.Get(DefaultAirBurstRangeXY);
		const int burstRangeZ = pTypeExt->Homing_AirBurstRangeZ.Get(DefaultAirBurstRangeZ);

		// 导弹已降到不高于飞机高度上方 burstRangeZ, 且水平已贴近单位 -> 在单位坐标引爆
		const bool descendedNearPlane = misCrd.Z <= aimCrd.Z + burstRangeZ;

		if (dist3d < DefaultAirBurstRange3D
			|| (distXY < burstRangeXY && descendedNearPlane))
		{
			HomingAirBurst(pMissile, pTarget, pTypeExt);
			return;
		}
	}

	if (!pExt->Homing_Active) // 上面自爆可能已把本弹移除
		return;

	auto aimCrd = pTarget->GetCenterCoords();

	// 地面目标: 瞄点 Z 归一到地面高度 (保持"触地爆炸"语义);
	// 空中目标: 保留完整 3D 坐标, 让导弹真的朝飞机爬升/俯冲。
	if (!targetInAir)
		aimCrd.Z = MapClass::Instance.GetCellFloorHeight(aimCrd);

	pExt->Homing_LastAim = aimCrd;

	// 方案 D (对齐 Kratos): 原版火箭的每帧转向步(0x662A32/steps)读取的是
	// RocketLocomotionClass::MovingDestination, 而不是 FootClass::Destination;
	// 只写后者不会被转向逻辑消费 —— 这正是此前制导"迟钝"的根因。
	// 每帧把目标当前坐标写进 loco 自己的 MovingDestination, 转向立即变连续。
	if (auto const pLoco = locomotion_cast<RocketLocomotionClass*>(pMissile->Locomotor))
		pLoco->MovingDestination = aimCrd;

	// 兜底: 仍同步一次 FootClass 目的地 (对非火箭 loco / 其他引擎流程保持兼容)
	if (auto const pCell = MapClass::Instance.TryGetCellAt(aimCrd))
		pMissile->SetDestination(pCell, true);
}

// Paradrop, spy plane, airstrike.
bool AircraftExt::PlaceReinforcementAircraft(AircraftClass* pThis, CoordStruct edgeCoords)
{
	auto const pType = pThis->Type;
	auto const pTypeExt = AircraftTypeExt::Fetch(pType);
	auto dir = DirType::North;
	auto coords = edgeCoords;
	coords.Z = 0;
	AbstractClass* pTarget = pThis->Target ? pThis->Target : pThis->Destination;

	if (pTarget)
	{
		auto const pTargetCoords = pTarget->GetCoords();

		if (pTypeExt->SpawnDistanceFromTarget.isset())
			coords = GeneralUtils::CalculateCoordsFromDistance(edgeCoords, pTargetCoords, pTypeExt->SpawnDistanceFromTarget.Get());

		dir = GeneralUtils::GetDirectionBetweenCoords(coords, pTargetCoords).GetDir();
	}

	bool result = false;

	++Unsorted::ScenarioInit;
	result = pThis->Unlimbo(coords, dir);
	--Unsorted::ScenarioInit;

	pThis->SetHeight(pTypeExt->SpawnHeight.isset() ? pTypeExt->SpawnHeight.Get() : pType->GetFlightLevel());

	if (pTarget)
		pThis->PrimaryFacing.SetDesired(pThis->GetTargetDirection(pTarget));

	return result;
}

CellStruct AircraftExt::PickEdgeCellForPlane(AircraftTypeClass* pPlaneType, CellStruct destCell, Edge edge, bool isOnRetreat)
{
	auto const pTypeExt = AircraftTypeExt::Fetch(pPlaneType);
	auto const edgeMode = !isOnRetreat ? pTypeExt->SpawnFromEdge.Get(RulesExt::Global()->AircraftSpawnFromEdge)
		: pTypeExt->RetreatToEdge.Get(RulesExt::Global()->AircraftRetreatToEdge);
	auto spawnEdge = edge;
	auto refCell = CellStruct::Empty;

	switch (edgeMode)
	{
	case EdgeType::Closest:
	{
		if (destCell != CellStruct::Empty)
		{
			spawnEdge = Edge::None;
			refCell = destCell;

			// Scatter the coords a bit to randomize spawn cell a little - otherwise multiple planes sent at same target
			// from same source might end up overlapping - still a possibility, just less likely.
			// The edge cell picking function itself will do no randomization on Edge::None + waypoint cell set mode.
			int const randomRange = 5;
			short const randomX = static_cast<short>(ScenarioClass::Instance->Random.RandomRanged(-randomRange, randomRange));
			short const randomY = static_cast<short>(ScenarioClass::Instance->Random.RandomRanged(-randomRange, randomRange));
			refCell += CellStruct { randomX, randomY };
		}
		break;
	}
	case EdgeType::Random:
	{
		int const min = static_cast<int>(Edge::North);
		int const max = static_cast<int>(Edge::West);
		spawnEdge = static_cast<Edge>(ScenarioClass::Instance->Random.RandomRanged(min, max));
		break;
	}
	default:
	{
		break;
	}
	}

	return MapClass::Instance.PickCellOnEdge(spawnEdge, refCell, CellStruct::Empty, SpeedType::Winged, true, MovementZone::Normal);
}

DirType AircraftExt::GetLandingDir(AircraftClass* pThis, BuildingClass* pDock, bool isProduction)
{
	auto const poseDir = static_cast<DirType>(RulesClass::Instance->PoseDir);

	if (!pThis)
		return poseDir;

	// If this is a spawnee, use the spawner's facing.
	if (auto const pOwner = pThis->SpawnOwner)
		return pOwner->PrimaryFacing.Current().GetDir();

	auto const pType = pThis->Type;

	const bool hasDockOrLink = (pDock || pThis->HasAnyLink());

	if (hasDockOrLink)
	{
		auto const pLink = pThis->GetNthLink(0);

		if (auto const pBuilding = pDock ? pDock : abstract_cast<BuildingClass*, true>(pLink))
		{
			auto const pBuildingType = pBuilding->Type;
			auto const pBuildingTypeExt = BuildingTypeExt::Fetch(pBuildingType);
			const int docks = pBuildingType->NumberOfDocks;
			const int linkIndex = pBuilding->FindLinkIndex(pThis);

			if (docks > 0 && linkIndex >= 0 && linkIndex < docks)
			{
				if (pBuildingTypeExt->AircraftDockingDirs[linkIndex].has_value())
					return *pBuildingTypeExt->AircraftDockingDirs[linkIndex];
			}
			else if (docks > 0 && pBuildingTypeExt->AircraftDockingDirs[0].has_value())
				return *pBuildingTypeExt->AircraftDockingDirs[0];

			if (!pBuildingTypeExt->AircraftDockingDir_DefaultToPoseDir.Get(RulesExt::Global()->AircraftDockingDir_DefaultToPoseDir))
			{
				if (!isProduction)
					return pBuilding->PrimaryFacing.Current().GetDir();
			}
		}
		else if (!pType->AirportBound)
			return pLink->PrimaryFacing.Current().GetDir();
	}

	auto const pTypeExt = AircraftTypeExt::Fetch(pType);

	if (pTypeExt->LandingDir.isset())
	{
		int landingDir = pTypeExt->LandingDir.Get();
		if (pType->AirportBound)
			return static_cast<DirType>(landingDir & 0xFF);
		else if (landingDir < 0)
			return pThis->PrimaryFacing.Current().GetDir();
		else
			return static_cast<DirType>(landingDir & 0xFF);
	}

	if (isProduction)
		return static_cast<DirType>(RulesExt::Global()->PoseDir_Production.Get((int)poseDir) & 0xFF);

	if (hasDockOrLink)
		return poseDir;

	int fieldDir = RulesExt::Global()->PoseDir_Field.Get((int)poseDir * 32);
	return static_cast<DirType>(fieldDir & 0xFF);
}

AircraftTypeClass* AircraftExt::GetAircraftTypeExtra(AircraftClass* pAircraft)
{
	auto const pType = pAircraft->Type;
	auto const pData = AircraftTypeExt::Fetch(pType);

	if (!pData->NeedDamagedImage || pAircraft->IsGreenHP())
	{
		return pType;
	}
	else if (pAircraft->IsYellowHP())
	{
		if (auto const imageYellow = pData->Image_ConditionYellow)
			return abstract_cast<AircraftTypeClass*, true>(imageYellow);
	}
	else
	{
		if (auto const imageRed = pData->Image_ConditionRed)
			return abstract_cast<AircraftTypeClass*, true>(imageRed);
		else if (auto const imageYellow = pData->Image_ConditionYellow)
			return abstract_cast<AircraftTypeClass*, true>(imageYellow);
	}

	return pType;
}

// =============================
// load / save

template <typename T>
void AircraftExt::Serialize(T& Stm)
{
	Stm
		.Process(this->Strafe_BombsDroppedThisRound)
		.Process(this->Strafe_TargetCell)
		.Process(this->CurrentAircraftWeaponIndex)
		.Process(this->Loiter_Active)
		.Process(this->Loiter_HasKillSpot)
		.Process(this->Loiter_KillSpot)
		.Process(this->ReturnOrderActive)
		.Process(this->ReturnSpeedMultiplier)
		.Process(this->HasReturnOrderDest)
		.Process(this->ReturnOrderDest)
		.Process(this->Homing_Active)
		.Process(this->Homing_Target)
		.Process(this->Homing_LastAim)
		// 新增字段一律追加在链尾（插在中间会让旧存档整体错位一格）
		.Process(this->Loiter_HoverState)
		;
}

void AircraftExt::LoadFromStream(PhobosStreamReader& Stm)
{
	FootExt::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void AircraftExt::SaveToStream(PhobosStreamWriter& Stm)
{
	FootExt::SaveToStream(Stm);
	this->Serialize(Stm);
}

// =============================
// container

AircraftExt::ExtContainer::ExtContainer() : Container("AircraftClass") { }
AircraftExt::ExtContainer::~ExtContainer() = default;

// =============================
// container hooks

DEFINE_HOOK(0x413D30, AircraftClass_CTOR, 0x7)
{
	GET(AircraftClass*, pItem, ESI);

	AircraftExt::ExtMap.Allocate(pItem);

	return 0;
}

// Late in every destructor body of the class, right before it chains into the
// base destructor: the last point where the extension is no longer used.
DEFINE_HOOK_AGAIN(0x41426D, AircraftClass_DTOR, 0x9)
DEFINE_HOOK(0x4141FA, AircraftClass_DTOR, 0x9)
{
	GET(AircraftClass*, pItem, EDI);

	AircraftExt::ExtMap.Remove(pItem);

	return 0;
}

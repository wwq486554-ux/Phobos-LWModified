#include "Body.h"

#include <AircraftTrackerClass.h>
#include <Kamikaze.h>
#include <RocketLocomotionClass.h>
#include <algorithm>
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

	// ---- 预判制导 (Lead Pursuit) 的数值辅助 ----
	// 全程 double、以"每帧"为单位, 输入只取同步的游戏状态(双方坐标), 不含墙钟、
	// 渲染插值或随机数, 因此双端解算结果一致 (验收 A4)。
	constexpr double DefaultLeadFactor = 0.8; // 提前量系数: 0 = 纯追踪(现状), 1 = 理论拦截点
	constexpr int DefaultMaxLeadTime = 12; // 提前量时间上限 (帧, 约 0.8 秒 @15fps)
	constexpr int DefaultFuzeRadius = 288; // 扫掠段引信半径 (leptons, 约 2.25 格)
	constexpr double TargetVelocityEMA = 0.35; // 目标每帧位移的平滑系数

	double HomingDistance3DSq(const CoordStruct& a, const CoordStruct& b)
	{
		const double dx = static_cast<double>(a.X) - b.X;
		const double dy = static_cast<double>(a.Y) - b.Y;
		const double dz = static_cast<double>(a.Z) - b.Z;
		return dx * dx + dy * dy + dz * dz;
	}

	double HomingDistance3D(const CoordStruct& a, const CoordStruct& b)
	{
		return std::sqrt(HomingDistance3DSq(a, b));
	}

	// 点到线段的最短距离 (闭式解); 线段退化为一个点时即点到点距离。
	double HomingDistanceToSegment(const CoordStruct& p, const CoordStruct& a, const CoordStruct& b)
	{
		const double abx = static_cast<double>(b.X) - a.X;
		const double aby = static_cast<double>(b.Y) - a.Y;
		const double abz = static_cast<double>(b.Z) - a.Z;
		const double lenSq = abx * abx + aby * aby + abz * abz;

		if (lenSq <= 0.0)
			return HomingDistance3D(p, a);

		const double apx = static_cast<double>(p.X) - a.X;
		const double apy = static_cast<double>(p.Y) - a.Y;
		const double apz = static_cast<double>(p.Z) - a.Z;

		double t = (apx * abx + apy * aby + apz * abz) / lenSq;
		t = std::max(0.0, std::min(1.0, t));

		const double cx = apx - abx * t;
		const double cy = apy - aby * t;
		const double cz = apz - abz * t;
		return std::sqrt(cx * cx + cy * cy + cz * cz);
	}

	// 阶段 0 (扫掠引信): 用两帧之间的**相对位移**作线段, 求原点到它的最短距离。
	// 得到的是"这一帧内双方最近接近到多少", 而不是"帧末那一刻相距多少"。导弹在
	// 帧间穿过引信球时点距很大、段距却接近 0, 于是不会再漏炸。
	double HomingSweepFuzeDistance(const CoordStruct& misNow, const CoordStruct& misPrev,
		const CoordStruct& targetNow, const CoordStruct& targetPrev)
	{
		const CoordStruct relNow
		{
			misNow.X - targetNow.X,
			misNow.Y - targetNow.Y,
			misNow.Z - targetNow.Z
		};
		const CoordStruct relPrev
		{
			misPrev.X - targetPrev.X,
			misPrev.Y - targetPrev.Y,
			misPrev.Z - targetPrev.Z
		};

		return HomingDistanceToSegment(CoordStruct { 0, 0, 0 }, relPrev, relNow);
	}

	// 阶段 1 (T2, 预判拦截点):
	//   P/T = 导弹 / 目标当前坐标,  Vt = 目标每帧位移(EMA 平滑),  Vm = 导弹每帧位移(标量)
	//   t   = 相遇时间(帧), 不动点迭代两轮求解;   A = T + Vt * t * LeadFactor
	// 每帧都重解, 所以一次迭代就够, 两轮只是保险; Vt 趋于 0 时 A = T, 静止目标的
	// 行为与纯追踪完全一致 (验收 A1), 因此不需要按目标类型分支。
	CoordStruct HomingComputeAimPoint(const CoordStruct& misNow, const CoordStruct& targetNow,
		AircraftExt* pExt, AircraftTypeExt* pTypeExt)
	{
		// 系数越界一律钳住: LeadFactor 到 [0, 1] (0 = 关闭提前量, 1 = 理论拦截点),
		// MaxLeadTime 到 [0, 600] (上限同时兜住 std::lround 的溢出可能)。
		const double leadFactor = std::max(0.0, std::min(1.0, pTypeExt->Homing_LeadFactor.Get(DefaultLeadFactor)));
		const int maxLeadTime = std::max(0, std::min(600, pTypeExt->Homing_MaxLeadTime.Get(DefaultMaxLeadTime)));

		// 关闭提前量 / 上限非正 / 没有上一帧数据(首帧、读档后第一帧): 退化纯追踪
		if (!pExt->Homing_HasPrev || leadFactor <= 0.0 || maxLeadTime <= 0)
			return targetNow;

		// 目标速度: 本帧原始位移经 EMA 平滑, 抗单帧抖动
		const double rawVx = static_cast<double>(targetNow.X - pExt->Homing_TargetPrev.X);
		const double rawVy = static_cast<double>(targetNow.Y - pExt->Homing_TargetPrev.Y);
		const double rawVz = static_cast<double>(targetNow.Z - pExt->Homing_TargetPrev.Z);
		pExt->Homing_TargetVelX += TargetVelocityEMA * (rawVx - pExt->Homing_TargetVelX);
		pExt->Homing_TargetVelY += TargetVelocityEMA * (rawVy - pExt->Homing_TargetVelY);
		pExt->Homing_TargetVelZ += TargetVelocityEMA * (rawVz - pExt->Homing_TargetVelZ);

		// 导弹速度取标量, 用于把距离折算成帧数
		const double mvx = static_cast<double>(misNow.X - pExt->Homing_MissilePrev.X);
		const double mvy = static_cast<double>(misNow.Y - pExt->Homing_MissilePrev.Y);
		const double mvz = static_cast<double>(misNow.Z - pExt->Homing_MissilePrev.Z);
		const double missileSpeed = std::sqrt(mvx * mvx + mvy * mvy + mvz * mvz);

		if (missileSpeed < 1.0)
			return targetNow; // 刚出生 / 未加速: 不足以估计相遇时间

		const double maxT = static_cast<double>(maxLeadTime);
		const double targetDistSq = HomingDistance3DSq(misNow, targetNow);
		double t = std::sqrt(targetDistSq) / missileSpeed;
		CoordStruct aim = targetNow;

		for (int i = 0; i < 2; ++i)
		{
			t = std::max(0.0, std::min(maxT, t)); // 限幅, 防止远距离外推出荒唐的提前量

			aim.X = targetNow.X + static_cast<int>(std::lround(pExt->Homing_TargetVelX * t * leadFactor));
			aim.Y = targetNow.Y + static_cast<int>(std::lround(pExt->Homing_TargetVelY * t * leadFactor));
			aim.Z = targetNow.Z + static_cast<int>(std::lround(pExt->Homing_TargetVelZ * t * leadFactor));

			// 守卫: 提前量偏移一旦超过当前弹目距离, 瞄点就可能落到导弹身后。上面的不动点
			// 迭代用的是无符号距离, 当目标迎面接近得比导弹飞得还快(|Vt|*LeadFactor > Vm)
			// 时存在这种伪解(例如距离 300、目标 -100/帧、导弹 40/帧 -> 瞄点 -300, 等于
			// 命令导弹掉头); 目标瞬移时也会外推出荒唐的点。这里退回纯追踪: 目标正朝导弹
			// 来, 追当前点即可。偏移 <= 弹目距离 的几何含义是"瞄点始终位于过导弹且垂直
			// 于弹目连线的平面之前", 因此该守卫同时覆盖反向前置与横向外推两种病态, 而
			// 目标速度不超过导弹时(|Vt|*LeadFactor <= Vm)恒不触发。
			if (HomingDistance3DSq(aim, targetNow) > targetDistSq)
				return targetNow;

			if (i + 1 < 2) // 仅当还要再迭代一轮时才需要新的 t
				t = HomingDistance3D(misNow, aim) / missileSpeed;
		}

		return aim;
	}

	// 在单位坐标处施放导弹载荷, 并移除导弹本体。
	// 返回值: true = 已引爆并无声抹除弹体 (此后 pMissile 及其扩展均已失效, 调用方必须
	// 立刻返回, 不得再访问); false = 未配置可复用载荷, 什么都没做, 交回原版落地行为。
	bool HomingAirBurst(AircraftClass* pMissile, TechnoClass* pTarget, AircraftTypeExt* pTypeExt)
	{
		const bool elite = pMissile->SpawnOwner && pMissile->SpawnOwner->Veterancy.IsElite();
		const int damage = elite
			? pTypeExt->Homing_EliteDamage.Get(pTypeExt->Homing_Damage.Get())
			: pTypeExt->Homing_Damage.Get();
		auto const warhead = elite
			? pTypeExt->Homing_EliteWarhead.Get(pTypeExt->Homing_Warhead.Get())
			: pTypeExt->Homing_Warhead.Get();

		if (!warhead || damage <= 0)
			return false; // 该导弹未配置可复用的载荷: 交给原版落地行为兜底

		auto const pExt = AircraftExt::Fetch(pMissile);
		pExt->Homing_Active = false;
		pExt->Homing_Target = nullptr;

		// 在目标单位当前坐标(含高度)引爆
		WarheadTypeExt::DetonateAt(warhead, pTarget->GetCenterCoords(), pMissile, damage, pMissile->Owner);

		// 载荷已经由上面施加, 弹体必须"无声"抹除: 不能再走正常死亡流程, 否则命中点会
		// 再吃一次 TechnoClass::FireDeathWeapon (0x70D690) 打出的武器 —— 它先读类型的
		// DeathWeapon, 再回退 [CombatDamage] 的全局 DeathWeapon (MO 配了
		// DefaultDeathWeapon), Ares 也允许子机导弹自带武器/死亡武器 —— 同一发导弹就会
		// 造成两次伤害。
		// 这里的顺序与引擎自己的导弹载荷例程(0x663030 以 AircraftTrackerClass::Remove
		// 开头、以 UnInit 收尾)以及 Mix 在 0x662FD8 的清理写法一致。
		Kamikaze::Instance.Remove(pMissile);
		AircraftTrackerClass::Instance.Remove(pMissile);
		pMissile->UnInit();

		return true;
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
	// 新锁定的目标没有"上一帧"数据, 旧目标的速度也不能被带进来(失锁重锁可能换成别的
	// 目标): 清空瞬时状态, 从纯追踪起步, 下一帧起自动进入提前量解算。
	pExt->Homing_HasPrev = false;
	pExt->Homing_TargetVelX = 0.0;
	pExt->Homing_TargetVelY = 0.0;
	pExt->Homing_TargetVelZ = 0.0;
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

// Missile.Homing: 每帧刷新瞄准点 / 对空接近自爆 (由 AircraftClass 每帧更新处调用)。
// 返回 true = 对空自爆已把弹体无声抹除, 此后 pMissile 及其扩展均已失效, 调用方
// 必须立即收尾, 不得再访问; 返回 false = 弹体仍有效, 可继续按常规处理。
bool AircraftExt::UpdateMissileHoming(AircraftClass* pMissile)
{
	auto const pExt = AircraftExt::TryFetch(pMissile);
	if (!pExt || !pExt->Homing_Active)
		return false;

	if (!pMissile->IsAlive || pMissile->InLimbo)
	{
		pExt->Homing_Active = false;
		pExt->Homing_Target = nullptr;
		return false;
	}

	auto const pTypeExt = pExt->GetTypeExtData();
	if (!pTypeExt || !pTypeExt->Missile_Homing)
	{
		pExt->Homing_Active = false;
		pExt->Homing_Target = nullptr;
		return false;
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

		return false;
	}

	const bool targetInAir = pTarget->IsInAir();
	const int dist3d = pMissile->DistanceFrom3D(pTarget);

	const auto misCrd = pMissile->GetCoords();
	const auto targetCrd = pTarget->GetCenterCoords();

	if (targetInAir)
	{
		// 2D 水平距离与高度差
		const double dx = static_cast<double>(misCrd.X) - targetCrd.X;
		const double dy = static_cast<double>(misCrd.Y) - targetCrd.Y;
		const int distXY = static_cast<int>(std::sqrt(dx * dx + dy * dy));

		const int burstRangeXY = pTypeExt->Homing_AirBurstRangeXY.Get(DefaultAirBurstRangeXY);
		const int burstRangeZ = pTypeExt->Homing_AirBurstRangeZ.Get(DefaultAirBurstRangeZ);
		// FuzeRadius <= 0 等于关闭扫掠判定
		const int fuzeRadius = std::max(0, pTypeExt->Homing_FuzeRadius.Get(DefaultFuzeRadius));

		// 导弹已降到不高于飞机高度上方 burstRangeZ, 且水平已贴近单位 -> 在单位坐标引爆
		const bool descendedNearPlane = misCrd.Z <= targetCrd.Z + burstRangeZ;

		// 阶段 0 (扫掠段): 帧末点距没进引信圈, 但这一帧的相对运动轨迹曾穿过引信球时
		// 照样引爆, 消掉高速弹"擦身而过却不炸"。首帧(含读档后第一帧)没有上一帧数据时
		// 该判定自动失效, 退化为原来的点距判定。
		const bool sweptFuzeHit = pExt->Homing_HasPrev
			&& HomingSweepFuzeDistance(misCrd, pExt->Homing_MissilePrev, targetCrd, pExt->Homing_TargetPrev)
				<= static_cast<double>(fuzeRadius);

		if (dist3d < DefaultAirBurstRange3D
			|| sweptFuzeHit
			|| (distXY < burstRangeXY && descendedNearPlane))
		{
			// 引爆成功时弹体已被无声抹除(扩展同时失效), 必须立刻返回;
			// 未配置载荷时什么都没发生, 继续本帧的瞄准刷新。
			if (HomingAirBurst(pMissile, pTarget, pTypeExt))
				return true; // 弹体已抹除, 调用方不得再碰 pMissile / pExt
		}
	}

	if (!pExt->Homing_Active) // 防御: 制导已在别处关闭时不再写瞄点
		return false;

	// 阶段 1 (T2): 瞄准点由"目标当前坐标"升级为"预判拦截点"。LeadFactor=0、
	// 没有上一帧数据或导弹尚未移动时, 该函数原样返回目标当前坐标, 即退化为
	// 改动前的纯追踪 (验收 A1/A6)。
	auto aimCrd = HomingComputeAimPoint(misCrd, targetCrd, pExt, pTypeExt);

	// 地面目标: 瞄点 Z 归一到地面高度 (保持"触地爆炸"语义);
	// 空中目标: 保留完整 3D 坐标, 让导弹真的朝飞机爬升/俯冲。
	if (!targetInAir)
		aimCrd.Z = MapClass::Instance.GetCellFloorHeight(aimCrd);

	pExt->Homing_LastAim = aimCrd;

	// 方案 D (对齐 Kratos): 原版火箭的每帧转向步(0x662A32/steps)读取的是
	// RocketLocomotionClass::MovingDestination, 而不是 FootClass::Destination;
	// 只写后者不会被转向逻辑消费 —— 这正是此前制导"迟钝"的根因。
	// 每帧把瞄准点写进 loco 自己的 MovingDestination, 转向立即变连续。
	if (auto const pLoco = locomotion_cast<RocketLocomotionClass*>(pMissile->Locomotor))
		pLoco->MovingDestination = aimCrd;

	// 兜底: 仍同步一次 FootClass 目的地 (对非火箭 loco / 其他引擎流程保持兼容)
	if (auto const pCell = MapClass::Instance.TryGetCellAt(aimCrd))
		pMissile->SetDestination(pCell, true);

	// 记录本帧双方坐标, 供下一帧的提前量解算与扫掠引信使用。
	// 这些字段刻意不进 Serialize: 纯运行期状态, 读档后第一帧 Homing_HasPrev=false
	// 退化为纯追踪, 第二帧自动恢复, 也因此不占用存档布局 (验收 A5 允许的首帧退化)。
	pExt->Homing_MissilePrev = misCrd;
	pExt->Homing_TargetPrev = targetCrd;
	pExt->Homing_HasPrev = true;

	return false;
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

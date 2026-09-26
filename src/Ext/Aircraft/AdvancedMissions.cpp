#include <Ext/Aircraft/AdvancedMissions.h>

#include <Ext/Aircraft/Body.h>
#include <Ext/AircraftType/Body.h>
#include <Ext/Rules/Body.h>

#include <BuildingClass.h>
#include <CellClass.h>
#include <FlyLocomotionClass.h>
#include <MapClass.h>

#include <Utilities/Debug.h>

#include <cmath>

// =============================================================================
// AdvancedAircraftMissions —— 实现
//
// 设计约束（与 SpecialAction 的 §4.4 联机规矩一致）：
//   * 任何会改变单位状态的动作都由同步事件（EventTypeExt::SpecialActionReturn）
//     的响应者执行，或由每台机都跑的每帧仿真判据执行；
//   * 命令入口（技能键、右键）只负责"入队事件"，绝不直接写状态；
//   * 所有判据只读 INI（各机同步）与仿真状态（各机一致），不含随机数。
// =============================================================================

namespace
{
	// 水平距离（leptons）。盘旋/进近判定都只看水平面，和上游 hoverOverArchive 一致。
	int HorizontalDistance(const CoordStruct& a, const CoordStruct& b)
	{
		return static_cast<int>(Point2D { a.X, a.Y }.DistanceFrom(Point2D { b.X, b.Y }));
	}

	// 返航硬件资格：不含冷却、不含"有没有机场"。用于技能与右键两条路。
	bool MeetsReturnRequirements(AircraftClass* const pThis)
	{
		if (!pThis || !pThis->IsAlive || pThis->InLimbo)
			return false;

		if (!AdvancedMissions::Enabled(pThis->Type))
			return false;

		// 与上游 ExtendedAircraftMissions 的排除规则一致；子机另有回收逻辑，不参与。
		if (pThis->Team || pThis->Airstrike || pThis->IsALoaner || pThis->SpawnOwner)
			return false;

		if (!pThis->Type->AirportBound)
			return false;

		// 已经停稳在停机坪上时没有"空中巡航段"可言，返航无意义。
		if (!pThis->IsInAir())
			return false;

		return true;
	}

	// 返航窗口是否该结束（正常结束或取消）。
	// 关键：不能用"任务名 == Enter"当判据 —— AircraftClass::EnterIdleMode 在找到机场时
	// 最终就是 QueueMission(Mission::Enter)，自动返航也会进 Enter（见反汇编记录）。
	bool ReturnWindowShouldEnd(AircraftClass* const pThis)
	{
		if (!pThis->IsAlive || pThis->InLimbo)
			return true;

		// 已落地/停稳：硬兜底，保证倍率不会残留到下一次起飞
		if (!pThis->IsInAir())
			return true;

		// 已进入下降段（EarlyDescend / 原版 768 距离快降都以此为准）
		if (auto const pFly = locomotion_cast<FlyLocomotionClass*>(pThis->Locomotor))
		{
			if (pFly->IsElevating)
				return true;
		}

		// 明显是"被派去别处"的命令。故意不含 Move / Attack / Area_Guard / Enter / Sleep：
		// 返航流程内部可能自己跑 Move 甚至 Area_Guard，自动索敌也可能给出 Attack。
		switch (pThis->CurrentMission)
		{
		case Mission::Stop:
		case Mission::Guard:
		case Mission::Patrol:
		case Mission::Capture:
		case Mission::Unload:
		case Mission::Sabotage:
		case Mission::Harvest:
		case Mission::Repair:
		case Mission::Rescue:
		case Mission::Selling:
		case Mission::Retreat:
			return true;
		default:
			break;
		}

		auto const pExt = AircraftExt::Fetch(pThis);

		// 目的地被改写：
		//   * 新的目的地仍是自家停机设施 → 引擎自动改派另一个机场，保留加成（G10）
		//   * 其它目的地 → 只有还停在返航快照点附近才算"仍在返航"
		//
		// 快照可能还是空的（EnterIdleMode 有时要到下一帧才把目的地设好），
		// 这种情况下补记快照而不是取消，否则提速窗口会在第一帧就被自己掐掉。
		if (pThis->Destination && !AdvancedMissions::IsOwnDockBuilding(pThis, pThis->Destination))
		{
			if (!pExt->HasReturnOrderDest)
			{
				pExt->ReturnOrderDest = pThis->Destination->GetCoords();
				pExt->HasReturnOrderDest = true;
			}
			else if (HorizontalDistance(pThis->Destination->GetCoords(), pExt->ReturnOrderDest) > Unsorted::LeptonsPerCell * 2)
			{
				return true;
			}
		}

		// 已进入停机设施进近圈：交给原版降落
		if (pThis->DockNowHeadingTo)
		{
			const int threshold = Math::max(pThis->Type->SlowdownDistance, 768);

			if (HorizontalDistance(pThis->Location, pThis->DockNowHeadingTo->GetCoords()) <= threshold)
				return true;
		}

		return false;
	}
}

// =============================================================================
// 闸门与参数
// =============================================================================

bool AdvancedMissions::Enabled(AircraftTypeClass* const pType)
{
	if (!pType)
		return false;

	auto const pTypeExt = AircraftTypeExt::Fetch(pType);

	if (!pTypeExt->AdvancedAircraftMissions.Get(false))
		return false;

	return pTypeExt->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions);
}

int AdvancedMissions::GetLoiterRadius(AircraftTypeClass* const pType)
{
	if (!Enabled(pType))
		return 0;

	auto const pTypeExt = AircraftTypeExt::Fetch(pType);
	const int cells = pTypeExt->AdvancedAircraftMissions_LoiterRadius.Get(
		RulesExt::Global()->AdvancedAircraftMissions_LoiterRadius);

	return cells > 0 ? cells * Unsorted::LeptonsPerCell : 0;
}

bool AdvancedMissions::LoiterHover(AircraftTypeClass* const pType)
{
	if (!Enabled(pType))
		return false;

	auto const pTypeExt = AircraftTypeExt::Fetch(pType);

	return pTypeExt->AdvancedAircraftMissions_LoiterMode.Get(
		RulesExt::Global()->AdvancedAircraftMissions_LoiterMode);
}

bool AdvancedMissions::LoiterAutoTarget(AircraftTypeClass* const pType)
{
	if (!Enabled(pType))
		return true;

	auto const pTypeExt = AircraftTypeExt::Fetch(pType);

	return pTypeExt->AdvancedAircraftMissions_LoiterAutoTarget.Get(
		RulesExt::Global()->AdvancedAircraftMissions_LoiterAutoTarget);
}

double AdvancedMissions::GetReturnSpeedMultiplier(AircraftTypeClass* const pType)
{
	if (!Enabled(pType))
		return 1.0;

	auto const pTypeExt = AircraftTypeExt::Fetch(pType);
	const double mult = pTypeExt->AdvancedAircraftMissions_ReturnSpeedMultiplier.Get(
		RulesExt::Global()->AdvancedAircraftMissions_ReturnSpeedMultiplier);

	return mult > 0.0 ? mult : 1.0;
}

bool AdvancedMissions::ReturnWithoutDockLoiter(AircraftTypeClass* const pType)
{
	if (!Enabled(pType))
		return false;

	auto const pTypeExt = AircraftTypeExt::Fetch(pType);

	return pTypeExt->AdvancedAircraftMissions_ReturnWithoutDock.Get(
		RulesExt::Global()->AdvancedAircraftMissions_ReturnWithoutDock);
}

int AdvancedMissions::GetTurningRadius(AircraftClass* const pThis)
{
	if (!pThis || !pThis->Type)
		return 0;

	constexpr double epsilon = 1e-10;
	constexpr double raw2Radian = Math::TwoPi / 65536;
	// GetRadian<65536>() is an incorrect method
	const double rotRadian = std::abs(static_cast<double>(pThis->PrimaryFacing.ROT.Raw) * raw2Radian);

	return rotRadian > epsilon ? static_cast<int>(static_cast<double>(pThis->Type->Speed) / rotRadian) : 0;
}

int AdvancedMissions::GetHoverBrakeRange(AircraftClass* const pThis)
{
	// 旧实现把 1024 leptons（4 格）硬编码在 GetActiveSpeedMultiplier() 里；
	// 取不到配置时返回同一个值，保证"不写该键 = 行为逐字不变"。
	constexpr int defaultCells = 4;

	if (!pThis || !pThis->Type || !Enabled(pThis->Type))
		return defaultCells * Unsorted::LeptonsPerCell;

	auto const pTypeExt = AircraftTypeExt::TryFetch(pThis->Type);

	if (!pTypeExt)
		return defaultCells * Unsorted::LeptonsPerCell;

	// 类型段优先，[General] 同名键兜底（与其它 5 个键一致）。
	int cells = pTypeExt->AdvancedAircraftMissions_HoverBrakeRange.Get(
		RulesExt::Global()->AdvancedAircraftMissions_HoverBrakeRange);

	// <=0 不当成"关闭降速"：那会让 hover 静默退化成圆盘旋，是最难排查的一类配置错误。
	// 钳到 1 格，再由下面的物理下界兜底。
	if (cells <= 0)
		cells = 1;

	int range = cells * Unsorted::LeptonsPerCell;

	// ---- 物理下界 ----
	// 飞机要在距离 d 处收敛到悬停点，条件是该处的转弯半径小于 d：
	//   factor(d) = d / R  ⇒  (d / R) × turnRadius < d  ⇒  R > turnRadius
	// 即"减速起始距离必须大于机体自然转弯半径"。否则飞机到达悬停点时仍带速度，
	// 只能在半径 factor × turnRadius 的圈上绕，永远停不干净（退化成小圈盘旋）。
	// 留 20% 余量。turnRadius<=0（ROT 为 0 等异常）时不钳制。
	const int turnRadius = GetTurningRadius(pThis);

	if (turnRadius > 0)
		range = Math::max(range, turnRadius * 6 / 5);

	return range;
}

// =============================================================================
// 通用判据
// =============================================================================

bool AdvancedMissions::HasAmmo(TechnoClass* const pThis)
{
	if (!pThis)
		return false;

	// 类型 Ammo<=0 = 无限弹药：实例 Ammo 可能一直是 0，必须单独放行。
	const auto pType = pThis->GetTechnoType();

	return pThis->Ammo > 0 || (pType && pType->Ammo <= 0);
}

bool AdvancedMissions::IsOwnDockBuilding(AircraftClass* const pThis, AbstractClass* const pDest)
{
	if (!pThis || !pDest)
		return false;

	auto const pBuilding = abstract_cast<BuildingClass*, true>(pDest);

	if (!pBuilding || !pBuilding->IsAlive || !pBuilding->Type)
		return false;

	if (!pThis->Owner || !pBuilding->Owner || !pBuilding->Owner->IsAlliedWith(pThis->Owner))
		return false;

	if (pBuilding->Type->Helipad || pBuilding->Type->UnitReload)
		return true;

	return pThis->Type->Dock.FindItemIndex(pBuilding->Type) != -1;
}

bool AdvancedMissions::IsEligibleForReturn(AircraftClass* const pThis)
{
	return MeetsReturnRequirements(pThis);
}

bool AdvancedMissions::HasUsableDock(AircraftClass* const pThis)
{
	if (!pThis || !pThis->Type)
		return false;

	return pThis->TryNearestDockBuilding(&pThis->Type->Dock, 0, 0) != nullptr;
}

bool AdvancedMissions::CanOrderReturn(AircraftClass* const pThis)
{
	if (!MeetsReturnRequirements(pThis))
		return false;

	// 有机场 → 正常返航；没有机场 → 只有 ReturnWithoutDock=loiter 才允许（回家待命）
	return HasUsableDock(pThis) || ReturnWithoutDockLoiter(pThis->Type);
}

// =============================================================================
// 常驻盘旋
// =============================================================================

bool AdvancedMissions::IsLoitering(AircraftClass* const pThis)
{
	if (!pThis)
		return false;

	auto const pExt = AircraftExt::TryFetch(pThis);

	return pExt && pExt->Loiter_Active;
}

bool AdvancedMissions::TryBeginLoiter(AircraftClass* const pThis, AbstractClass* const pCenter)
{
	if (!pThis || !pCenter || !pThis->IsAlive || pThis->InLimbo)
		return false;

	if (!Enabled(pThis->Type))
		return false;

	if (pThis->Team || pThis->Airstrike || pThis->IsALoaner || pThis->SpawnOwner)
		return false;

	if (!pThis->Type->AirportBound)
		return false;

	// 没弹药就交回原版返航逻辑（G5）
	if (!HasAmmo(pThis))
		return false;

	// 没配半径 = 不盘旋（G2）
	if (GetLoiterRadius(pThis->Type) <= 0)
		return false;

	// 与上游 Guard → AreaGuard 的转换保持同一套状态：圆心进 ArchiveTarget，
	// 由 AircraftClass_Mission_AreaGuard 的 hoverOverArchive 负责盘旋。
	pThis->SetArchiveTarget(pCenter);
	pThis->SetDestination(pCenter, true);
	pThis->MissionStatus = 0;
	pThis->QueueMission(Mission::Area_Guard, false);

	AircraftExt::Fetch(pThis)->Loiter_Active = true;
	return true;
}

void AdvancedMissions::EndLoiter(AircraftClass* const pThis)
{
	if (!pThis)
		return;

	auto const pExt = AircraftExt::TryFetch(pThis);

	if (!pExt || !pExt->Loiter_Active)
		return;

	pExt->Loiter_Active = false;
	pThis->SetArchiveTarget(nullptr);

	if (pThis->CurrentMission == Mission::Area_Guard)
		pThis->MissionStatus = 0;
}

// =============================================================================
// 手动返航
// =============================================================================

bool AdvancedMissions::StartReturnToBase(AircraftClass* const pThis)
{
	if (!MeetsReturnRequirements(pThis))
		return false;

	auto const pExt = AircraftExt::Fetch(pThis);
	EndLoiter(pThis);

	// ---- 有机场：正常返航 + 巡航段提速 ----
	if (HasUsableDock(pThis))
	{
		const double mult = GetReturnSpeedMultiplier(pThis->Type);

		if (mult != 1.0)
		{
			pExt->ReturnOrderActive = true;
			pExt->ReturnSpeedMultiplier = mult;
		}

		pThis->EnterIdleMode(false, true);

		// 快照返航目的地（EnterIdleMode 内部才设），供每帧"被改派"判据使用
		AbstractClass* const pDest = pThis->Destination ? pThis->Destination : pThis->DockNowHeadingTo;

		if (pDest)
		{
			pExt->ReturnOrderDest = pDest->GetCoords();
			pExt->HasReturnOrderDest = true;
		}

		return true;
	}

	// ---- 无可用机场 ----
	if (!ReturnWithoutDockLoiter(pThis->Type))
		return false; // deny：调用方播 NotReady、不扣冷却

	// loiter：就地盘旋等机场（不加成，CD 照常花掉）
	AbstractClass* const pCenter = pThis->Destination
		? pThis->Destination
		: MapClass::Instance.TryGetCellAt(pThis->GetCoords());

	if (pCenter && TryBeginLoiter(pThis, pCenter))
		return true;

	// 盘旋条件不满足（例如没弹药）：交回引擎的无机场处理
	pThis->EnterIdleMode(false, true);
	return true;
}

void AdvancedMissions::CancelReturnBoost(AircraftClass* const pThis)
{
	if (!pThis)
		return;

	if (auto const pExt = AircraftExt::TryFetch(pThis))
	{
		pExt->ReturnOrderActive = false;
		pExt->ReturnSpeedMultiplier = 1.0;
	}
}

double AdvancedMissions::GetActiveSpeedMultiplier(FootClass* const pThis)
{
	auto const pAircraft = abstract_cast<AircraftClass*, true>(pThis);

	if (!pAircraft)
		return 1.0;

	auto const pExt = AircraftExt::TryFetch(pAircraft);

	if (!pExt)
		return 1.0;

	// 手动返航提速（窗口外恒为 1.0）
	if (pExt->ReturnOrderActive)
		return pExt->ReturnSpeedMultiplier;

	// ---- LoiterMode=hover：靠"降速"实现悬停 ----
	//
	// 为什么只能这么做（第四轮实测 + 反汇编定案）：
	//   Fly 飞控的速度就是 `Type->Speed × CurrentSpeed × 倍率`（0x4CDA78 等三处），
	//   **没有任何距离项 → 恒速**；引擎里不存在"速度=0"的状态，飞机永远无法停住，
	//   只能在半径 = 实际速度 / ROT 的圆上绕（实测稳定绕 619 leptons ≈ 2.4 格）。
	//   而转弯半径正比于**实际速度**：把速度乘 k，可达的最小圆就缩成 k 倍。
	//   所以贴住悬停点时把倍率压到 0，圈半径也趋近 0 —— 视觉上就是定在原地。
	//
	// 只在本功能的盘旋 + Area_Guard 期间生效：出击（Attack）与返航期间速度照常。
	// 超出 AdvancedAircraftMissions.HoverBrakeRange 格全速飞回，进圈按"匀减速"降速，
	// 贴住悬停点时降到 0（真停）。起始距离默认 4 格，且被钳到不小于机体
	// 自然转弯半径的 1.2 倍（GetHoverBrakeRange）。
	//
	// 速度剖面（第五轮修正，见下）分两段：
	//   ① 匀减速段  v ∝ √(d/R)   —— 减速度恒定，这才是"均匀减速到目标格"。
	//   ② 收敛段    v ∝ d/turnRadius —— 末端必须让转弯半径小于剩余距离，否则贴不上点。
	// 取 0 是有依据的：起降时引擎自己就是把 `CurrentSpeed` 压到 0
	// （MovementAI 0x4CD669 判 `Mission::Enter` → 起降分支；
	//   Fly Process 0x4CCB4C/4CCB53 在 IsLanding/WasLanding 时跳过目标速度更新），
	// 水平速度与垂直速度同源于 `Speed × CurrentSpeed × 倍率`，
	// 所以"水平速度为 0"是引擎的正常状态，不是边界情况。
	if (pExt->Loiter_Active
		&& pAircraft->CurrentMission == Mission::Area_Guard
		&& pAircraft->ArchiveTarget
		&& LoiterHover(pAircraft->Type))
	{
		const int rampDistance = GetHoverBrakeRange(pAircraft);

		const int dist = HorizontalDistance(pAircraft->Location, pAircraft->ArchiveTarget->GetCoords());

		if (dist < rampDistance)
		{
			// ① 匀减速段：v = vMax × √(d/R) ⇒ a = vMax²/(2R) = 常数。
			//    旧的 v ∝ d/R 看着像"线性斜坡"，实际是**指数收敛**：每把距离减半
			//    才把速度减半，时间常数固定为 R/vMax，所以 R 一调大，最后一段就
			//    慢得几乎到不了位（实机反馈："离目标格很近的时候剧烈减速"）。
			const double uniformBrake = std::sqrt(static_cast<double>(dist) / rampDistance);

			// ② 收敛段：转弯半径正比于实际速度，飞机要能真正贴上悬停点，必须满足
			//      实际转弯半径 = f × turnRadius < d   ⇒   f < d / turnRadius
			//    √d 在贴点时下降太慢（f 需要比 d 更低阶才收敛），会退化成半径
			//    ≈ turnRadius²/R 的小圈盘旋；所以末端换成带 20% 余量的线性项
			//    f = 0.8 × d / turnRadius —— 它天然满足收敛条件，而且**与 R 无关**：
			//    把 HoverBrakeRange 调大只会让减速更早、更缓，最后一段的到位速度
			//    不受影响（这正是第五轮要修的那条反馈）。
			const int turnRadius = GetTurningRadius(pAircraft);

			if (turnRadius > 0)
				return Math::min(uniformBrake, 0.8 * static_cast<double>(dist) / turnRadius);

			return uniformBrake;
		}
	}

	return 1.0;
}

// =============================================================================
// 每帧状态机
// =============================================================================

void AdvancedMissions::Update(AircraftClass* const pThis)
{
	if (!pThis)
		return;

	auto const pExt = AircraftExt::TryFetch(pThis);

	if (!pExt)
		return;

	// 闸门未开的机型：本功能对它零足迹——连状态字段都不写。
	// 所有写 Loiter_Active / ReturnOrderActive 的入口本身也都带这道闸门，
	// 所以这里提前返回不会漏掉任何需要收尾的状态。
	if (!Enabled(pThis->Type))
		return;

	// 击杀点：目标在的时候每帧刷新。目标被击毁后 Target 会变空，
	// 所以必须在还有目标时就记录下来（G2 的"目标最后已知位置"）。
	if (pThis->Target)
	{
		pExt->Loiter_KillSpot = pThis->Target->GetCoords();
		pExt->Loiter_HasKillSpot = true;
	}

	// 盘旋状态收尾：一旦不再处于"带圆心的 Area_Guard"（例如弹药耗尽被原版逻辑
	// 接手返航），就清掉标志，避免陈旧状态影响下一次出击。
	// 只清标志、不动 ArchiveTarget，避免误伤引擎/上游自己设置的存档目标。
	if (pExt->Loiter_Active
		&& (pThis->CurrentMission != Mission::Area_Guard || !pThis->ArchiveTarget))
	{
		pExt->Loiter_Active = false;
	}

	// 悬停状态机：一旦离开 Area_Guard（出击 / 被改派 / 返航）就复位，
	// 这样打完回来时能重新下一次 Move_To 飞回悬停点。
	if (pThis->CurrentMission != Mission::Area_Guard)
		pExt->Loiter_HoverState = 0;

	// 返航窗口收尾
	if (pExt->ReturnOrderActive && ReturnWindowShouldEnd(pThis))
		CancelReturnBoost(pThis);
}

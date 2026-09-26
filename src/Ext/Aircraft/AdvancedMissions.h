#pragma once

#include <AircraftClass.h>
#include <AircraftTypeClass.h>

// AdvancedAircraftMissions —— 战机常驻盘旋 + 手动返航提速。
//
// 闸门（两者都要满足）：
//   1. ExtendedAircraftMissions（类型段优先，[General] 兜底）= true
//   2. AdvancedAircraftMissions=yes（仅类型段）
//
// 参数键（类型段优先，[General] 兜底）：
//   AdvancedAircraftMissions.LoiterRadius           盘旋半径（格），不写/<=0 = 不盘旋
//   AdvancedAircraftMissions.LoiterMode             circle（默认，绕圈）| hover（原地悬浮）
//   AdvancedAircraftMissions.HoverBrakeRange        hover 减速起始距离（格），默认 4
//   AdvancedAircraftMissions.LoiterAutoTarget       盘旋是否主动索敌，默认 yes
//   AdvancedAircraftMissions.ReturnSpeedMultiplier  手动返航速度倍率，默认 1.0
//   AdvancedAircraftMissions.ReturnWithoutDock      无可用机场时的行为：deny|loiter
//
// 手动返航的两条触发路径（技能 SpecialAction=return、玩家右键自家机场）共用
// EventTypeExt::SpecialActionReturn 这一条同步事件链；状态只允许由事件响应者和
// 每帧仿真判据写，生产端（本机命令入口）只入队。
//
// 完整规格、反汇编结论与实机待测项见工作区文档
// docs/AdvancedAircraftMissions-详解.md。
class AdvancedMissions
{
public:
	// ---- 闸门与参数 ----
	static bool Enabled(AircraftTypeClass* const pType);
	static int GetLoiterRadius(AircraftTypeClass* const pType); // leptons，<=0 = 不盘旋
	// hover 的"减速起始距离"（leptons）：距离悬停点小于它时倍率按线性斜坡降速。
	// 键值单位是格，不写 = 4 格（1024 leptons，与旧硬编码逐字相同）；
	// 返回值含物理下界钳制（必须大于机体自然转弯半径，见 .cpp）。
	static int GetHoverBrakeRange(AircraftClass* const pThis);
	// 机体自然转弯半径（leptons）= Speed / ROT(弧度)。<=0 表示无法计算。
	// 单一来源：本模块的 hover 钳制与 Ext/Aircraft/Hooks.cpp 的到位判定共用。
	static int GetTurningRadius(AircraftClass* const pThis);
	// circle=false | hover=true。语义：盘旋时"动不动"；LoiterAutoTarget 只管"搜不搜"。
	// hover 下 LoiterRadius 只保留"有没有开盘旋"的语义。
	static bool LoiterHover(AircraftTypeClass* const pType);
	static bool LoiterAutoTarget(AircraftTypeClass* const pType);
	static double GetReturnSpeedMultiplier(AircraftTypeClass* const pType);
	static bool ReturnWithoutDockLoiter(AircraftTypeClass* const pType);

	// ---- 通用判据 ----
	// 类型 Ammo<=0 视为无限弹药（判据与 Ext/Script/Body.cpp 一致）
	static bool HasAmmo(TechnoClass* const pThis);
	// pDest 是否是该机（或其盟友）可以降落的停机设施
	static bool IsOwnDockBuilding(AircraftClass* const pThis, AbstractClass* const pDest);
	// 是否满足"能用手动返航"的全部硬件条件（不含冷却）
	static bool IsEligibleForReturn(AircraftClass* const pThis);
	// 现在是否有可落机场
	static bool HasUsableDock(AircraftClass* const pThis);
	// 生产端（本机）资格：能返航，且（有机场 或 ReturnWithoutDock=loiter）
	static bool CanOrderReturn(AircraftClass* const pThis);

	// ---- 常驻盘旋 ----
	static bool IsLoitering(AircraftClass* const pThis);
	static bool TryBeginLoiter(AircraftClass* const pThis, AbstractClass* const pCenter);
	static void EndLoiter(AircraftClass* const pThis);

	// ---- 手动返航（响应端，每台机调用） ----
	static bool StartReturnToBase(AircraftClass* const pThis);
	static void CancelReturnBoost(AircraftClass* const pThis);
	// 当前应乘进飞行速度的返航倍率；不在提速窗口内时恒为 1.0。
	// 由 Misc/Hooks.BugFixes.cpp 的三处飞行速度 hook 调用。
	static double GetActiveSpeedMultiplier(FootClass* const pThis);

	// ---- 每帧状态机（挂在 AircraftClass::Update 尾部） ----
	static void Update(AircraftClass* const pThis);
};

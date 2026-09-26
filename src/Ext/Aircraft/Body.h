#pragma once
#include <Ext/Foot/Body.h>
#include <Ext/AircraftType/Body.h>
#include <AircraftClass.h>

// Concrete leaf extension for AircraftClass.
class AircraftExt final : public FootExt
{
public:
	using base_type = AircraftClass;

	static constexpr DWORD Canary = 0xA1A2A3A4;

	int Strafe_BombsDroppedThisRound;
	CellClass* Strafe_TargetCell;
	int CurrentAircraftWeaponIndex;

	// AdvancedAircraftMissions —— 常驻盘旋 / 手动返航提速（详见 docs 里的详解）
	bool Loiter_Active; // 本机当前处于"到达后常驻盘旋"状态
	bool Loiter_HasKillSpot; // Loiter_KillSpot 是否有效
	CoordStruct Loiter_KillSpot; // 攻击目标最后已知位置（盘旋圆心候选）
	bool ReturnOrderActive; // 正处于"手动返航提速窗口"
	double ReturnSpeedMultiplier; // 1.0 = 不生效；否则乘进飞行速度
	bool HasReturnOrderDest; // ReturnOrderDest 是否有效
	CoordStruct ReturnOrderDest; // 下达返航时的目的地快照（用于识别"被改派"）
	// LoiterMode=hover 的悬停状态机：0=未下令 / 1=正飞向悬停点 / 2=已停稳。
	// 必须存状态：Fly 的 Move_To 每调用一次就把 HasMoveOrder 置 1，而 Is_Moving()
	// 读的正是它 —— 每帧重下 Move_To 会让飞机永远"到不了位"，只能绕着悬停点打转。
	int Loiter_HoverState;

	// Missile.Homing —— 子机导弹单位级制导 (每颗导弹一份)
	bool Homing_Active; // 本颗导弹当前是否处于制导飞行
	TechnoClass* Homing_Target; // 锁定的目标 (死亡/隐形后置空, 靠 Homing_LastAim 兜底)
	CoordStruct Homing_LastAim; // 失锁后继续飞往并引爆的最后瞄准点

	explicit AircraftExt(AircraftClass* const OwnerObject) : FootExt(OwnerObject)
		, Strafe_BombsDroppedThisRound { 0 }
		, Strafe_TargetCell { nullptr }
		, CurrentAircraftWeaponIndex {}
		, Loiter_Active { false }
		, Loiter_HasKillSpot { false }
		, Loiter_KillSpot {}
		, ReturnOrderActive { false }
		, ReturnSpeedMultiplier { 1.0 }
		, HasReturnOrderDest { false }
		, ReturnOrderDest {}
		, Loiter_HoverState { 0 }
		, Homing_Active { false }
		, Homing_Target { nullptr }
		, Homing_LastAim {}
	{ }

	AircraftClass* OwnerObject() const
	{
		return static_cast<AircraftClass*>(this->GetAttachedObject());
	}

	// an aircraft's type extension is always the AircraftTypeExt leaf
	AircraftTypeExt* GetTypeExtData() const
	{
		return static_cast<AircraftTypeExt*>(this->TypeExtData);
	}

	static void FireWeapon(AircraftClass* pThis, AbstractClass* pTarget);
	static bool PlaceReinforcementAircraft(AircraftClass* pThis, CoordStruct edgeCoords);
	static CellStruct PickEdgeCellForPlane(AircraftTypeClass* pPlaneType, CellStruct destCell, Edge edge, bool isOnRetreat = false);
	static DirType GetLandingDir(AircraftClass* pThis, BuildingClass* pDock = nullptr, bool isProduction = false);
	static AircraftTypeClass* GetAircraftTypeExtra(AircraftClass* pAircraft);

	// Missile.Homing 制导: 发射时锁定目标 / 每帧刷新瞄准
	static void StartMissileHoming(AircraftClass* pMissile, TechnoClass* pTarget);
	static void UpdateMissileHoming(AircraftClass* pMissile);
	// 目标是否仍在 TechnoClass::Array 中 (对象删除前必先移出数组);
	// 用于在对目标解引用前验活, 防止访问已释放对象。
	static bool IsTrackedTargetValid(TechnoClass* pTarget);

	class ExtContainer final : public Container<AircraftExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	static AircraftExt* Fetch(const AircraftClass* pThis)
	{
		return AbstractExt::Fetch<AircraftExt>(pThis);
	}

	static AircraftExt* TryFetch(const AircraftClass* pThis)
	{
		return AbstractExt::TryFetch<AircraftExt>(pThis);
	}

	virtual void LoadFromStream(PhobosStreamReader& Stm) override;
	virtual void SaveToStream(PhobosStreamWriter& Stm) override;

private:
	template <typename T>
	void Serialize(T& Stm);
};

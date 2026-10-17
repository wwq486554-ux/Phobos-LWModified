#pragma once

#include <Ext/TechnoType/Body.h>
#include <AircraftTypeClass.h>
#include <WarheadTypeClass.h>

// Concrete leaf extension for AircraftTypeClass (empty).
class AircraftTypeExt final : public TechnoTypeExt
{
public:
	using base_type = AircraftTypeClass;

	static constexpr DWORD Canary = 0xA5A6A7A8;

	NullableIdx<VocClass> VoicePickup; // Used by carryalls instead of VoiceMove if set.
	Nullable<EdgeType> SpawnFromEdge;
	Nullable<EdgeType> RetreatToEdge;
	Nullable<Leptons> SpawnDistanceFromTarget;
	Nullable<int> SpawnHeight;
	Nullable<int> LandingDir;
	Nullable<bool> CurleyShuffle;
	Nullable<bool> ExtendedAircraftMissions;
	Nullable<bool> ExtendedAircraftMissions_SmoothMoving;
	Nullable<bool> ExtendedAircraftMissions_EarlyDescend;
	Nullable<bool> ExtendedAircraftMissions_RearApproach;
	Nullable<bool> ExtendedAircraftMissions_FastScramble;
	Nullable<int> ExtendedAircraftMissions_UnlandDamage;

	// AdvancedAircraftMissions —— 战机常驻盘旋 / 手动返航提速（类型段总闸 + 参数）
	Nullable<bool> AdvancedAircraftMissions;
	Nullable<int> AdvancedAircraftMissions_LoiterRadius; // 格，<=0/不写 = 不盘旋
	Nullable<bool> AdvancedAircraftMissions_LoiterMode; // circle=false | hover=true
	Nullable<int> AdvancedAircraftMissions_HoverBrakeRange; // 格，hover 减速起始距离（默认 4）
	Nullable<bool> AdvancedAircraftMissions_LoiterAutoTarget;
	Nullable<double> AdvancedAircraftMissions_ReturnSpeedMultiplier;
	Nullable<bool> AdvancedAircraftMissions_ReturnWithoutDock; // deny=false | loiter=true
	Nullable<bool> FiringForceScatter;
	Nullable<int> ParadropDelay;
	Nullable<int> ParadropEndDelay;
	Nullable<bool> FlyNoWobbles;
	Nullable<bool> IsALoaner;
	Nullable<AnimTypeClass*> LandingAnim;
	Valueable<bool> Missile_Cruise;
	Valueable<AnimTypeClass*> Missile_TakeOffAnim;
	Valueable<int> Missile_TakeOffSeparation;
	Valueable<bool> Missile_Homing; // Missile.Homing - 子机导弹单位级制导
	Nullable<int> Homing_Damage; // Missile.Damage (Ares 同节键, 对空自引爆用)
	Nullable<int> Homing_EliteDamage; // Missile.EliteDamage
	Nullable<WarheadTypeClass*> Homing_Warhead; // Missile.Warhead
	Nullable<WarheadTypeClass*> Homing_EliteWarhead; // Missile.EliteWarhead
	Nullable<int> Homing_AirBurstRangeXY; // Missile.Homing.AirBurstRangeXY (leptons, 对空水平命中圈)
	Nullable<int> Homing_AirBurstRangeZ; // Missile.Homing.AirBurstRangeZ (leptons, 目标高度上方容差)
	Nullable<int> Homing_CruiseSkipRange; // Missile.Homing.CruiseSkipRange (leptons, 进入俯冲前的最小水平距离)
	Nullable<double> Homing_LeadFactor; // Missile.Homing.LeadFactor (0 = 纯追踪=现状, 1 = 理论拦截点)
	Nullable<int> Homing_MaxLeadTime; // Missile.Homing.MaxLeadTime (帧, 提前量时间上限)
	Nullable<int> Homing_FuzeRadius; // Missile.Homing.FuzeRadius (leptons, 扫掠段引信半径)
	Nullable<bool> Homing_AbortDive; // Missile.Homing.AbortDive (yes=目标还远时可中止俯冲拉起, no=提交俯冲砸地)

	// Missile.Scatter —— 子机导弹弹道散布 (MissileSpawn=yes 的 AircraftType)
	// 键写在导弹自己的类型段; 不写 Missile.Scatter 或半径全 0 时行为与原版逐位一致。
	Valueable<bool> Scatter_Enabled; // Missile.Scatter (总闸)
	Valueable<double> Scatter_Min; // Missile.Scatter.Min (格, 散布半径下限; 负值按 0)
	Valueable<double> Scatter_Max; // Missile.Scatter.Max (格, 散布半径上限; Max<Min 时交换)
	Valueable<double> Scatter_Sigma; // Missile.Scatter.Sigma (半径分布指数, 夹 [0.5,16])
	Valueable<double> Scatter_Aspect; // Missile.Scatter.Aspect (面积守恒椭圆比, 夹 [0.125,8])
	Nullable<double> Scatter_Divergence; // Missile.Scatter.Divergence (留空 = 继承 [General] Divergence)

	explicit AircraftTypeExt(AircraftTypeClass* const OwnerObject) : TechnoTypeExt(OwnerObject)
		, VoicePickup {}
		, SpawnFromEdge {}
		, RetreatToEdge {}
		, SpawnDistanceFromTarget {}
		, SpawnHeight {}
		, LandingDir {}
		, CurleyShuffle {}
		, ExtendedAircraftMissions {}
		, ExtendedAircraftMissions_SmoothMoving {}
		, ExtendedAircraftMissions_EarlyDescend {}
		, ExtendedAircraftMissions_RearApproach {}
		, ExtendedAircraftMissions_FastScramble {}
		, ExtendedAircraftMissions_UnlandDamage {}
		, AdvancedAircraftMissions {}
		, AdvancedAircraftMissions_LoiterRadius {}
		, AdvancedAircraftMissions_LoiterMode {}
		, AdvancedAircraftMissions_HoverBrakeRange {}
		, AdvancedAircraftMissions_LoiterAutoTarget {}
		, AdvancedAircraftMissions_ReturnSpeedMultiplier {}
		, AdvancedAircraftMissions_ReturnWithoutDock {}
		, FiringForceScatter {}
		, ParadropDelay {}
		, ParadropEndDelay {}
		, FlyNoWobbles {}
		, IsALoaner {}
		, LandingAnim {}
		, Missile_Cruise { false }
		, Missile_TakeOffAnim { nullptr }
		, Missile_TakeOffSeparation { 24 }
		, Missile_Homing { false }
		, Homing_Damage {}
		, Homing_EliteDamage {}
		, Homing_Warhead {}
		, Homing_EliteWarhead {}
		, Homing_AirBurstRangeXY {}
		, Homing_AirBurstRangeZ {}
		, Homing_CruiseSkipRange {}
		, Homing_LeadFactor {}
		, Homing_MaxLeadTime {}
		, Homing_FuzeRadius {}
		, Homing_AbortDive {}
		, Scatter_Enabled { false }
		, Scatter_Min { 0.0 }
		, Scatter_Max { 1.5 }
		, Scatter_Sigma { 1.0 }
		, Scatter_Aspect { 1.0 }
		, Scatter_Divergence {}
	{ }

	AircraftTypeClass* OwnerObject() const
	{
		return static_cast<AircraftTypeClass*>(this->GetAttachedObject());
	}

	// Missile.Scatter: 是否对本类型生效 (总闸 + 必须是 MissileSpawn 导弹)
	bool IsMissileScatterEnabled() const;
	// Missile.Scatter.Divergence: 留空时继承 [General] Divergence (与通用散布一致)
	double GetMissileScatterDivergence() const;

	class ExtContainer final : public Container<AircraftTypeExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	static AircraftTypeExt* Fetch(const AircraftTypeClass* pThis)
	{
		return AbstractExt::Fetch<AircraftTypeExt>(pThis);
	}

	static AircraftTypeExt* TryFetch(const AircraftTypeClass* pThis)
	{
		return AbstractExt::TryFetch<AircraftTypeExt>(pThis);
	}

	virtual void Initialize() override;
	virtual void LoadFromINIFile(CCINIClass* pINI) override;
	virtual void LoadFromStream(PhobosStreamReader& Stm) override;
	virtual void SaveToStream(PhobosStreamWriter& Stm) override;

private:
	template <typename T>
	void Serialize(T& Stm);
};

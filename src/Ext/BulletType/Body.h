#pragma once
#include <BulletTypeClass.h>

#include <Ext/ObjectType/Body.h>
#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

#include <New/Type/LaserTrailTypeClass.h>

#include <Ext/Bullet/Trajectories/PhobosTrajectory.h>

class BulletTypeExt final : public ObjectTypeExt
{
public:
	using base_type = BulletTypeClass;

	// deprecated: the pre-rework nested data class is now the extension class itself
	using ExtData [[deprecated("use the extension class itself instead")]] = BulletTypeExt;

	static constexpr DWORD Canary = 0xF00DF00D;

public:
	// typed owner accessor
	BulletTypeClass* OwnerObject() const
	{
		return static_cast<BulletTypeClass*>(this->GetAttachedObject());
	}

	// Valueable<int> Strength; //Use OwnerObject()->ObjectTypeClass::Strength
	Nullable<ArmorType> Armor;
	Nullable<bool> Interceptable;
	Valueable<bool> Interceptable_DeleteOnIntercept;
	Valueable<WeaponTypeClass*> Interceptable_WeaponOverride;
	ValueableIdxVector<LaserTrailTypeClass> LaserTrail_Types;
	Nullable<double> Gravity;
	Nullable<bool> Vertical_AircraftFix;
	Nullable<bool> VerticalInitialFacing;

	TrajectoryTypePointer TrajectoryType;

	Valueable<int> LifeDuration;
	Valueable<int> NoTargetLifeTime;
	Valueable<int> CreateCapacity;
	Valueable<int> RetargetInterval;
	Valueable<double> RetargetRadius;
	Valueable<AffectedHouse> RetargetHouses;
	Valueable<bool> Synchronize;
	Nullable<bool> PeacefulVanish;
	Valueable<bool> ApplyRangeModifiers;
	Valueable<bool> UseDisperseCoord;

	Valueable<bool> PassDetonate;
	Valueable<bool> PassDetonateLocal;
	Valueable<WarheadTypeClass*> PassDetonateWarhead;
	Nullable<int> PassDetonateDamage;
	Valueable<int> PassDetonateDelay;
	Valueable<int> PassDetonateInitialDelay;
	Valueable<int> ProximityImpact;
	Valueable<WarheadTypeClass*> ProximityWarhead;
	Nullable<int> ProximityDamage;
	Valueable<Leptons> ProximityRadius;
	Valueable<bool> ProximityDirect;
	Valueable<bool> ProximityMedial;
	Valueable<bool> ProximityAllies;
	Valueable<bool> ProximityFlight;
	Valueable<bool> ProximitySphere;
	Valueable<bool> ThroughVehicles;
	Valueable<bool> ThroughBuilding;
	Valueable<double> DamageEdgeAttenuation;
	Valueable<double> DamageCountAttenuation;

	ValueableVector<WeaponTypeClass*> DisperseWeapons;
	ValueableVector<int> DisperseBursts;
	ValueableVector<int> DisperseCounts;
	ValueableVector<int> DisperseDelays;
	Valueable<int> DisperseCycle;
	Valueable<int> DisperseInitialDelay;
	Valueable<Leptons> DisperseEffectiveRange;
	Valueable<bool> DisperseSeparate;
	Valueable<bool> DisperseRetarget;
	Valueable<bool> DisperseLocation;
	Valueable<bool> DisperseTendency;
	Valueable<bool> DisperseHolistic;
	Valueable<bool> DisperseMarginal;
	Valueable<bool> DisperseDoRepeat;
	Valueable<bool> DisperseSuicide;
	Nullable<bool> DisperseFromFirer;
	Valueable<bool> DisperseFaceCheck;
	Valueable<bool> DisperseForceFire;
	Valueable<CoordStruct> DisperseCoord;

	Nullable<bool> Shrapnel_AffectsGround;
	Nullable<bool> Shrapnel_AffectsBuildings;
	Nullable<bool> Shrapnel_UseWeaponTargeting;
	Nullable<bool> Shrapnel_IgnoreHitBuildings;
	Nullable<bool> Shrapnel_ObeyWarheadTriggerConditions;
	Nullable<bool> SubjectToLand;
	Valueable<bool> SubjectToLand_Detonate;
	Nullable<bool> SubjectToWater;
	Valueable<bool> SubjectToWater_Detonate;

	Valueable<Leptons> ClusterScatter_Min;
	Valueable<Leptons> ClusterScatter_Max;

	Valueable<bool> AAOnly;
	Nullable<bool> Arcing_AllowElevationInaccuracy;
	Valueable<WeaponTypeClass*> ReturnWeapon;
	Nullable<bool> ReturnWeapon_ApplyFirepowerMult;

	Valueable<bool> SubjectToGround;
	Valueable<bool> SubjectToSolid;

	Valueable<bool> BallisticScatter_IncreaseByRange;
	Nullable<Leptons> BallisticScatter_MinRange;
	Nullable<Leptons> BallisticScatter_MaxRange;
	Nullable<Leptons> BallisticScatter_Min_InMinRange;
	Nullable<Leptons> BallisticScatter_Min_InMaxRange;
	Nullable<Leptons> BallisticScatter_Max_InMinRange;
	Nullable<Leptons> BallisticScatter_Max_InMaxRange;
	Valueable<double> BallisticScatter_Chance;

	// Universal scatter shaping. Applied to every scatter path: classic FlakScatter
	// (Unlimbo), visible Fire_At scatter, Phobos trajectories and ClusterScatter.
	// - Sigma: radius distribution exponent. 1.0 = uniform in radius (legacy behaviour),
	//   0.5 = uniform in area, >1 = biased towards the centre (each +1 halves the median radius).
	// - Aspect: area-conserving ellipse ratio (long axis / short axis). 1.0 = circle.
	// - Divergence: reshapes how the scatter grows with distance. See BulletExt::
	//   ScatterDistanceFactor. 1.0 = the engine's linear ramp, smaller = scatter stays
	//   tight up close and climbs to Max near maximum range, larger = the ramp flattens
	//   out towards a distance-independent radius. Only a lower bound of 0.05 is enforced.
	//   When this projectile does not write the key, the global [General] Divergence is
	//   used instead - resolve it through GetScatterDivergence(), never read the field
	//   directly, or the global default is silently skipped.
	Valueable<double> Scatter_Sigma;
	Valueable<double> Scatter_Aspect;
	Nullable<double> Scatter_Divergence;

	Valueable<bool> Splits;
	Valueable<double> AirburstSpread;
	Valueable<double> RetargetAccuracy;
	Valueable<bool> RetargetSelf;
	Valueable<double> RetargetSelf_Probability;
	Nullable<bool> AroundTarget;
	Nullable<bool> Airburst_UseCluster;
	Valueable<bool> Airburst_RandomClusters;
	Valueable<bool> Airburst_TargetAsSource;
	Nullable<bool> Airburst_TargetAsSource_SkipHeight;
	Valueable<Leptons> Splits_TargetingDistance;
	Nullable<bool> Splits_TargetingDistance_Cylindrical;
	Nullable<bool> Splits_AllowRepeatTargets;
	Valueable<int> Splits_TargetCellRange;
	Nullable<bool> Splits_UseWeaponTargeting;
	Nullable<bool> AirburstWeapon_ApplyFirepowerMult;
	Valueable<Leptons> AirburstWeapon_SourceScatterMin;
	Valueable<Leptons> AirburstWeapon_SourceScatterMax;
	Nullable<bool> AirburstWeapon_UseFiringEffects;
	Nullable<bool> AirburstWeapon_HeadToTarget;
	Valueable<int> AirburstWeapon_RadialFireSegments;

	Valueable<bool> Parachuted;
	Valueable<int> Parachuted_FallRate;
	Nullable<int> Parachuted_MaxFallRate;
	Nullable<AnimTypeClass*> BombParachute;

	Valueable<bool> AU;

	Valueable<int> ZAdjust;

	// Ares 0.7
	Nullable<Leptons> BallisticScatter_Min;
	Nullable<Leptons> BallisticScatter_Max;

	BulletTypeExt(BulletTypeClass* OwnerObject) : ObjectTypeExt(OwnerObject)
		, Armor {}
		, Interceptable {}
		, Interceptable_DeleteOnIntercept { false }
		, Interceptable_WeaponOverride {}
		, LaserTrail_Types {}
		, Gravity {}
		, Vertical_AircraftFix {}
		, VerticalInitialFacing {}
		, TrajectoryType { }
		, LifeDuration { 0 }
		, NoTargetLifeTime { -1 }
		, CreateCapacity { -1 }
		, RetargetInterval { 1 }
		, RetargetRadius { 0 }
		, RetargetHouses { AffectedHouse::Enemies }
		, Synchronize { false }
		, PeacefulVanish {}
		, ApplyRangeModifiers { false }
		, UseDisperseCoord { false }
		, PassDetonate { false }
		, PassDetonateLocal { false }
		, PassDetonateWarhead {}
		, PassDetonateDamage {}
		, PassDetonateDelay { 1 }
		, PassDetonateInitialDelay { 0 }
		, ProximityImpact { 0 }
		, ProximityWarhead {}
		, ProximityDamage {}
		, ProximityRadius { Leptons(179) }
		, ProximityDirect { false }
		, ProximityMedial { false }
		, ProximityAllies { false }
		, ProximityFlight { false }
		, ProximitySphere { true }
		, ThroughVehicles { true }
		, ThroughBuilding { true }
		, DamageEdgeAttenuation { 1.0 }
		, DamageCountAttenuation { 1.0 }
		, DisperseWeapons {}
		, DisperseBursts {}
		, DisperseCounts {}
		, DisperseDelays {}
		, DisperseCycle { 0 }
		, DisperseInitialDelay { 0 }
		, DisperseEffectiveRange { Leptons(0) }
		, DisperseSeparate { false }
		, DisperseRetarget { false }
		, DisperseLocation { false }
		, DisperseTendency { false }
		, DisperseHolistic { false }
		, DisperseMarginal { false }
		, DisperseDoRepeat { false }
		, DisperseSuicide { true }
		, DisperseFromFirer {}
		, DisperseFaceCheck { false }
		, DisperseForceFire { true }
		, DisperseCoord { CoordStruct::Empty }
		, Shrapnel_AffectsGround {}
		, Shrapnel_AffectsBuildings {}
		, Shrapnel_UseWeaponTargeting {}
		, Shrapnel_IgnoreHitBuildings {}
		, Shrapnel_ObeyWarheadTriggerConditions {}
		, ClusterScatter_Min { Leptons(256) }
		, ClusterScatter_Max { Leptons(512) }
		, BallisticScatter_Min {}
		, BallisticScatter_Max {}
		, SubjectToLand {}
		, SubjectToLand_Detonate { true }
		, SubjectToWater {}
		, SubjectToWater_Detonate { true }
		, AAOnly { false }
		, Arcing_AllowElevationInaccuracy {}
		, ReturnWeapon {}
		, SubjectToSolid { false }
		, ReturnWeapon_ApplyFirepowerMult {}
		, SubjectToGround { false }
		, BallisticScatter_IncreaseByRange { false }
		, BallisticScatter_MinRange {}
		, BallisticScatter_MaxRange {}
		, BallisticScatter_Min_InMinRange {}
		, BallisticScatter_Min_InMaxRange {}
		, BallisticScatter_Max_InMinRange {}
		, BallisticScatter_Max_InMaxRange {}
		, BallisticScatter_Chance { 1.0 }
		, Scatter_Sigma { 1.0 }
		, Scatter_Aspect { 1.0 }
		// Unset on purpose: the fallback is the global [General] Divergence (default 0.5),
		// resolved by GetScatterDivergence() so that changing the global takes effect without
		// touching any projectile.
		, Scatter_Divergence {}
		, Splits { false }
		, AirburstSpread { 1.5 }
		, RetargetAccuracy { 0.0 }
		, RetargetSelf { true }
		, RetargetSelf_Probability { 0.5 }
		, AroundTarget {}
		, Airburst_UseCluster {}
		, Airburst_RandomClusters { false }
		, Airburst_TargetAsSource { false }
		, Airburst_TargetAsSource_SkipHeight {}
		, Splits_TargetingDistance{ Leptons(1280) }
		, Splits_TargetingDistance_Cylindrical {}
		, Splits_AllowRepeatTargets {}
		, Splits_TargetCellRange { 3 }
		, Splits_UseWeaponTargeting {}
		, AirburstWeapon_ApplyFirepowerMult {}
		, AirburstWeapon_SourceScatterMin { Leptons(0) }
		, AirburstWeapon_SourceScatterMax { Leptons(0) }
		, AirburstWeapon_UseFiringEffects {}
		, AirburstWeapon_HeadToTarget {}
		, AirburstWeapon_RadialFireSegments { 0 }
		, Parachuted { false }
		, Parachuted_FallRate { 1 }
		, Parachuted_MaxFallRate {}
		, BombParachute {}
		, AU { false }
		, ZAdjust { 0 }
	{ }

	virtual ~BulletTypeExt() = default;

	virtual void LoadFromINIFile(CCINIClass* pINI) override;
	// virtual void Initialize() override;

	virtual void LoadFromStream(PhobosStreamReader& Stm) override;
	virtual void SaveToStream(PhobosStreamWriter& Stm) override;

private:
	template <typename T>
	void Serialize(T& Stm);

	void TrajectoryValidation() const;

public:
	// Divergence coefficient to use for this projectile: its own 'Divergence' key when it was
	// written, otherwise the global [General] Divergence. Deliberately resolved when used
	// rather than at INI load, so the global value is always the final one and a projectile
	// only has to write the key when it wants to differ from the global.
	double GetScatterDivergence() const;

	class ExtContainer final : public Container<BulletTypeExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	static BulletTypeExt* Fetch(const BulletTypeClass* pThis)
	{
		return AbstractExt::Fetch<BulletTypeExt>(pThis);
	}

	static BulletTypeExt* TryFetch(const BulletTypeClass* pThis)
	{
		return AbstractExt::TryFetch<BulletTypeExt>(pThis);
	}

	static double GetAdjustedGravity(BulletTypeClass* pType);
	static BulletTypeClass* GetDefaultBulletType();
};


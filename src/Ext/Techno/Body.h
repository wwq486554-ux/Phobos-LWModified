#pragma once

#include <Ext/TechnoType/Body.h>
#include <Ext/Radio/Body.h>
#include <Utilities/Container.h>
#include <Utilities/Detach.h>
#include <Utilities/TemplateDef.h>
#include <Utilities/Macro.h>
#include <Ext/Bullet/Body.h>
#include <New/Entity/ShieldClass.h>
#include <New/Entity/LaserTrailClass.h>
#include <New/Entity/AttachEffectClass.h>

class AirstrikeClass;
class BulletClass;

// One sweep in progress. A weapon with SweepFire enabled walks a virtual line
// once per trigger, firing a real shot every ROF frames. Burst=N fires N of
// these concurrently, which is why they live in a container on the techno.
struct SweepFireInstance
{
	int WeaponIndex { -1 }; // Weapon slot that started this sweep
	int BurstIndex { -1 }; // Position in the current Burst, decides the mirroring
	CDTimerClass ShotTimer {}; // Frames left until the next shot of this sweep
	CDTimerClass SweepTimer {}; // Frames left until the aim point reaches the line end
	CoordStruct LineSource { CoordStruct::Empty }; // World-space start of the line
	CoordStruct LineTarget { CoordStruct::Empty }; // World-space end of the line
	// World-space point the virtual offsets are resolved against (normally the target). Kept
	// separately from the two ends so that SweepFire.AttachToTarget can move it and
	// SweepFire.UpdateDirection can turn the line around it.
	CoordStruct AnchorCoord { CoordStruct::Empty };
	double RotateRadian { 0.0 }; // Orientation the line was resolved with
	bool Mirrored { false }; // Whether the virtual offsets were mirrored along Y
	int ShotsFired { 0 }; // Follow-up shots fired so far, for diagnostics
	double LastDistance { 0.0 }; // Distance along the line the last shot was fired at
	DWORD AnchorTargetID { 0 }; // UniqueID of the target the line is attached to, 0 if none

	bool Load(PhobosStreamReader& stm, bool registerForChange);
	bool Save(PhobosStreamWriter& stm) const;

private:
	template <typename T>
	bool Serialize(T& stm);
};

// Diagnostic instrumentation for the SweepFire mechanism. Development only: a game that does not
// set SweepFire.ControlProbe=yes on some weapon writes no SweepFire probe to debug.log at all.
// Even when switched on, every site keeps its own line budget, so a sweep cannot flood the log.
namespace SweepFireDiag
{
	// Compile-time master switch. Turn it off to drop the probes from a build entirely.
	constexpr bool Enabled = true;

	extern int Remaining;
	extern int HookEntryRemaining;

	// True when the probes are switched on: the compile-time master says yes and some weapon
	// opted in. Sites that keep their own line budget use this; the rest use Allowed().
	bool On();
	// On() plus a shared per-session line budget, for the sites that have no budget of their own.
	bool Allowed();
	bool HookEntryAllowed();
	// True the first time a given (hook site, weapon) pair is seen. Used to list every weapon
	// that actually reaches the Fire_At hooks, without spamming.
	bool FirstTime(int site, const char* pWeaponId);
}

class TechnoExt : public RadioExt, public Detach::Listener<AirstrikeClass>
{
public:
	using base_type = TechnoClass;

	// deprecated: the pre-rework nested data class is now the extension class itself
	using ExtData [[deprecated("use the extension class itself instead")]] = TechnoExt;

	static constexpr DWORD Canary = 0x55555555;

public:
	// typed owner accessor
	TechnoClass* OwnerObject() const
	{
		return static_cast<TechnoClass*>(this->GetAttachedObject());
	}

	TechnoTypeExt* TypeExtData;
	std::unique_ptr<ShieldClass> Shield;
	std::vector<std::unique_ptr<LaserTrailClass>> LaserTrails;
	std::vector<std::unique_ptr<AttachEffectClass>> AttachedEffects;
	AttachEffectTechnoProperties AE;
	std::vector<EBolt*> ElectricBolts; // EBolts are not serialized so do not serialize this either.
	int AnimRefCount; // Used to keep track of how many times this techno is referenced in anims f.ex Invoker, ParentBuilding etc., for pointer invalidation.
	CDTimerClass PassengerDeletionTimer;
	ShieldTypeClass* CurrentShieldType;
	CDTimerClass ChargeTurretTimer; // Used for charge turrets instead of RearmTimer if weapon has ChargeTurret.Delays set.
	CDTimerClass AutoDeathTimer;
	AnimTypeClass* MindControlRingAnimType;
	int DamageNumberOffset;
	bool HasBeenPlacedOnMap; // Set to true on first Unlimbo() call.
	bool ForceFullRearmDelay;
	bool LastRearmWasFullDelay;
	bool CanCloakDuringRearm; // Current rearm timer was started by DecloakToFire=no weapon.
	int WHAnimRemainingCreationInterval;
	WeaponTypeClass* LastWeaponType;
	CoordStruct LastWeaponFLH;
	std::shared_ptr<PhobosMap<BulletTypeClass*, BulletGroupData>> TrajectoryGroup;
	CellClass* FiringObstacleCell; // Set on firing if there is an obstacle cell between target and techno, used for updating WaveClass target etc.
	bool IsDetachingForCloak; // Used for checking animation detaching, set to true before calling Detach_All() on techno when this anim is attached to and to false after when cloaking only.
	int BeControlledThreatFrame;
	DWORD LastTargetID;
	int AccumulatedGattlingValue;
	bool ShouldUpdateGattlingValue;
	int AttachedEffectInvokerCount;

	bool DelayedFireSequencePaused;
	int DelayedFireWeaponIndex;
	CDTimerClass DelayedFireTimer;
	AnimClass* CurrentDelayedFireAnim;

	AirstrikeClass* AirstrikeTargetingMe;

	bool IsSelected;

	// --- SpecialAction ---
	// Cooldown of this unit's own special ability, independent of weapon,
	// super weapon or deploy timers.
	CDTimerClass SpecialActionTimer;
	// Frame of the last successful SpecialAction use; -1 means never used.
	int LastSpecialActionFrame;
	// Weapon slot the engine is asked to use while a "Weapon" SpecialAction is
	// firing. -1 means "let the engine choose". Consumed by the force-weapon
	// hook, so no engine function pointer is ever rewritten.
	int SpecialActionWeaponIndex;
	// How many shots of the forced burst still have to be fired. The slot is
	// dropped once this reaches zero so the unit returns to normal behaviour.
	int SpecialActionBurstShotsLeft;
	// Set while the ability's own "Unload" request is the thing driving
	// Mission::Unload. The engine routes both the deploy command and a passenger
	// unload through that single mission, so the unit type's deploy keys
	// (Deploy.SkipPassengerUnload / Deploy.NoPassenger) cannot tell the two apart.
	// This marker lets the 0x73D63B hook keep the ability's request a passenger
	// release while the deploy command keeps whatever the type asks for.
	// Deliberately NOT serialized: a mission does not survive a save/load, and
	// leaving it out keeps the savegame layout untouched.
	bool SpecialActionUnloading;

	// The weapon that most recently delivered a Temporal=yes warhead from this unit,
	// recorded when its bullet detonates. The engine derives a Temporal warp's erase
	// rate from the techno's "current" weapon selection rather than from the weapon
	// that actually hit, and those are not the same thing: a Temporal warhead fired
	// from a slot the engine never selects - for example through the Weapon ability's
	// forced slot, which is released as soon as the burst ends - would otherwise erase
	// at the speed of an unrelated weapon. Consumed by Phobos' own replacement for
	// TemporalClass::GetWarpPerStep.
	// Deliberately NOT serialized: it is only meaningful while a warp is running, and
	// leaving it out keeps the savegame layout untouched.
	WeaponTypeClass* TemporalWarpWeapon;

	// cache tint values
	int TintColorOwner;
	int TintColorAllies;
	int TintColorEnemies;
	int TintIntensityOwner;
	int TintIntensityAllies;
	int TintIntensityEnemies;

	bool SpecialTracked;
	bool FallingDownTracked;

	bool OnParachuted; // This is just a temporary patch. TODO: fully check HasParachuted and correct its maintenance method.
	bool HoverShutdown;
	CoordStruct LastTargetCrd;
	CDTimerClass LastTargetCrdClearTimer;

	bool ShouldBeDead;

	int DropCrate; // Drop crate on death, modified by map action
	Powerup DropCrateType;

	bool PreventCrewEscape;

	// --- SweepFire ---
	// Sweeps currently in progress. An empty vector means this techno is not
	// sweeping, which is the check the per-frame update short-circuits on.
	std::vector<SweepFireInstance> Sweeps;
	// Frames between the end of a sweep and the start of the next one.
	CDTimerClass SweepFireCooldownTimer;

	TechnoExt(TechnoClass* OwnerObject) : RadioExt(OwnerObject)
		, TypeExtData { nullptr }
		, Shield {}
		, LaserTrails {}
		, AttachedEffects {}
		, AE {}
		, ElectricBolts {}
		, AnimRefCount { 0 }
		, PassengerDeletionTimer {}
		, CurrentShieldType { nullptr }
		, ChargeTurretTimer {}
		, AutoDeathTimer {}
		, MindControlRingAnimType { nullptr }
		, DamageNumberOffset { INT32_MIN }
		, HasBeenPlacedOnMap { false }
		, ForceFullRearmDelay { false }
		, LastRearmWasFullDelay { false }
		, CanCloakDuringRearm { false }
		, WHAnimRemainingCreationInterval { 0 }
		, LastWeaponType {}
		, LastWeaponFLH {}
		, TrajectoryGroup {}
		, FiringObstacleCell {}
		, IsDetachingForCloak { false }
		, BeControlledThreatFrame { 0 }
		, LastTargetID { 0xFFFFFFFF }
		, AccumulatedGattlingValue { 0 }
		, ShouldUpdateGattlingValue { false }
		, AirstrikeTargetingMe { nullptr }
		, DelayedFireSequencePaused { false }
		, DelayedFireWeaponIndex { -1 }
		, DelayedFireTimer {}
		, CurrentDelayedFireAnim { nullptr }
		, AttachedEffectInvokerCount { 0 }
		, IsSelected { false }
		, SpecialActionTimer {}
		, LastSpecialActionFrame { -1 }
		, SpecialActionWeaponIndex { -1 }
		, SpecialActionBurstShotsLeft { 0 }
		, SpecialActionUnloading { false }
		, TemporalWarpWeapon { nullptr }
		, TintColorOwner { 0 }
		, TintColorAllies { 0 }
		, TintColorEnemies { 0 }
		, TintIntensityOwner { 0 }
		, TintIntensityAllies { 0 }
		, TintIntensityEnemies { 0 }
		, SpecialTracked { false }
		, FallingDownTracked { false }
		, OnParachuted { false }
		, HoverShutdown { false }
		, LastTargetCrd { CoordStruct::Empty }
		, LastTargetCrdClearTimer {}
		, ShouldBeDead { false }
		, DropCrate { -1 }
		, DropCrateType { Powerup::Money }
		, PreventCrewEscape { false }
		, Sweeps {}
		, SweepFireCooldownTimer {}
	{ }

	void OnEarlyUpdate();

	// the extension state that goes with TechnoClass::Init
	void InitializeState(TechnoTypeClass* pType = nullptr);

	// the techno was created while a savegame was loading, so TechnoClass::Init found
	// no extension to initialize; catch up now that there is one
	virtual void OnDeferredAllocation() override { this->InitializeState(); }

	// True while the object is hidden underground (subterranean units); false for
	// everything else. Overridden by UnitExt, which owns the burrow state.
	virtual bool IsBurrowedState() const { return false; }

	// True while the object is inside a tunnel (foot units); false for everything
	// else. Overridden by FootExt, which owns the tunnel state.
	virtual bool IsInTunnelState() const { return false; }

	void ApplyInterceptor();
	bool CheckDeathConditions(bool isInLimbo = false);
	void EatPassengers();
	void UpdateShield();
	void ApplySpawnLimitRange();
	void UpdateLaserTrails();
	void UpdateAttachEffects();
	void UpdateGattlingRateDownReset();
	void UpdateCumulativeAttachEffects(AttachEffectTypeClass* pAttachEffectType, bool createAnim = false);
	bool RecalculateStatMultipliers(AttachEffectClass* pAttachEffect = nullptr);
	void UpdateTemporal();
	void UpdateMindControlAnim();
	void UpdateRecountBurst();
	void UpdateRearmInEMPState();
	void UpdateRearmInTemporal();

	// --- SweepFire ---
	// Starts a sweep for a shot that is about to be fired. Returns the cell the
	// engine must aim that shot at, or nullptr when no sweep was started and the
	// shot should behave as a normal single shot.
	CellClass* TryStartSweepFire(WeaponTypeClass* pWeapon, int weaponIndex, AbstractClass* pTarget, int burstIndex, CoordStruct* pLineStartOut = nullptr);
	// Advances every sweep of this techno by one frame, firing follow-up shots.
	void UpdateSweepFire();
	// Re-resolves a sweep's world-space segment from its virtual offsets. Runs once per frame
	// before the follow-up shots and does nothing unless SweepFire.AttachToTarget (move the
	// anchor with the live target) or SweepFire.UpdateDirection (turn the line with the firer)
	// asked for it. The line's length is invariant, so the sweep's schedule stays valid.
	void RefreshSweepLine(SweepFireInstance& sweep, WeaponTypeClass* pWeapon);
	// Fires one follow-up shot of a sweep at the current aim point. Returns false
	// on allocation failure, which stops the sweep safely. A non-negative
	// distanceOverride aims the shot that far along the line instead of at the
	// point the sweep timer has reached (used for SweepFire.ShotAtEnd).
	bool FireSweepShot(SweepFireInstance& sweep, WeaponTypeClass* pWeapon, double distanceOverride = -1.0);
	// Finds the sweep belonging to a Burst slot, or nullptr.
	SweepFireInstance* FindSweep(int burstIndex);
	// Rearm time a sweep needs (its duration plus the cooldown), or -1 when the
	// shot that just fired does not finish a sweep. Consumed by the rearm hook so
	// the engine accounts for a whole sweep as a single long shot.
	int GetSweepFireRearmTime(WeaponTypeClass* pWeapon);
	// Drops every running sweep. releaseRearm also cancels the synthetic reload
	// the sweep had claimed and clears the cooldown, so the techno is free to act
	// again on the very next frame.
	void AbortSweepFire(bool releaseRearm);
	// Keeps the infantry firing sequence looping for as long as a sweep runs
	// (SweepFire.InfantryFireAnim). No effect on other technos.
	void UpdateInfantrySweepAnim(int weaponIndex);

	void InitializeLaserTrails();
	void InitializeAttachEffects();
	void UpdateSelfOwnedAttachEffects();
	bool HasAttachedEffects(std::vector<AttachEffectTypeClass*> attachEffectTypes, bool requireAll, bool ignoreSameSource, TechnoClass* pInvoker, AbstractClass* pSource, std::vector<int> const* minCounts, std::vector<int> const* maxCounts) const;
	int GetAttachedEffectCumulativeCount(AttachEffectTypeClass* pAttachEffectType, bool ignoreSameSource = false, TechnoClass* pInvoker = nullptr, AbstractClass* pSource = nullptr) const;
	void InitializeDisplayInfo();
	void ApplyMindControlRangeLimit();
	int ApplyForceWeaponInRange(AbstractClass* pTarget);
	void ResetDelayedFireTimer();
	void UpdateTintValues();
	void UpdateLastTargetCrd();
	int GetSight();

	static bool CanReceiveEvent(TechnoClass* pThis, HouseClass* pHouse);

	virtual ~TechnoExt() override;
	virtual void OnDetach(AirstrikeClass* pTarget, bool removed) override;
	virtual void LoadFromStream(PhobosStreamReader& Stm) override;
	virtual void SaveToStream(PhobosStreamWriter& Stm) override;

private:
	template <typename T>
	void Serialize(T& Stm);

public:
	// TechnoExt is never instantiated and has no container of its own: instances are
	// concrete leaves (UnitExt/InfantryExt/AircraftExt/BuildingExt) tracked by their
	// own containers. The polymorphic fetch reads the inline slot directly.
	static TechnoExt* Fetch(const TechnoClass* pThis)
	{
		return AbstractExt::Fetch<TechnoExt>(pThis);
	}

	static TechnoExt* TryFetch(const TechnoClass* pThis)
	{
		return AbstractExt::TryFetch<TechnoExt>(pThis);
	}

	// deprecated stand-in for the pre-rework container of all TechnoClass extensions
	static inline CompatExtMap<TechnoExt, TechnoClass> ExtMap {};

	static bool LoadGlobals(PhobosStreamReader& Stm);
	static bool SaveGlobals(PhobosStreamWriter& Stm);

	static bool IsActive(TechnoClass* pThis);

	static bool IsHarvesting(TechnoClass* pThis);
	static bool HasAvailableDock(TechnoClass* pThis);
	static bool HasRadioLinkWithDock(TechnoClass* pThis);


	static Matrix3D GetTransform(TechnoClass* pThis, VoxelIndexKey* pKey = nullptr, bool isShadow = false);
	static Matrix3D GetFLHMatrix(TechnoClass* pThis, const CoordStruct& flh, bool isOnTurret, double factor = 1.0, bool isShadow = false, int turIdx = -1);
	static Matrix3D TransformFLHForTurret(TechnoClass* pThis, Matrix3D mtx, bool isOnTurret, double factor = 1.0, int turIdx = -1);
	static CoordStruct GetFLHAbsoluteCoords(TechnoClass* pThis, const CoordStruct& flh, bool isOnTurret = false, int turIdx = -1);

	// burstIndex < 0 means "use the techno's current Burst index".
	static CoordStruct GetBurstFLH(TechnoClass* pThis, int weaponIndex, bool& FLHFound, int burstIndex = -1);
	static void ChangeOwnerMissionFix(FootClass* pThis);
	static void KillSelf(TechnoClass* pThis, AutoDeathBehavior deathOption, const std::vector<AnimTypeClass*>& pVanishAnimation, bool isInLimbo = false);
	static void ObjectKilledBy(TechnoClass* pThis, TechnoClass* pKiller);
	static void UpdateSharedAmmo(TechnoClass* pThis);
	static bool HasAdditionalAbility(TechnoClass* pThis, AdditionalAbility ability);
	static double GetCurrentSpeedMultiplier(FootClass* pThis);
	static double GetCurrentFirepowerMultiplier(TechnoClass* pThis);
	static double GetCurrentArmorMultiplier(TechnoClass* pThis, TechnoTypeClass* pType, HouseClass* pSourceHouse = nullptr, WarheadTypeClass* pWarhead = nullptr);
	static double CalculateArmorMultipliers(TechnoClass* pThis, WarheadTypeClass* pWarhead, HouseClass* pSourceHouse, bool hitAnim = false);
	static void DrawSelfHealPips(TechnoClass* pThis, Point2D* pLocation, RectangleStruct* pBounds);
	// Draws the SpecialAction cooldown strip. Independent of PipScale on purpose -
	// see RulesExt::Pips_SpecialAction_* for why.
	static void DrawSpecialActionPips(TechnoClass* pThis, Point2D* pLocation, RectangleStruct* pBounds);
	static void DrawInsignia(TechnoClass* pThis, Point2D* pLocation, RectangleStruct* pBounds);
	static void ApplyGainedSelfHeal(TechnoClass* pThis);
	static void SyncInvulnerability(TechnoClass* pFrom, TechnoClass* pTo);
	static CoordStruct PassengerKickOutLocation(TechnoClass* pThis, FootClass* pPassenger, int maxAttempts);
	static bool AllowedTargetByZone(TechnoClass* pThis, TechnoClass* pTarget, TargetZoneScanType zoneScanType, WeaponTypeClass* pWeapon = nullptr, bool useZone = false, int zone = -1);
	static void UpdateAttachedAnimLayers(TechnoClass* pThis);
	static bool ConvertToType(FootClass* pThis, TechnoTypeClass* toType);
	static bool IsTypeImmune(TechnoClass* pThis, TechnoClass* pSource);
	static int GetTintColor(TechnoClass* pThis, bool invulnerability, bool airstrike, bool berserk);
	static int GetCustomTintColor(TechnoClass* pThis);
	static int GetCustomTintIntensity(TechnoClass* pThis);
	static void ApplyCustomTintValues(TechnoClass* pThis, int& color, int& intensity);
	static Point2D GetScreenLocation(TechnoClass* pThis);
	static Point2D GetFootSelectBracketPosition(TechnoClass* pThis, Anchor anchor);
	static Point2D GetBuildingSelectBracketPosition(TechnoClass* pThis, BuildingSelectBracketPosition bracketPosition);
	static void DrawSelectBox(TechnoClass* pThis, const Point2D* pLocation, const RectangleStruct* pBounds, bool drawBefore = false);
	static void ProcessDigitalDisplays(TechnoClass* pThis);
	static int GetDropCrateIndex(TechnoClass* pThis);
	static void GetValuesForDisplay(TechnoClass* pThis, TechnoTypeClass* pType, DisplayInfoType infoType, int& value, int& maxValue, int infoIndex);
	static void GetDigitalDisplayFakeHealth(TechnoClass* pThis, int& value, int& maxValue);
	static void CreateDelayedFireAnim(TechnoClass* pThis, AnimTypeClass* pAnimType, int weaponIndex, bool attach, bool center, bool removeOnNoDelay, bool onTurret, CoordStruct firingCoords);
	static bool HandleDelayedFireWithPauseSequence(TechnoClass* pThis, WeaponTypeClass* pWeapon, int weaponIndex, int frame, int firingFrame);
	static bool IsHealthInThreshold(TechnoClass* pObject, double min, double max);
	static void ShowPromoteAnim(TechnoClass* pThis);
	static void ClickedApproachObject(FootClass* pThis, ObjectClass* pObject);
	static bool CanBeRecruitedFix(FootClass* pThis, HouseClass* pHouse);

	static bool EjectRandomly(FootClass* pEjectee, const CoordStruct& coords, int distance, bool select);
	static bool EjectSurvivor(FootClass* pSurvivor, CoordStruct coords, bool select);
	static bool __fastcall ApplyKillDriver(TechnoClass** pData, void*, HouseClass* pToHouse, TechnoClass* pKiller, bool resetVeterancy);

	// WeaponHelpers.cpp
	static int PickWeaponIndex(TechnoClass* pThis, TechnoClass* pTargetTechno, AbstractClass* pTarget, int weaponIndexOne, int weaponIndexTwo, bool allowFallback = true, bool allowAAFallback = true);
	static void FireWeaponAtSelf(TechnoClass* pThis, WeaponTypeClass* pWeaponType);
	static bool CanFireNoAmmoWeapon(TechnoClass* pThis, int weaponIndex);
	static bool CanFireNoAmmoWeapon(TechnoClass* pThis, TechnoTypeClass* pType, int weaponIndex);
	static WeaponTypeClass* GetDeployFireWeapon(TechnoClass* pThis, TechnoTypeClass* pType, int& weaponIndex);
	static WeaponTypeClass* GetDeployFireWeapon(TechnoClass* pThis, TechnoTypeClass* pType);
	static WeaponTypeClass* GetCurrentWeapon(TechnoClass* pThis, TechnoTypeClass* pType, int& weaponIndex, bool getSecondary = false);
	static WeaponTypeClass* GetCurrentWeapon(TechnoClass* pThis, TechnoTypeClass* pType, bool getSecondary = false);
	static int GetWeaponIndexAgainstWall(TechnoClass* pThis, OverlayTypeClass* pWallOverlayType);
	static void ApplyKillWeapon(TechnoClass* pThis, TechnoClass* pSource, WarheadTypeClass* pWH);
	static void ApplyRevengeWeapon(TechnoClass* pThis, TechnoClass* pSource, WarheadTypeClass* pWH);
	static bool TryToCreateCrate(CoordStruct location, Powerup selectedPowerup = Powerup::Money, int maxCellRange = 10);
	static bool MultiWeaponCanFire(TechnoClass* const pThis, AbstractClass* const pTarget, WeaponTypeClass* const pWeaponType);
	static bool HasWeaponsDisabled(TechnoClass* pThis);
	static FireError GetFireErrorIgnoreDisableWeapons(TechnoClass* pThis, AbstractClass* pTarget, int weaponIndex, bool ignoreRange);
};


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
class TemporalClass;
class WarheadTypeClass;

// The two flags the fixed-duration Temporal freeze uses to talk to the ReceiveDamage hook
// in Hooks.ReceiveDamage.cpp. The damage a freeze deals is dealt synchronously, so plain
// flags are enough - there is no re-entrancy.
namespace TemporalTemp
{
	// Set while the freeze pays out its own damage. It makes ReceiveDamage clamp a lethal
	// hit to 1 HP instead of letting the engine run the normal death handling, so that the
	// target is removed through the engine's warp erase rather than a wreck.
	extern bool PayingFreezeDamage;
	// Set by that clamp when it fired, i.e. the payment would have killed the target.
	extern bool FreezeDamageWasLethal;
	// Set while a warp is handed from one instance to another without ending it: a CellSpread
	// Temporal shot moves its caster's own slot onto one of its other victims when the instance
	// holding the slot loses its victim. That is not the end of the freeze - the victim is
	// neither settled nor started over - so the warp's record has to survive the move untouched.
	extern bool MovingWarp;
	// The frame a warp was removed on *by its own schedule* (TemporalClass::Update's erase path,
	// entered from the AI hook), and which target that was. The warp is then let go of by the
	// engine's erase tail, and that LetGo must not be mistaken for the caster breaking the freeze
	// off: only a warp that was broken off releases the rest of its CellSpread shot. A frame stamp
	// rather than a flag, so it can never be stale; the target is carried with it because one
	// frame can see both an extra's own erase and a let-go of the caster's slot.
	extern int ScheduledEraseFrame;
	extern DWORD ScheduledEraseVictim;
}
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
	// Frames this sweep waits before the next trigger may start another one, already scaled by
	// the weapon's ROF curve at the moment the sweep began. Kept on the sweep so the engine's
	// rearm timer and the internal cooldown that gates the next trigger can never disagree -
	// including across a save taken while the sweep is still running.
	int Cooldown { 0 };

	bool Load(PhobosStreamReader& stm, bool registerForChange);
	bool Save(PhobosStreamWriter& stm) const;

private:
	template <typename T>
	bool Serialize(T& stm);
};

// What the per-frame step of a fixed-duration freeze wants the engine to do this frame.
enum class TemporalFreezeFrame
{
	NotHandled = 0, // this techno has no freeze for that Temporal; keep the vanilla decision
	KeepAlive,      // a freeze is running: hold the warp open, do not erase yet
	Erase           // the damage finished the target off: erase it the vanilla warp way
};

// One "freeze the target for Temporal.Duration frames" attack. It lives on the *caster*
// (which is what TemporalImUsing belongs to) and finds its victim through TargetID, the
// same way SweepFireInstance does - a raw pointer would not survive a save/load.
struct TemporalFreezeInstance
{
	bool Active { false };
	DWORD TargetID { 0 }; // UniqueID of the unit being held
	int Duration { 0 }; // Frames the freeze lasts, from Temporal.Duration (0: no damage window)
	int Interval { 0 }; // Frames the warp is held after that, from Temporal.Interval
	int EraseAt { 0 }; // CellSpread.TemporalEraseMode=pooled: this many frames of freeze (0: off)
	// The absolute frame the pooled erase lands on, so that every victim of the shot leaves on
	// one frame no matter when its own update got round to advancing the freeze, and so that the
	// caster's reload can be held to that exact frame (HoldTemporalRearm). 0 on a record that
	// predates the field; the per-victim counter above is then used instead.
	int EraseFrame { 0 };
	int TickRof { 0 }; // Frames between two payments (weapon ROF, AE.ROFMultiplier applied)
	int Total { 0 }; // Damage owed for a full Duration, warhead modifiers included
	int Paid { 0 }; // Damage already dealt
	int Elapsed { 0 }; // Frames the target has been frozen for
	CDTimerClass TickTimer {}; // Frames left until the next payment
	TemporalDamageMode Mode { TemporalDamageMode::Continuous };
	// Not serialized: after a load the warhead is resolved again from the caster's
	// Temporal warp the first time the freeze is advanced, and the record is dropped if
	// that fails (the warp may have ended while the game was being saved).
	WarheadTypeClass* Warhead { nullptr };

	bool Load(PhobosStreamReader& stm, bool registerForChange);
	bool Save(PhobosStreamWriter& stm) const;

private:
	template <typename T>
	bool Serialize(T& stm);
};

// Per-mount state of one ExtraWeapon list. Each mount keeps its own cooldown and burst
// state, so the mounts fire on their own schedules; a paused mount (no target, out of its
// own range, unit-level gate, different group active) keeps that state untouched, which is
// the vanilla burst behaviour: the round in progress is resumed, not restarted.
struct ExtraWeaponRuntime
{
	WeaponTypeClass* Weapon { nullptr }; // Weapon this mount currently holds, for change detection
	CDTimerClass Cooldown {};            // Frames until the next round may start
	CDTimerClass BurstTimer {};          // Frames until the next shot of the current burst
	int BurstShotsLeft { 0 };            // Shots still owed by the current burst
	int NextShotIndex { 0 };             // Burst index of the next shot (delay/mirroring)
	// Transient diagnostic flag, not serialized: an unresolvable weapon ID leaves an empty
	// shell behind and the engine-special weapon kinds are skipped, and this keeps the log
	// warning to one per mount instead of every frame.
	bool SkippedLogged { false };
	bool Load(PhobosStreamReader& Stm, bool RegisterForChange);
	bool Save(PhobosStreamWriter& Stm) const;

private:
	template <typename T>
	bool Serialize(T& Stm);
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

	// --- ROFCurve ---
	// Transient spin-up state of a ROFCurve weapon. One canister's worth of state: how far
	// along the curve the current burst of fire has travelled, which weapon it belongs to,
	// and what the previous shot was handed (its frame, and the effective rearm delay it
	// produced). Together those two tell a continuing burst apart from a paused one.
	//
	// Deliberately NOT serialized. It describes a burst of fire in progress, so a save/load
	// starts the curve over, which is what it looks like anyway. Leaving it out also keeps
	// the savegame layout of previous builds intact, and both machines of a multiplayer
	// session zero it from the same save, so it stays in sync.
	WeaponTypeClass* ROFCurveWeapon;
	double ROFCurveProgress;
	int ROFCurveLastRearm;
	int ROFCurveLastFrame;

	// --- TankMakeupKit ---
	// The look a Mirage-like vehicle took over from a MakesDisguise weapon hit. Vanilla keeps
	// it only in TechnoClass::Disguise, which the mirage disguise cycle wipes on damage,
	// movement or EMP and then refills from [General] DefaultMirageDisguises. Remembering the
	// picked type here lets the re-disguise step put that look back instead of a random tree.
	//
	// Deliberately NOT serialized: a save/load drops the record, which sends the unit back to
	// vanilla behaviour until it is hit by the weapon again, and it keeps the savegame layout
	// of previous builds intact (same reasoning as the ROFCurve state above).
	ObjectTypeClass* MakeupDisguise;
	HouseClass* MakeupHouse;

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

	// --- ExtraWeapon ---
	// Runtime state, one record per mount. The suffix-less list has a single outer slot
	// (index 0), the grouped lists are indexed by engine weapon slot. Only the list that
	// the engine's current weapon selection activates advances; the others keep their
	// state so a group switch pauses a burst without cancelling it.
	std::vector<std::vector<ExtraWeaponRuntime>> ExtraWeapon_FlatRuntimes;
	std::vector<std::vector<ExtraWeaponRuntime>> ExtraWeapon_GroupRuntimes;

	// --- Temporal.Duration ---
	// The "one-shot freezes" this techno is currently applying, one record per victim: a
	// CellSpread Temporal warhead holds every unit in its radius at once, and each of those
	// warps has its own duration and its own damage to pay. Keyed by TargetID.
	std::vector<TemporalFreezeInstance> TemporalFreezes;

	// --- CellSpread.TemporalEraseMode=pooled (written on the *caster*) ---
	// The frame and length of the pooled erase the shot being delivered right now is scheduled
	// for, and which victims it covers: the whole radius leaves together on frame
	// TemporalAreaFrame + TemporalAreaEraseAt. Set by the delivery (Ext/Bullet/Hooks.DetonateLogics.cpp)
	// just before it starts the warps and read back by BeginTemporalFreeze for each of them, so it
	// is good for the frame it was written on and for those victims only - a warp that has nothing
	// to do with the shot picks up nothing. Never serialized.
	int TemporalAreaEraseAt;
	int TemporalAreaFrame;
	std::vector<DWORD> TemporalAreaVictims;

	// --- the reload the freeze holds back (written on the *caster*) ---
	// The frame the engine's own reload expires on, as it stood when the last area shot was
	// delivered: the freeze hold only ever lengthens the timer, so this is what has to come back
	// when the freeze is broken off (SyncTemporalRearm). Never serialized; a save taken mid-freeze
	// simply resumes without it and the trim then falls back to the interval alone.
	int TemporalRearmBaseFrame;
	// The frame Temporal.Interval owes, kept here so that a trim cannot eat it.
	int TemporalRearmIntervalFrame;

	// --- Temporal.ZeroHealthAction / KilledByOthers (written on the *victim*) ---
	// The victim's health was used up while a Linger freeze held it: it sits at 1 HP and has
	// to be erased when that freeze ends instead of being released.
	bool TemporalLingerSaved;
	// An outsider's lethal hit on this frozen victim has to resolve as a warp erase rather than
	// as a death. The warp that holds the victim picks this up on its next update - the victim
	// itself drives that update every frame - and removes it through the engine's own erase.
	bool TemporalErasePending;

	// Registers the freeze a Temporal detonation just started. Called from the tail of
	// TemporalClass::Fire; does nothing unless the warhead sets Temporal.Duration or the shot is
	// a pooled CellSpread one.
	void BeginTemporalFreeze(TemporalClass* pTemporal, TechnoClass* pTarget);
	// The record write on its own, without a Temporal instance to read the target off. The
	// delivery calls it directly for a victim this techno is *already* warping, because a shot
	// that covers such a victim again must re-time it rather than warp it a second time: a second
	// warp is chained onto the first as a helper, and the holder's per-frame erase step then grows
	// by the helper's (0x71AB10), draining that victim twice as fast as the rest.
	void BeginTemporalFreezeFor(TechnoClass* pTarget);
	// Ends this techno's freeze on pTemporal's target (the warp let go, or the duration ran
	// out): pays the damage still owed for the time the target was actually frozen and
	// clears the record. Returns whether there *was* a record to settle - the callers use that
	// to tell a warp broken off from one that ended on its own schedule.
	bool EndTemporalFreeze(TemporalClass* pTemporal);
	// Advances the freeze by one frame. Returns whether the engine should still erase the
	// target, keep it warped, or whether this techno has nothing to say about it.
	TemporalFreezeFrame UpdateTemporalFreeze(TemporalClass* pTemporal, TechnoClass* pTarget);
	// Drops this techno's freeze without paying anything, because something else is ending it -
	// the target is being erased by Temporal.KilledByOthers, for one. The cooldown still starts.
	void AbandonTemporalFreeze(TechnoClass* pTarget);
	// The weapon whose Temporal warhead is holding this techno's current warp: the one that
	// actually delivered it when that is known, otherwise the engine's own selection.
	static WeaponTypeClass* GetTemporalWarpWeapon(TechnoClass* pOwner);
	// Releases every TemporalClass this techno owns apart from TemporalImUsing. A CellSpread
	// Temporal freeze is delivered by one instance per victim, and the engine only ever lets go
	// of TemporalImUsing - the rest have to be released by hand when the caster stops being able
	// to hold them (it is destroyed, or converted into a techno without a Temporal weapon).
	// includeIdle also drops the instances that hold nobody at the moment. They must be left
	// alone when this runs from inside a TemporalClass::Fire that is still setting one up (see
	// ReleaseExtraTemporalInstances), but a caster that is going away has to take them along:
	// an idle instance still points at its Owner, and TemporalClass::Update dereferences that as
	// soon as it is called on one.
	static void ReleaseExtraTemporalInstances(TechnoClass* pOwner, bool includeIdle = false);

	// Pushes this techno's reload (TechnoClass RearmTimer, +0x2EC - the timer the engine's own
	// firing paths consult, and the one Temporal.Interval and SweepFire already use) out to at
	// least this many frames. A CellSpread Temporal shot calls it with the length of its own
	// freeze, so the caster physically cannot fire again until the units that shot covered are
	// gone. Only ever extended; a longer vanilla reload is left alone.
	static void ExtendTemporalRearm(TechnoClass* pOwner, int frames);

	// Keeps this techno's reload pushed out to the frame its last pending pooled freeze leaves on.
	// Called every frame while such a freeze runs (from TemporalClass::Update, where every victim
	// of the shot passes through): the timer has other writers - the engine restarts it on every
	// shot and this fork's own features scale or stop it - and a shot that escapes while the
	// previous one's victims are still held cannot schedule them at all, so they then fall back to
	// the engine's per-victim erase counter and leave one by one.
	// Only ever extends, and stops by itself once the frame is past.
	void HoldTemporalRearm();

	// Recomputes this techno's reload from everything it still owes and writes it, force included -
	// unlike ExtendTemporalRearm this one may also move the timer *down*:
	//
	//   * the freeze hold of its pending pooled records (the only part that can be given up),
	//   * Temporal.Interval, as pushed when a freeze ended,
	//   * the reload the engine itself started when the shot was fired, captured at delivery
	//     (TemporalRearmBaseFrame), so a trim can never cut into the weapon's own ROF.
	//
	// A caster whose freeze is broken off mid-way owes none of the freeze hold any more, and
	// without this the timer would sit at the remaining erase time and lock the weapon out for it
	// (the reported "after stopping the attack it still waits out the erase time plus ROF").
	void SyncTemporalRearm();

	// The health a Temporal warp's length is measured against: the target's type Strength, which
	// is what vanilla uses, or its current health when the warhead sets Temporal.UseCurrentHealth.
	static int GetFreezeHealth(TechnoClass* pVictim, WarheadTypeClass* pWarhead);
	// How many frames vanilla itself would take to erase this victim with this caster's warp -
	// the health above times ten, at the warp's per-frame step. 0 when it cannot be worked out.
	static int GetTemporalEraseFrames(TechnoClass* pOwner, TechnoClass* pVictim, WarheadTypeClass* pWarhead);

	// Whether this techno holds a pooled record for that victim whose frame has come, i.e. the
	// victim's own update is about to erase it. The caller uses it to keep such a victim where it
	// is: moving it onto the caster's slot first would skip that update (it lands a frame late),
	// and if the slot refuses the unit it is left loose instead of being erased.
	bool IsPooledEraseDue(TechnoClass* pVictim);

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
		, TemporalFreezes {}
		, TemporalAreaEraseAt { 0 }
		, TemporalAreaFrame { 0 }
		, TemporalAreaVictims {}
		, TemporalRearmBaseFrame { 0 }
		, TemporalRearmIntervalFrame { 0 }
		, TemporalLingerSaved { false }
		, TemporalErasePending { false }
		, ROFCurveWeapon { nullptr }
		, ROFCurveProgress { 0.0 }
		, ROFCurveLastRearm { 0 }
		, ROFCurveLastFrame { 0 }
		, MakeupDisguise { nullptr }
		, MakeupHouse { nullptr }
		, ExtraWeapon_FlatRuntimes {}
		, ExtraWeapon_GroupRuntimes {}
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

	// --- ExtraWeapon ---
	// Advances every active mount of this techno's extra weapons by one frame.
	void UpdateExtraWeapons();
	// Resizes/reinitialises the mount states to match the currently active list. Only a
	// weapon change on a mount resets it - a paused list keeps its state.
	void SyncExtraWeaponRuntimes(const std::vector<WeaponTypeClass*>& mounts, std::vector<ExtraWeaponRuntime>& runtimes);
	// Unit-level gates shared by all extra weapons (limbo, EMP/deactivation, disabled
	// weapons, open-topped transport passengers, selling/constructing buildings).
	bool CanExtraWeaponsFire() const;
	// Range test with the extra weapon's own effective range; the engine weapon's range
	// only decides which target the engine acquires.
	bool IsInExtraWeaponRange(WeaponTypeClass* pWeapon, AbstractClass* pTarget) const;
	// Effective rearm delay of an extra weapon, including veteran/house/AE ROF modifiers.
	int GetExtraWeaponROF(WeaponTypeClass* pWeapon) const;
	// Builds and launches one real bullet of the extra weapon from the given FLH.
	bool FireExtraWeaponShot(WeaponTypeClass* pWeapon, AbstractClass* pTarget, const CoordStruct& flh, int burstIndex, int burstCount);

	// --- ROF curve, shared primitives ---
	// The curve itself, and the hook that applies it to an ordinary weapon's reload, live in
	// Hooks.Firing.cpp. These two primitives are here as well because the features that own a
	// different pace - SweepFire's cooldown between two sweeps, and the interval between two
	// Temporal freezes - are paced from this file and read the same curve.
	//
	// Advances the spin-up state and returns the curve's ROF as a ratio to the weapon's own ROF
	// (1.0 when there is no usable curve). Calling it twice on one frame is harmless: the second
	// call sees a zero gap and changes nothing.
	double StepROFCurve(WeaponTypeClass* pWeapon);
	// Scales a frame count by the curve that this weapon has already put the techno on. A frame
	// count that is not a delay (<= 0, i.e. the feature is off) and a weapon without a running
	// chain are returned untouched.
	int ScaleByROFCurve(int frames, WeaponTypeClass* pWeapon) const;

	void InitializeLaserTrails();
	void InitializeAttachEffects();
	void UpdateSelfOwnedAttachEffects();
	bool HasAttachedEffects(std::vector<AttachEffectTypeClass*> attachEffectTypes, bool requireAll, bool ignoreSameSource, TechnoClass* pInvoker, AbstractClass* pSource, std::vector<int> const* minCounts, std::vector<int> const* maxCounts) const;
	int GetAttachedEffectCumulativeCount(AttachEffectTypeClass* pAttachEffectType, bool ignoreSameSource = false, TechnoClass* pInvoker = nullptr, AbstractClass* pSource = nullptr) const;
	void InitializeDisplayInfo(TechnoTypeClass* pType);
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

	static void ChangeOwnerMissionFix(FootClass* pThis, TechnoTypeClass* pType);
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
	static bool IsTypeImmune(TechnoClass* pThis, TechnoTypeClass* pType, TechnoClass* pSource);
	static int GetTintColor(TechnoClass* pThis, bool invulnerability, bool airstrike, bool berserk);
	static int GetCustomTintColor(TechnoClass* pThis);
	static int GetCustomTintIntensity(TechnoClass* pThis);
	static void ApplyCustomTintValues(TechnoClass* pThis, int& color, int& intensity);
	static Point2D GetScreenLocation(TechnoClass* pThis);
	static Point2D GetFootSelectBracketPosition(TechnoClass* pThis, Anchor anchor, bool isInfantry);
	static Point2D GetBuildingSelectBracketPosition(TechnoClass* pThis, TechnoTypeClass* pType, BuildingSelectBracketPosition bracketPosition);
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


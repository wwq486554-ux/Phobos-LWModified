#pragma once

#include <TechnoClass.h>
#include <GeneralDefinitions.h>

#include <New/Type/SpecialActionType.h>

class TechnoExt;

// Shared SpecialAction plumbing.
//
// The actual behaviour dispatch lives here so that it can be driven from the
// hotkey command and, for the Weapon action, from the engine's own attack.
namespace SpecialAction
{
	// =====================================================================
	// MASTER SWITCH - start here when the game misbehaves.
	//
	// The whole feature can be taken out of the running with this single flag:
	//   * every hook this feature added returns immediately;
	//   * the force-weapon branch falls through to Phobos' original code;
	//   * the command still registers, so the hotkey list looks unchanged, but
	//     pressing it does nothing.
	//
	// Set it to false whenever the game crashes or misbehaves and you need to
	// know whether this feature is involved at all. When it is false and the
	// problem is still there, the cause is somewhere else.
	// =====================================================================
	constexpr bool Enabled = true;

	// Returns the action data that applies to the given techno right now,
	// resolving the elite override field by field. Returned by value because the
	// elite entry only supplies what it actually overrides; an unset value keeps
	// the regular entry's.
	SpecialActionData GetData(TechnoClass* pTechno);

	// Cooldown bookkeeping.
	bool IsOnCooldown(TechnoClass* pTechno);
	// Effective cooldown length in frames, or -1 when the ability has none.
	// Resolves the elite override exactly like GetData, but returns a plain int so
	// that per-frame callers (the pip strip) do not copy the whole data block.
	int GetEffectiveROF(TechnoClass* pTechno);
	void StartCooldown(TechnoClass* pTechno, const SpecialActionData& data);
	// Plays the configured "not ready" sound, if any.
	void PlayNotReadyFeedback(TechnoClass* pTechno, const SpecialActionData& data);

	// ---------------------------------------------------------------------
	// Weapon action: "the next attack fires the ability's weapon".
	//
	// The action deliberately does NOT take over targeting or the cursor. The
	// hotkey only arms the unit; the player then attacks normally - a forced
	// attack, an explicit target, or the unit's own target acquisition - and the
	// next firing cycle uses the slot the ability named. Giving the unit any
	// other order (move, deploy, ...) disarms it again.
	//
	// A firing cycle is one weapon discharge including all of its Burst shots,
	// followed by the weapon's ROF. The ability takes effect at a cycle boundary,
	// so an attack that is already running finishes with the weapon it started
	// with and the ability applies to the following cycle.
	// ---------------------------------------------------------------------

	// Names the slot the ability will fire and marks the unit as armed. The slot
	// is one of the unit's own weapon slots, so the engine still does all the
	// firing. Returns false when the slot does not exist or holds no weapon.
	//
	// ArmWeapon and DisarmWeapon run on the machine that issued the order and only
	// queue the synchronized event; the state itself is written on every machine
	// by ApplyArmedWeapon, so all of them pick the same weapon.
	bool ArmWeapon(TechnoClass* pTechno, const SpecialActionData& data);

	// Drops the armed state, on every machine, the same way.
	void DisarmWeapon(TechnoClass* pTechno);

	// Simulation side of the two functions above: the EventExt responder calls
	// this on every machine, and nothing else does. A negative slot disarms.
	void ApplyArmedWeapon(TechnoClass* pTechno, int slot);

	// Weapon slot the engine must use while the ability's firing cycle is
	// running, or -1 for "let the engine decide". Consumed by the force-weapon
	// hook in Hooks.Firing.cpp.
	int GetForcedWeaponSlot(TechnoClass* pTechno);

	// Called from the firing hook once the ability's whole burst has been fired.
	// The cooldown starts here rather than when the key was pressed, so an
	// ability that was armed and then cancelled costs nothing.
	void NotifyWeaponCycleFinished(TechnoClass* pTechno);

	// Called when the unit is given an order. Every order except Attack replaces
	// the pending ability.
	void NotifyOrderIssued(TechnoClass* pTechno, Mission mission);

	// ---------------------------------------------------------------------
	// SuperWeapon action with SuperWeaponSource=unit: "the unit carries a super
	// weapon the house does not own".
	//
	// The super weapon is never granted, so it stays invisible to the house: no
	// sidebar entry, no recharge, no EVA, and nothing for the engine's (or Ares')
	// availability recomputation to iterate over. The launch turns it on for the
	// duration of one ClickFire call and puts it back exactly as it was, which is
	// why every field ClickFire touches has to be saved and restored.
	//
	// The launch has to travel as an event: ClickFire runs the effect, so calling
	// it straight from the key press would only affect the machine that pressed
	// the key.
	// ---------------------------------------------------------------------

	// Simulation side, called by the EventExt responder on every machine and by
	// nothing else. Restores the super weapon's ownership bookkeeping afterwards.
	void FireAttachedSuperWeapon(TechnoClass* pTechno, int superIndex, const CellStruct& cell);

	// ---------------------------------------------------------------------
	// AttachEffect action: "release and/or strip these effects on myself".
	//
	// Applying the effects is not something the key press may do directly: the
	// effects can consume the synchronized random number generator, so the press
	// only checks whether anything would happen and queues the work as an event.
	// The responder then applies it on every machine in the same frame, which also
	// lets it start the ability's cooldown there - so every machine agrees on when
	// the cooldown began, unlike the actions that start it locally.
	// ---------------------------------------------------------------------

	// Simulation side, called by the EventExt responder on every machine and by
	// nothing else. Attaches, then strips, then charges the cooldown.
	void ApplyAttachEffect(TechnoClass* pTechno);

	// ---------------------------------------------------------------------
	// Return action: "fly home right now, boosted while cruising home".
	//
	// Only meaningful for AircraftTypes that opted in via
	// AdvancedAircraftMissions=yes; the eligibility (gate, airport available,
	// not spawned, in the air, ...) is checked here so that the key press can
	// report "not ready" instead of burning the cooldown for nothing.
	//
	// Like the other state-changing actions, the press only queues the
	// synchronized event: the responder runs AdvancedMissions::StartReturnToBase
	// on every machine, which is also where the cooldown is charged - so the
	// right-click trigger below agrees with the key press on when it started.
	// ---------------------------------------------------------------------

	// Returns true when the given techno's ability (elite-resolved) is Return.
	bool HasReturnAction(TechnoClass* pTechno);

	// producer side (the machine that issued the order): queue the synchronized
	// Return event. cancel = true drops an active return boost instead.
	bool QueueReturnToBase(TechnoClass* pTechno, bool cancel = false);

	// responder side: charge the ability's cooldown for the unit, on every machine.
	void StartReturnCooldown(TechnoClass* pTechno);

	// Performs the action. Returns true when it actually took effect.
	bool Execute(TechnoClass* pTechno);
}

#pragma once

#include <GeneralStructures.h>
#include <TargetClass.h>
#include <TechnoClass.h>

class UnitClass;

// Carryall: a Jumpjet vehicle that picks another vehicle up and puts it down again.
//
// The carrier is an ordinary UnitType that opted in with Carryall=yes and moves with
// the Jumpjet locomotor. The vanilla AircraftType carryall is a separate code path and
// none of this touches it.
//
// Orders travel through the engine's own multiplayer pipeline:
//   local input (Alt + click / the DropPayload special action)
//     -> TechnoClass::ClickedMission(Mission::Unload, target, ...)
//     -> the engine queues a MegaMission event,
//     -> every machine ends up with Mission/QueuedMission == Unload and Target set.
// The per-frame state machine below then drives the whole sequence on every machine
// from those engine fields plus the shared world state. There is no custom event type:
// only the local input is local, everything after it is deterministic.
class Carryall
{
public:
	// ---- type / instance predicates ----

	// Whether this object's type opted in (Carryall=yes on a Jumpjet unit type).
	static bool IsCarrierType(TechnoClass* pCarrier);

	// A carrier that is idle and is not itself being carried.
	static bool IsFreeCarrier(TechnoClass* pCarrier);

	// A carrier that is currently holding a payload.
	static bool IsCarrying(TechnoClass* pCarrier);

	// Everything the cursor and the click need to agree on: carrier eligibility, the
	// instance state of both ends and the type checks of UnitTypeExt::CanLift.
	static bool CanLiftNow(TechnoClass* pCarrier, TechnoClass* pTarget);

	// ---- order entry points (local machine only) ----

	// Turns the local input into one engine MegaMission order. The engine replicates it;
	// returns false when the local input is not a valid order (nothing is queued then).
	static bool IssuePickupOrder(UnitClass* pCarrier, TechnoClass* pTarget);
	static bool IssueDropOrder(UnitClass* pCarrier);

	// ---- per-frame state machine (every machine, same order) ----

	// Carrier side, called once a frame for every unit.
	static void Update(UnitClass* pThis);

	// Payload side, called once a frame for every unit. Follows nothing by itself - the
	// carrier moves it - and carries out the "the carrier died, so I die too" rule, but
	// only for a payload our own attach step hooked on.
	static void UpdatePayload(UnitClass* pThis);

	// ---- hooks ----

	// True while the carrier is in a purely vertical phase (landing, dropping, ascending).
	// The jumpjet's kinematics are frozen for the duration, so the aircraft descends and
	// climbs straight down/up without its locomotion flying it sideways.
	static bool IsVerticallyControlled(TechnoClass* pTechno);

	// H16: must this object be refused a Deploy right now? True for a payload we are
	// carrying and for a carrier that is already busy with a pick-up/drop sequence.
	static bool BlocksDeploy(TechnoClass* pTechno);

	// H14: the payload to draw under this carrier (carrier side only), or null.
	static UnitClass* GetHeldPayload(TechnoClass* pCarrier);

	// H15: the carrier was destroyed - the payload dies with it, the same way Ares kills
	// the vanilla carryall cargo (health as damage, C4 warhead).
	static void OnCarrierDestroyed(TechnoClass* pCarrier);

	// H2 guard: while one of our sequences is being driven, the vanilla
	// UnitClass::Mission_Unload body is not allowed to run (it would treat the order as
	// its own unload/deploy request). Only reachable for unit types with Passengers > 0.
	static bool BlocksVanillaUnload(TechnoClass* pCarrier);

	// H4 guard: whether this techno is a payload our own attach step has hooked onto a
	// carrier (then its own jumpjet kinematics must not run).
	static bool IsOurCarriedPayload(TechnoClass* pPayload);

	// ---- shared helpers ----

	// Resolves an engine reference to a techno that is still present in the engine's
	// logic list, or null.
	//
	// TargetClass::As_Techno alone is NOT enough: it only compares the stored RTTI and
	// then returns whatever the engine's id index holds, which for an object destroyed a
	// moment ago is the address of memory that has already been reused. Every reference
	// this feature reads has to come through here before it is dereferenced.
	static TechnoClass* ResolveLiveTechno(TargetClass& link);

	// Pure cell test for "a vehicle of this payload's type may be put down here".
	// It deliberately does not look at the payload's own current position or occupation
	// state, so the verdict cannot flip between the moment the drop is decided and the
	// moment the payload is released.
	//
	// The carrier is passed in because the cell the carrier itself hovers over has to
	// count as free: that is where a drop in place lands, and the carrier leaves it right
	// after the release.
	static bool IsDropCellUsable(const CellStruct& cell, UnitClass* pPayload, UnitClass* pCarrier = nullptr);

	// Nearest usable drop cell to the carrier, ring search outwards. Returns false when
	// no usable cell is found inside the search radius - the caller must then keep
	// holding the payload, never force it down.
	static bool FindDropCell(UnitClass* pCarrier, UnitClass* pPayload, CellStruct& outCell);
};

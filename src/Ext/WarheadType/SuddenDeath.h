#pragma once

#include <Utilities/Enum.h>

#include <GeneralStructures.h>

class TechnoClass;
class HouseClass;

// SuddenDeath execution queue.
//
// A SuddenDeath warhead does not kill inside the damage call it arrives in. That call runs deep
// inside the projectile detonation, where destroying the target re-enters the object bookkeeping
// that is still being walked: the shipyard crash this queue was written for is an AnimClass being
// destroyed a second time out of the pointer-expired notification, which leaves the outer
// ObjectClass::UnInit to call through a vtable that has already fallen back to AbstractClass.
//
// So a hit only *records* what is supposed to die. The engine settles the hit itself, in the shape
// it already knows - the warhead's own damage, its own gates, its own death path, nothing
// rewritten - and the kill is executed at the next safe point (the head of LogicClass::Update,
// before any object is updated). By then the target is either already dead, in which case there is
// nothing left to do, or standing somewhere the engine can be asked to kill it with a plain,
// neutral C4Warhead execution - the same call Phobos' AutoDeath uses for its own kills.
//
// Terrain is the one thing that cannot be handed to that call at all: the damage pipeline never
// reaches a TerrainClass, so a tree or a tiberium pillar is recorded as a cell sweep instead and
// removed through the terrain removal path Phobos already uses for its crumbling frames.
namespace SuddenDeath
{
	// Diagnostics for this feature, off by default. The lines below are how the mechanism was
	// verified in game - queue -> execute, cell sweeps, the cursor bypass - and they cost nothing
	// while this is false (the logging calls compile away). Flip it to true, rebuild and deploy
	// to see them again when something needs re-measuring.
	constexpr bool Diagnostics = false;
	// Record one Techno the warhead reached. Duplicates are collapsed, so a target hit by several
	// projectiles in the same frame is killed once.
	void RegisterTechno(TechnoClass* pTarget, TechnoClass* pAttacker, HouseClass* pAttackingHouse, SuddenDeathMode mode);

	// Record a detonation that has to sweep the terrain of the cells it covers.
	void RegisterTerrain(const CoordStruct& coords, double cellSpread);

	// Execute everything recorded so far. Called once per logic frame, at the head of
	// LogicClass::Update. Always empties both queues.
	void ExecutePending();

	// Drop everything recorded. Called when a scenario is loaded: a new scenario must never
	// inherit pending hits that point into the old one.
	void ClearPending();

	// Cursor layer. The engine's three per-class cursor functions (Aircraft/Infantry/Unit) all
	// compute the action they would offer for the object under the mouse by calling the shared
	// helper at 0x4DDED0, and each of them then downgrades Attack to "cannot attack" when the
	// target's type is Immune (0x417F63 / 0x51F179 / 0x74049A - see the fork's notes). An
	// executioner warhead kills regardless of immunity, so that downgrade is wrong while one is
	// in hand: the helper's entry records whose cursor is being computed, and the three downgrade
	// sites ask about that unit. NoteCursorTechno is called from the 0x4DDED0 hook.
	void NoteCursorTechno(TechnoClass* pTechno);
	bool CursorHoldsExecutionerWeapon();
}

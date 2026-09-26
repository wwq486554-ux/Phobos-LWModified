#pragma once

#include <Utilities/TemplateDef.h>
#include <New/Type/AttachEffectTypeClass.h>

// Which behaviour a SpecialAction performs when the player triggers it.
// The set is intentionally small and each entry maps to an existing engine
// entry point, so no new gameplay logic has to be invented here.
//
// New entries are only ever appended: the enum value is written to the savegame,
// so reordering or renumbering the existing ones would misread old saves.
enum class SpecialActionType
{
	None = 0,      // no action configured
	Deploy,        // the whole vanilla deploy package (EventType::Deploy)
	DeploysInto,   // only the unit -> building transformation
	Convert,       // only the unit -> unit type conversion (one way)
	DeployFire,    // fire a chosen weapon at the unit's own cell
	Unload,        // only unload passengers
	Weapon,        // prep one of the unit's own slots for its next firing cycle
	SuperWeapon,   // fire a chosen super weapon
	AttachEffect,  // attach and/or strip AttachEffects on the unit itself
	Return         // AdvancedAircraftMissions: fly home now, boosted while cruising home
};

// Where a SuperWeapon action aims. The engine's super weapon launch always takes
// a cell, so an ability that does not want to aim still has to name one.
enum class SpecialActionAim
{
	Unit = 0,   // the unit's own cell - the pre-existing behaviour, and the default
	Self,       // the owner's building that provides this super weapon
	Empty       // (0, 0), the cell vanilla itself uses for a no-target super weapon
};

// Where a SuperWeapon action takes the super weapon from.
//
// House is the pre-existing behaviour: the ability only fires a super weapon the
// house already owns - either provided by a building, or granted permanently to
// every house by <SuperWeapon>.AlwaysGranted. The engine refuses to launch one
// the house does not have, so such an ability simply does nothing for a house
// that never acquired it.
//
// Unit lets the ability carry the super weapon itself: the unit exists, so the
// ability works, and losing the unit loses the ability. The house never "has"
// the super weapon, which is what keeps it out of the sidebar, out of EVA, out
// of the recharge system and out of the per-house availability bookkeeping that
// AlwaysGranted entries force on every house. The ability's own ROF is then the
// only cooldown. This is why the attached super weapon has to be launched
// through its own event instead of the engine's SpecialPlace path: that path
// re-derives the house's ownership at every hop.
enum class SpecialActionSource
{
	House = 0,  // the house must already own the super weapon - the default
	Unit        // the ability provides the super weapon while the unit lives
};

// One parsed SpecialAction= entry.
//
// Only plain values live here on purpose: a TechnoTypeExt is streamed as part
// of the type registry, so anything holding a raw pointer would have to be
// registered with the swizzler. Weapon and super weapon are therefore kept as
// ids and resolved through the type arrays when the action actually runs.
struct SpecialActionData
{
	Valueable<SpecialActionType> Action { SpecialActionType::None };
	Nullable<int> ROF {};                     // cooldown in frames, unset means none
	Nullable<int> Voice {};                   // VocClass queued as key-press feedback (a unit voice)
	Nullable<int> Sound {};                   // VocClass played at the unit as key-press feedback
	Nullable<int> NotReadySound {};           // VocClass index played while on cooldown
	PhobosFixedString<0x40> NotReadyMessage {}; // CSF label shown while on cooldown

	// --- parameters, only the one matching Action is meaningful ---
	//
	// Each of these is filled either from the inline form
	// ("SpecialAction=DeployFire,1") or from the action's own key
	// ("SpecialAction.DeployFire=1"), and the dedicated key wins. The deploy
	// actions take their parameter from here rather than from the type's vanilla
	// deploy keys, so a unit can keep one deploy behaviour on the Deploy hotkey
	// and give the ability another one without the two clashing.
	PhobosFixedString<0x20> WeaponID {};       // Weapon / DeployFire, weapon id or slot number
	PhobosFixedString<0x20> SuperWeaponID {};  // SuperWeapon
	PhobosFixedString<0x20> ConvertToID {};    // Convert, target TechnoType
	PhobosFixedString<0x20> DeploysIntoID {};  // DeploysInto, target BuildingType

	// Second parameter of SuperWeapon: where to aim. Unset means Unit, which is
	// what the action did before the key existed.
	Nullable<SpecialActionAim> Aim {};

	// Third parameter of SuperWeapon: where the super weapon comes from. Unset
	// means House, which is what the action did before the key existed.
	Nullable<SpecialActionSource> Source {};

	// AttachEffect action: which effects to release on the unit itself and which
	// to strip again. Only some of AEAttachInfoTypeClass is ever filled in - see
	// ReadSpecialActionAttachEffect, which reads the four keys this action
	// supports and deliberately leaves the cumulative/self-owned ones at their
	// defaults. The action always applies non self-owned effects, so those keys
	// would have no effect anyway.
	AEAttachInfoTypeClass AttachEffect {};

	SpecialActionData() = default;

	// Stream hooks. Named Load/Save so that Savegame::ImplementsUpperCaseSaveLoad
	// picks them up instead of falling back to a raw byte copy of this struct.
	bool Load(PhobosStreamReader& Stm, bool RegisterForChange)
	{
		return this->Process(Stm, RegisterForChange);
	}

	bool Save(PhobosStreamWriter& Stm) const
	{
		return this->Process(Stm);
	}

private:
	// Keeps the serialization self-contained: the reader and writer paths must
	// process exactly the same members in exactly the same order.
	bool Process(PhobosStreamReader& Stm, bool RegisterForChange)
	{
		Stm
			.Process(this->Action, RegisterForChange)
			.Process(this->ROF, RegisterForChange)
			.Process(this->Voice, RegisterForChange)
			.Process(this->Sound, RegisterForChange)
			.Process(this->NotReadySound, RegisterForChange)
			.Process(this->NotReadyMessage, RegisterForChange)
			.Process(this->WeaponID, RegisterForChange)
			.Process(this->SuperWeaponID, RegisterForChange)
			.Process(this->ConvertToID, RegisterForChange)
			.Process(this->DeploysIntoID, RegisterForChange)
			.Process(this->Aim, RegisterForChange)
			.Process(this->Source, RegisterForChange)
			.Process(this->AttachEffect, RegisterForChange);

		return Stm.Success();
	}

	bool Process(PhobosStreamWriter& Stm) const
	{
		Stm
			.Process(this->Action)
			.Process(this->ROF)
			.Process(this->Voice)
			.Process(this->Sound)
			.Process(this->NotReadySound)
			.Process(this->NotReadyMessage)
			.Process(this->WeaponID)
			.Process(this->SuperWeaponID)
			.Process(this->ConvertToID)
			.Process(this->DeploysIntoID)
			.Process(this->Aim)
			.Process(this->Source)
			.Process(this->AttachEffect);

		return true;
	}
};

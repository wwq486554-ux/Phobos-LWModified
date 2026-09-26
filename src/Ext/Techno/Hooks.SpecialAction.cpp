#include <Ext/Techno/SpecialAction.h>
#include <Ext/Aircraft/Body.h>
#include <Ext/Aircraft/AdvancedMissions.h>

#include <GeneralDefinitions.h>
#include <Utilities/Macro.h>

// =============================================================================
// SpecialAction: replacing the armed Weapon action when the player orders
// something else.
//
// The Weapon action only arms a unit so that its next firing cycle uses the
// ability's weapon; it does not take over targeting or the cursor. Any other
// order the player gives that unit replaces the pending ability, which is what
// these two hooks detect.
//
// Both are the engine's own "the player clicked something" entry points at the
// TechnoClass level, reached by an ordinary call or through the vtable - not by
// hooking the map click handler:
//
//   0x6FFBE0  TechnoClass::ClickedMission(Mission, ObjectClass*, CellClass*, CellClass*)
//             vtable +0x378. Every mission the player can click is dispatched
//             here (Move, Enter, Capture, Harvest, Unload, Sabotage, Patrol,
//             Area_Guard, ...). Only orders that clearly send the unit off to do
//             something else drop the pending ability - see
//             SpecialAction::NotifyOrderIssued for the list and the reasoning.
//   0x6FFE00  TechnoClass::ClickedEvent(EventType)
//             vtable +0x374. Event-style orders - Deploy, Scatter and Idle are
//             the only event types ever passed to it - so all of them replace
//             the pending ability.
//
// AI orders do not come through here (they use QueueMission / ForceMission), so
// this only reacts to the local player's commands.
//
// DO NOT go back to hooking DisplayClass::LeftMouseButtonUp (0x4AB9F1).
// An earlier version of this feature did, to run its own targeting mode, and
// declared a length of 7 bytes for an address whose instruction is only 6 bytes
// long. The ordinary "return 0" path then resumed in the middle of the following
// instruction, and the game died on the first mouse click.
// =============================================================================

DEFINE_HOOK(0x6FFBE0, TechnoClass_ClickedMission_SpecialActionCancel, 0x6)
{
	if constexpr (SpecialAction::Enabled)
	{
		GET(TechnoClass* const, pThis, ECX);
		GET_STACK(Mission, mission, 0x4);   // arg1; __thiscall, so the args start at esp+4
		GET_STACK(ObjectClass* const, pTarget, 0x8); // arg2: the clicked object, if any

		SpecialAction::NotifyOrderIssued(pThis, mission);

		// AdvancedAircraftMissions: the second trigger of the Return ability.
		//
		// A player-issued order is the only thing that may start a boosted return
		// or drop one; AI orders use QueueMission/ForceMission and never reach this
		// hook. Both operations are turned into the same synchronized event the
		// hotkey uses, because this hook only runs on the machine that issued the
		// order.
		if (auto const pAircraft = abstract_cast<AircraftClass*, true>(pThis))
		{
			if (AdvancedMissions::Enabled(pAircraft->Type))
			{
				const bool returnOrder = mission == Mission::Enter
					|| AdvancedMissions::IsOwnDockBuilding(pAircraft, pTarget);

				if (returnOrder)
				{
					// Only the player who configured the ability gets the boost,
					// and only while the ability is off cooldown. The engine's own
					// Enter order still happens either way, so the aircraft always
					// goes home - a cooling down ability merely loses the boost.
					if (SpecialAction::HasReturnAction(pThis)
						&& !SpecialAction::IsOnCooldown(pThis)
						&& !AircraftExt::Fetch(pAircraft)->ReturnOrderActive
						&& AdvancedMissions::CanOrderReturn(pAircraft))
					{
						SpecialAction::QueueReturnToBase(pThis);
					}
				}
				else if (AircraftExt::Fetch(pAircraft)->ReturnOrderActive)
				{
					// Any other order cancels an active return boost.
					SpecialAction::QueueReturnToBase(pThis, true);
				}
			}
		}
	}

	return 0;
}

// NOTE on the size: an earlier version declared 3 here, because the instruction
// at 0x6FFE00 (`sub esp,0x78`) really is 3 bytes long. That is not what the size
// means. Syringe always patches max(size, 5) bytes with a 5-byte JMP, and its
// trampoline resumes at hook+5, so a size below 5 is only safe when the bytes it
// does not cover are NOPs. Here they were `push esi` and `push edi`: both were
// overwritten by the JMP and skipped by the trampoline, leaving the stack frame
// 8 bytes short. The function then returned to garbage - observed as an access
// violation at EIP = 0, triggered by the first order that calls ClickedEvent
// (Stop / Deploy / Scatter). 5 bytes covers `sub esp,0x78` plus both pushes and
// ends on an instruction boundary.
//
// RULE for every hook added here: size must be >= 5 AND end on an instruction
// boundary.
DEFINE_HOOK(0x6FFE00, TechnoClass_ClickedEvent_SpecialActionCancel, 0x5)
{
	if constexpr (SpecialAction::Enabled)
	{
		GET(TechnoClass* const, pThis, ECX);

		SpecialAction::DisarmWeapon(pThis);

		// AdvancedAircraftMissions: Deploy / Scatter / Idle is "another order" too,
		// so an active return boost is dropped the same way as in ClickedMission.
		if (auto const pAircraft = abstract_cast<AircraftClass*, true>(pThis))
		{
			if (AdvancedMissions::Enabled(pAircraft->Type)
				&& AircraftExt::Fetch(pAircraft)->ReturnOrderActive)
			{
				SpecialAction::QueueReturnToBase(pThis, true);
			}
		}
	}

	return 0;
}

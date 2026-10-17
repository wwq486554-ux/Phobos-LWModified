#include <Ext/Unit/Carryall.h>

#include <Ext/Unit/Body.h>

#include <GeneralDefinitions.h>
#include <InputManagerClass.h>
#include <JumpjetLocomotionClass.h>
#include <UnitClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

// =============================================================================
// Carryall hooks.
//
// The feature is built the way the reference implementation builds it: it extends the
// game classes' own entry points instead of inventing a parallel state machine.
//
//   0x7388FD  UnitClass click handler (0x738890, vtable +0x144), just before the shared
//             action dispatcher 0x4D74E0. With the force-move key held on a vehicle the
//             carrier may lift, the click is turned into ONE engine order:
//             TechnoClass::ClickedMission(Mission::Unload, target). The engine packs that
//             into a MegaMission event, so every machine ends up with the same mission
//             and target - that is the whole synchronization story, no custom event.
//
//   0x73D6EC  Inside UnitClass::Mission_Unload (0x73D630), right after the type's
//             Passengers check. While one of our sequences is being driven, the vanilla
//             unload body must not run and treat our order as its own unload/deploy
//             request. NOTE: this point is only reached for unit types with
//             TechnoTypeClass::Passengers (+0x5E0) greater than zero; for an ordinary
//             carrier the state machine detects the order from the engine fields instead,
//             so this is a guard, not the primary path.
//
//   0x54AED0  Inside JumpjetLocomotionClass::Process (0x54AEC0). A payload that is itself
//             a jumpjet must not run its own kinematics while it hangs under the carrier;
//             the engine freezes it through the same early exit the reference uses.
// =============================================================================

DEFINE_HOOK(0x7388FD, UnitClass_ActiveClickWith_Carryall, 0x5)
{
	enum { ContinueToDispatcher = 0, SkipToEpilogue = 0x738907 };

	GET(UnitClass* const, pThis, ESI);
	GET(ObjectClass* const, pClicked, EDI);
	GET(Action const, action, ECX);

	// The key reads the player's own force-move binding (Alt by default), exactly like the
	// cursor wrapper in Ext/Techno/Hooks.TargetEvaluation.cpp.
	if (!pClicked || !InputManagerClass::Instance->IsForceMoveKeyPressed())
		return ContinueToDispatcher;

	// A force move on a vehicle arrives here as either of these; anything else keeps the
	// vanilla behaviour untouched.
	if (action != Action::Tote && action != Action::Move)
		return ContinueToDispatcher;

	auto const pTarget = abstract_cast<TechnoClass*>(pClicked);

	if (!Carryall::CanLiftNow(pThis, pTarget))
		return ContinueToDispatcher;

	if (!Carryall::IssuePickupOrder(pThis, pTarget))
		return ContinueToDispatcher;

	// The order was queued as a MegaMission event: the shared dispatcher must not also
	// issue its own mission for this click.
	return SkipToEpilogue;
}

DEFINE_HOOK(0x73D6EC, UnitClass_Mission_Unload_CarryallGuard, 0x6)
{
	enum
	{
		NotOursPassengers = 0x73DCD3, // the jle taken: Type->Passengers <= 0
		NotOursContinue = 0x73D6F8,   // the jle not taken: continue the vanilla body
		ReturnZero = 0x73DED8         // epilogue that pops the frame without touching EAX
	};

	GET(TechnoClass* const, pThis, ESI);

	if (Carryall::BlocksVanillaUnload(pThis))
	{
		// Stay inside Mission::Unload for another frame; the state machine in
		// UnitClass::AI is what drives the sequence. A zero return value keeps the
		// mission handler's update timer at zero, i.e. it runs again next frame.
		R->EAX(0);
		return ReturnZero;
	}

	// Not one of ours: reproduce the stolen comparison exactly.
	// The stolen instruction is `cmp [eax+0x5E0], ebx` with EAX = Type and EBX = 0, and
	// the following instruction is `jle`. 0x5E0 is TechnoTypeClass::Passengers.
	const auto pType = reinterpret_cast<TechnoTypeClass*>(R->EAX());
	const int passengers = *reinterpret_cast<const int*>(
		reinterpret_cast<const char*>(pType) + 0x5E0);

	return passengers <= 0 ? NotOursPassengers : NotOursContinue;
}

DEFINE_HOOK(0x54AED0, JumpjetLocomotion_Process_CarryallFreeze, 0x5)
{
	enum { FrozenExit = 0x54B16C };

	// At this point ESI is the ILocomotion interface pointer of the jumpjet being
	// processed (COM methods take `this` as a stack argument). LocomotionClass::Owner
	// (+0x8 on the object, +0x4 from this pointer) and LinkedTo (+0xC / +0x8) both hold
	// the unit; the engine's own code just below reads [esi+8] for it, so this does the
	// same.
	auto const pThis = R->ESI();
	auto const pOwner = *reinterpret_cast<TechnoClass**>(
		reinterpret_cast<char*>(pThis) + 0x8);

	if (!pOwner)
		return 0;

	// Two cases are frozen: a payload that is itself a jumpjet (it must not fly around in
	// the cabin), and the carrier itself while it is in a purely vertical phase. For the
	// carrier this is what makes the descent vertical: its locomotion would otherwise keep
	// flying it sideways (a destination is re-issued by the movement AI every frame), which
	// is exactly the "it drifts sideways while descending" bug.
	if (!Carryall::IsOurCarriedPayload(pOwner) && !Carryall::IsVerticallyControlled(pOwner))
		return 0;

	// Skip the whole kinematics step of the frozen payload.
	return FrozenExit;
}



// H16: never let a deployable carrier Deploy in the middle of a carry sequence (or a
// carried payload Deploy by any path). This is the same guard Kaleidoscope installs in
// TechnoClass::CanDeploy (0x700E8B -> 0x700DCE, the function's own "cannot deploy" exit):
// without it, a unit that has Deployer=yes (the HID does) can be deployed while it is
// holding a vehicle - it turns into its ground form, the limboed payload is lost with it,
// and the aircraft "sits on the ground and never flies again".
DEFINE_HOOK(0x700E8B, TechnoClass_CanDeploy_CarryallGuard, 0x6)
{
	GET(TechnoClass* const, pThis, ESI);

	if (Carryall::BlocksDeploy(pThis))
		return 0x700DCE;

	return 0;
}

// H14: the carried payload is limboed (no entity, not selectable, not targetable, not drawn
// by the engine). The carrier draws its model itself: the engine's own TechnoClass::Draw is
// called for the payload with the carrier's draw coordinates - `IsOnCarryall` makes the
// engine shift it down by its 14px hang offset, so the vehicle visibly hangs below the
// carrier. This is the reference implementation's model: only the model, no entity.
DEFINE_HOOK(0x73CEC0, TechnoClass_Draw_CarryallPayload, 0x5)
{
	GET(TechnoClass* const, pThis, ECX);

	auto const pLocation = R->Stack<Point2D*>(4);
	auto const pBounds = R->Stack<RectangleStruct*>(8);

	if (!pLocation || !pBounds)
		return 0;

	auto const pPayload = Carryall::GetHeldPayload(pThis);

	if (!pPayload)
		return 0;

	using DrawFn = int(__thiscall*)(TechnoClass*, Point2D*, RectangleStruct*);
	static DrawFn const draw = reinterpret_cast<DrawFn>(0x73CEC0);

	draw(pPayload, pLocation, pBounds);

	return 0;
}


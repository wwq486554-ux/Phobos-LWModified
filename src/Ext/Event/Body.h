#pragma once
#include <EventClass.h>
#include <TargetClass.h>
#include <HouseClass.h>

#include <cstddef>
#include <stdint.h>

enum class EventTypeExt : uint8_t
{
	// Vanilla game used Events from 0x00 to 0x2F
	// CnCNet reserved Events from 0x30 to 0x3F
	// Ares used Events 0x60 and 0x61

	ApproachObject = 0x40,
	TogglePlayerAutoRepair = 0x41,
	SpecialActionWeapon = 0x42,
	SpecialActionSuperWeapon = 0x43,
	SpecialActionAttachEffect = 0x44,
	SpecialActionReturn = 0x45,

	FIRST = ApproachObject,
	LAST = SpecialActionReturn
};

#pragma pack(push, 1)
class EventExt
{
public:
	EventTypeExt Type;
	bool IsExecuted;
	char HouseIndex;
	uint32_t Frame;
	union
	{
		char DataBuffer[104];

		struct APPROACHOBJECT
		{
			TargetClass Whom;
			TargetClass Target;
		} ApproachObject;
		struct TogglePlayerAutoRepair
		{ } TogglePlayerAutoRepair;

		// Arms (WeaponSlot >= 0) or disarms (WeaponSlot < 0) one unit's pending
		// SpecialAction weapon. The chosen slot has to travel with the event: it
		// decides which weapon the unit fires next, so every machine must apply the
		// same value or the shot itself desynchronises.
		struct SPECIALACTIONWEAPON
		{
			TargetClass Whom;
			int WeaponSlot;
		} SpecialActionWeapon;

		// Launches one super weapon on behalf of one unit, without the owner ever
		// owning it. Like the weapon event above, everything the responder needs has
		// to travel with the event - the unit, which super weapon, and the cell -
		// because the responder re-derives nothing from local state and only turns
		// the super weapon on for the duration of the launch.
		struct SPECIALACTIONSUPERWEAPON
		{
			TargetClass Whom;
			int SuperIndex;
			CellStruct Cell;
		} SpecialActionSuperWeapon;

		// Releases and/or strips one unit's SpecialAction AttachEffects, on every
		// machine. The payload itself does not travel: it is type-level static data
		// that every machine already parsed and serialized with the TechnoType, so
		// the responder re-derives the exact same parameters from the unit alone.
		// Only "who" therefore needs to be sent.
		struct SPECIALACTIONATTACHEFFECT
		{
			TargetClass Whom;
		} SpecialActionAttachEffect;

		// Orders one unit to return to base (Cancel = false) or drops an active
		// return speed boost (Cancel = true), on every machine. The unit was
		// selected by AdvancedAircraftMissions; the payload itself is type-level
		// static data every machine already has, so only "who" and which of the
		// two operations has to travel.
		struct SPECIALACTIONRETURN
		{
			TargetClass Whom;
			bool Cancel;
		} SpecialActionReturn;
	};

	bool AddEvent();
	void RespondEvent();

	void RespondApproachObject();
	static void RaiseTogglePlayerAutoRepair();
	void RespondToTogglePlayerAutoRepair();
	void RespondSpecialActionWeapon();
	void RespondSpecialActionSuperWeapon();
	void RespondSpecialActionAttachEffect();
	void RespondSpecialActionReturn();

	static size_t GetDataSize(EventTypeExt type);
	static bool IsValidType(EventTypeExt type);
};

static_assert(sizeof(EventExt) == 111);
static_assert(offsetof(EventExt, DataBuffer) == 7);
#pragma pack(pop)


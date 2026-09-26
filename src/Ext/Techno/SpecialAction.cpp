#include "SpecialAction.h"

#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/Rules/Body.h>
#include <Ext/BuildingType/Body.h>
#include <Ext/SWType/Body.h>
#include <Ext/Aircraft/Body.h>
#include <Ext/Aircraft/AdvancedMissions.h>

#include <New/Entity/AttachEffectClass.h>
#include <Utilities/EnumFunctions.h>

#include <UnitClass.h>
#include <BuildingTypeClass.h>
#include <BuildingClass.h>
#include <ObjectClass.h>
#include <WeaponTypeClass.h>
#include <SuperWeaponTypeClass.h>
#include <SuperClass.h>
#include <EventClass.h>
#include <Ext/Event/Body.h>
#include <VocClass.h>
#include <MapClass.h>
#include <MissionClass.h>
#include <MessageListClass.h>
#include <StringTable.h>
#include <Utilities/Debug.h>
#include <HouseClass.h>
#include <RulesClass.h>

namespace SpecialAction
{
	namespace
	{
		// Defined further down. The DeployFire action resolves its weapon through
		// the very same helper the Weapon action uses, so both accept a weapon id or
		// a bare slot number and both look at every slot the type has.
		int ResolveWeaponSlot(TechnoClass* pTechno, const SpecialActionData& data);

		bool IsOwnedByLocalPlayer(TechnoClass* pTechno)
		{
			return pTechno && pTechno->Owner && pTechno->Owner->IsControlledByCurrentPlayer();
		}

		bool ExecuteDeploy(TechnoClass* pTechno)
		{
			// The whole vanilla deploy package: the engine decides internally
			// between transformation, deploy fire and unloading.
			return pTechno->ClickedEvent(EventType::Deploy);
		}

		// UnitClass::TryToDeploy - and the routines it calls - decide *what* to
		// build by reading the unit type's DeploysInto field; the function takes no
		// target parameter. An ability that names its own building therefore has to
		// point that field at it for the duration of the call, and restore the
		// original value on every exit path.
		//
		// This is the one deliberate exception to "never touch shared state". The
		// game is single-threaded and the deploy completes synchronously, so nothing
		// else can observe the temporary value; every machine runs the same command
		// and performs the same swap, so the result stays deterministic.
		class ScopedDeploysIntoOverride
		{
		public:
			ScopedDeploysIntoOverride(UnitClass* pUnit, BuildingTypeClass* pTarget)
				: pType { pUnit ? pUnit->Type : nullptr }
				, pOriginal { this->pType ? this->pType->DeploysInto : nullptr }
			{
				if (this->pType && pTarget && this->pOriginal != pTarget)
				{
					this->pType->DeploysInto = pTarget;
					this->applied = true;
				}
			}

			~ScopedDeploysIntoOverride()
			{
				if (this->applied)
					this->pType->DeploysInto = this->pOriginal;
			}

			// True while the type has been pointed at the ability's own building.
			bool Applied() const { return this->applied; }

			ScopedDeploysIntoOverride(const ScopedDeploysIntoOverride&) = delete;
			ScopedDeploysIntoOverride& operator=(const ScopedDeploysIntoOverride&) = delete;

		private:
			TechnoTypeClass* pType;
			BuildingTypeClass* pOriginal;
			bool applied { false };
		};

		bool ExecuteDeploysInto(TechnoClass* pTechno, const SpecialActionData& data)
		{
			auto const pUnit = abstract_cast<UnitClass*>(pTechno);

			if (!pUnit || !pUnit->Type)
				return false;

			// Without its own target the ability keeps using the type's DeploysInto,
			// which is what it did before the dedicated key existed.
			auto pTarget = pUnit->Type->DeploysInto;

			if (data.DeploysIntoID)
			{
				pTarget = abstract_cast<BuildingTypeClass*>(TechnoTypeClass::Find(data.DeploysIntoID));

				if (!pTarget)
					return false;
			}

			if (!pTarget)
				return false;

			// Covers the deploy check as well: the routines involved read the type's
			// DeploysInto too.
			ScopedDeploysIntoOverride scopedTarget { pUnit, pTarget };

			if (!pUnit->CanDeployNow())
			{
				Debug::Log("[SpecialAction] unit %d: cannot deploy into '%s' - the engine refused (CanDeployNow)\n",
					pTechno->Fetch_ID(), pTarget->ID ? pTarget->ID : "?");

				return false;
			}

			if (scopedTarget.Applied())
			{
				Debug::Log("[SpecialAction] unit %d: deploying into '%s' with the type's own DeploysInto overridden for this call\n",
					pTechno->Fetch_ID(), pTarget->ID ? pTarget->ID : "?");
			}

			return pUnit->TryToDeploy();
		}

		bool ExecuteConvert(TechnoClass* pTechno, const SpecialActionData& data)
		{
			if (!data.ConvertToID)
				return false;

			auto const pFoot = abstract_cast<FootClass*>(pTechno);

			if (!pFoot)
				return false;

			auto const pToType = TechnoTypeClass::Find(data.ConvertToID);
			auto const pFromType = pTechno->GetTechnoType();

			// Converting to itself would be a no-op loop; converting between
			// different kinds of objects is not something the engine supports.
			if (!pToType || pToType == pFromType || pToType->WhatAmI() != pFromType->WhatAmI())
				return false;

			return TechnoExt::ConvertToType(pFoot, pToType);
		}

		bool ExecuteDeployFire(TechnoClass* pTechno, const SpecialActionData& data)
		{
			int weaponIndex = -1;
			WeaponTypeClass* pWeapon = nullptr;

			if (data.WeaponID)
			{
				// The ability names its own weapon (id or slot), so none of the
				// vanilla deploy fire configuration - DeployFire, DeployFireWeapon,
				// TechnoTypeExt::DeployFireWeapon - is consulted. That is what leaves
				// the vanilla Deploy hotkey free to do something else.
				weaponIndex = ResolveWeaponSlot(pTechno, data);

				if (weaponIndex >= 0)
					pWeapon = pTechno->GetWeapon(weaponIndex)->WeaponType;
			}
			else
			{
				// Legacy entry without a weapon: keep using the type's own deploy
				// weapon definition.
				pWeapon = TechnoExt::GetDeployFireWeapon(pTechno, pTechno->GetTechnoType(), weaponIndex);
			}

			if (weaponIndex < 0 || !pWeapon)
				return false;

			auto const pCell = MapClass::Instance.GetCellAt(pTechno->GetMapCoords());
			pTechno->SetTarget(pCell);

			if (pTechno->GetFireError(pCell, weaponIndex, true) != FireError::OK)
			{
				pTechno->SetTarget(nullptr);
				return false;
			}

			return pTechno->Fire(pCell, weaponIndex) != nullptr;
		}

		bool ExecuteUnload(TechnoClass* pTechno)
		{
			// Mission::Unload is NOT a passenger-only entry point: it is the engine's
			// combined "deploy or unload" mission, and for a type that can deploy the
			// passenger phase is only a prefix of the deploy tail. The relevant edges
			// in UnitClass::Mission_Unload (0x73D630):
			//
			//   0x73D6EC  cmp [Type+0x5E0], 0     ; TechnoTypeClass::Passengers
			//   0x73D6F2  jle 0x73DCD3            ; nothing boardable -> skip the phase
			//   0x73DCD3  ...                     ; also where the 0x73D63B hook lands
			//   0x73D672  ...                     ; deploy checks (Harvester/Weeder/
			//                                     ; DeploysInto/IsSimpleDeployer)
			//   0x73DE6E  [Type+0xE13]            ; UnitTypeClass::IsSimpleDeployer
			//             -> 0x739AC0 Deploy() / 0x739CD0 Undeploy()
			//
			// The Phobos hook at 0x73D63B (Deploy.NoPassenger + nothing aboard) routes
			// into that very same tail, so asking the engine to unload a unit that has
			// nothing to unload *is* asking it to deploy. That is exactly what turned
			// "SpecialAction=Unload" into a second deploy key on a unit that also
			// deploys. An action named Unload must never do that, so refuse before the
			// mission is queued - and therefore before any cooldown is consumed.
			auto const pFoot = abstract_cast<FootClass*>(pTechno);

			if (!pFoot || pFoot->Passengers.NumPassengers <= 0)
			{
				Debug::Log("[SpecialAction] unit %d: nothing to unload - refusing "
					"(Mission::Unload would fall through to the deploy tail)\n",
					pTechno->Fetch_ID());
				return false;
			}

			if (auto const pExt = TechnoExt::TryFetch(pTechno))
				pExt->SpecialActionUnloading = true;

			Debug::Log("[SpecialAction] unit %d: unload requested - %d aboard, type capacity %d; "
				"forcing the passenger phase for this mission\n",
				pTechno->Fetch_ID(), pFoot->Passengers.NumPassengers,
				pFoot->GetTechnoType()->Passengers);

			return pTechno->ClickedMission(Mission::Unload, nullptr, nullptr, nullptr);
		}

		// Resolves the ability's parameter to one of the unit's own weapon slots.
		// Only slots the type actually carries are accepted, so a typo in the INI
		// cannot make the unit fire nothing.
		int ResolveWeaponSlot(TechnoClass* pTechno, const SpecialActionData& data)
		{
			auto const pType = pTechno ? pTechno->GetTechnoType() : nullptr;

			if (!pType)
				return -1;

			// Slots are enumerated over the whole array (MaxWeapons). WeaponCount is
			// NOT "how many weapons this type has": it belongs to the multi-weapon
			// selection feature and stays 0 on ordinary types, so using it here
			// rejected every unit.
			constexpr int MaxSlots = TechnoTypeClass::MaxWeapons;

			// A slot counts as usable when either the normal or the elite array holds
			// a weapon there; which one the engine actually fires depends on the
			// unit's veterancy.
			auto const slotHasWeapon = [pType](int index) -> bool
			{
				return pType->GetWeapon(index, false).WeaponType != nullptr
					|| pType->GetWeapon(index, true).WeaponType != nullptr;
			};

			// The engine fires weapons by slot, so the ability must name one of the
			// unit's own slots. A weapon id is accepted too and resolved to whichever
			// slot holds it; a bare number is read as the slot itself.
			int slot = -1;

			if (data.WeaponID)
			{
				const char* const pValue = data.WeaponID;

				bool allDigits = *pValue != '\0';

				for (const char* p = pValue; *p; ++p)
				{
					if (*p < '0' || *p > '9')
					{
						allDigits = false;
						break;
					}
				}

				if (allDigits)
				{
					for (const char* p = pValue; *p; ++p)
						slot = slot < 0 ? (*p - '0') : (slot * 10 + (*p - '0'));
				}
				else if (auto const pWeapon = WeaponTypeClass::Find(pValue))
				{
					for (int i = 0; i < MaxSlots; ++i)
					{
						if (pType->GetWeapon(i, false).WeaponType == pWeapon
							|| pType->GetWeapon(i, true).WeaponType == pWeapon)
						{
							slot = i;
							break;
						}
					}
				}
			}

			if (slot < 0 || slot >= MaxSlots || !slotHasWeapon(slot))
				return -1;

			return slot;
		}

		// Queues a SpecialPlace event so that the super weapon runs on every
		// machine through the normal event queue. Calling ClickFire or Launch
		// directly from player input would only affect this machine and desync.
		bool FireSuperWeaponAt(TechnoClass* pTechno, int swIndex, const CellStruct& cell)
		{
			auto const pHouse = pTechno->Owner;

			if (!pHouse || swIndex < 0)
				return false;

			EventClass event { pHouse->ArrayIndex, EventType::SpecialPlace, swIndex, cell };
			return EventClass::OutList.Add(event);
		}

		// Queues the "this unit fires this super weapon" event. Deliberately not
		// EventType::SpecialPlace: that one makes every machine look the super weapon
		// up through the owner's own ownership state (HouseClass::Fire_SW), and Ares
		// additionally hooks the SpecialPlace branch itself. An attached super weapon
		// is not owned by the house, so it has to travel in an event of its own.
		bool QueueAttachedSuperWeapon(TechnoClass* pTechno, int swIndex, const CellStruct& cell)
		{
			auto const pHouse = pTechno->Owner;

			if (!pHouse || swIndex < 0)
				return false;

			EventExt event {};
			event.Type = EventTypeExt::SpecialActionSuperWeapon;
			event.HouseIndex = static_cast<char>(pHouse->ArrayIndex);
			event.Frame = Unsorted::CurrentFrame;
			event.SpecialActionSuperWeapon.Whom = TargetClass(pTechno);
			event.SpecialActionSuperWeapon.SuperIndex = swIndex;
			event.SpecialActionSuperWeapon.Cell = cell;

			return event.AddEvent();
		}

		// Where the launch aims. The engine always takes a cell, so an ability that
		// does not want to aim still has to name one; SpecialAction.SuperWeaponTarget
		// picks which.
		CellStruct ResolveSuperWeaponCell(TechnoClass* pTechno, SuperWeaponTypeClass* pType, const SpecialActionData& data)
		{
			auto const aim = data.Aim.Get(SpecialActionAim::Unit);

			// What vanilla itself passes for a super weapon whose Action is None.
			if (aim == SpecialActionAim::Empty)
				return CellStruct { 0, 0 };

			if (aim == SpecialActionAim::Self && pTechno->Owner)
			{
				// The owner's building that provides this super weapon, so that an
				// Ares "AITargeting=Self" style super weapon lands where a sidebar
				// launch would put it.
				for (auto const& pBuilding : pTechno->Owner->Buildings)
				{
					if (!pBuilding || !pBuilding->IsAlive || pBuilding->InLimbo || !pBuilding->Type)
						continue;

					auto const pBuildingExt = BuildingTypeExt::Fetch(pBuilding->Type);

					for (int i = 0; i < pBuildingExt->GetSuperWeaponCount(); ++i)
					{
						if (pBuildingExt->GetSuperWeaponIndex(i) == pType->ArrayIndex)
							return pBuilding->GetMapCoords();
					}
				}
			}

			// Unit, and the fallback for Self when no source building exists.
			return pTechno->GetMapCoords();
		}

		// Fires the ability's super weapon through the normal SpecialPlace queue, so
		// charge and multiplayer synchronisation match a sidebar launch.
		bool ExecuteSuperWeapon(TechnoClass* pTechno, const SpecialActionData& data)
		{
			if (!data.SuperWeaponID)
				return false;

			auto const pType = SuperWeaponTypeClass::Find(data.SuperWeaponID);

			if (!pType || pType->ArrayIndex < 0)
				return false;

			auto const pHouse = pTechno->Owner;

			if (!pHouse)
				return false;

			// The launch cell is resolved once and shared by every check below, so the
			// proximity test and the launch can never disagree about where it lands.
			auto const cell = ResolveSuperWeaponCell(pTechno, pType, data);

			// Inhibitors and designators have to be checked here. The engine and Ares
			// only evaluate them in the sidebar cameo click, the cursor and the AI's
			// super weapon selection; the launch path itself - which is the only one an
			// ability key press uses - never looks at them. Phobos re-implements the
			// same two calls for its own programmatic launches (SW.Next in
			// FireSuperWeapon.cpp and the LaunchSW warhead in Detonate.cpp), and this
			// follows that pattern exactly.
			//
			// The two helpers are not symmetrical: HasInhibitor reports a prohibition,
			// so "no inhibitors configured" is false, while HasDesignator reports a
			// requirement and treats "no designators configured" as satisfied.
			auto const pSWExt = SWTypeExt::Fetch(pType);
			auto const hasInhibitor = pSWExt->HasInhibitor(pHouse, cell);
			auto const hasDesignator = pSWExt->HasDesignator(pHouse, cell);

			if (hasInhibitor || !hasDesignator)
			{
				Debug::Log("[SpecialAction] unit %d: super weapon '%s' blocked at (%d,%d) - inhibitor=%d designator=%d\n",
					pTechno->Fetch_ID(), pType->ID ? pType->ID : "?", cell.X, cell.Y,
					hasInhibitor ? 1 : 0, hasDesignator ? 1 : 0);

				return false;
			}

			// Unit-sourced super weapon: the ability brings the super weapon with it,
			// so the house's ownership is not consulted at all. The only gate is the
			// ability's own cooldown, which Execute already checked before dispatching.
			// Requiring IsPresent/CanFire here would reject exactly the case this mode
			// exists for - a house that never acquired the super weapon.
			if (data.Source.Get(SpecialActionSource::House) == SpecialActionSource::Unit)
			{
				// Every house owns one SuperClass per registered super weapon type
				// (HouseClass::HouseClass creates them unconditionally), so a missing
				// entry means the type is not registered - but check anyway.
				auto const pSuper = pHouse->Supers.GetItemOrDefault(pType->ArrayIndex);

				if (!pSuper)
				{
					Debug::Log("[SpecialAction] unit %d: attached super weapon '%s' has no SuperClass\n",
						pTechno->Fetch_ID(), pType->ID ? pType->ID : "?");

					return false;
				}

				if (!QueueAttachedSuperWeapon(pTechno, pType->ArrayIndex, cell))
				{
					Debug::Log("[SpecialAction] unit %d: attached super weapon '%s' -> EVENT QUEUE REFUSED IT\n",
						pTechno->Fetch_ID(), pType->ID ? pType->ID : "?");

					return false;
				}

				return true;
			}

			// The engine refuses to launch a super weapon the house does not have, or
			// one that is still recharging, and Fire_SW throws ClickFire's answer away
			// - so the key press would look like it worked while nothing happened.
			// Check first, so the ability reports "not ready" instead and does not
			// burn its own cooldown. This only reads synchronised simulation state;
			// the launch itself still goes through the event queue.
			auto const pSuper = pHouse->Supers.GetItemOrDefault(pType->ArrayIndex);
			auto const canFire = pSuper && pSuper->IsPresent && pSuper->CanFire();

			if (!canFire)
			{
				Debug::Log("[SpecialAction] unit %d: super weapon '%s' is not launchable (present=%d, canfire=%d)\n",
					pTechno->Fetch_ID(), pType->ID ? pType->ID : "?",
					pSuper ? pSuper->IsPresent : 0, pSuper ? pSuper->CanFire() : 0);

				return false;
			}

			return FireSuperWeaponAt(pTechno, pType->ArrayIndex, cell);
		}

		// ---------------------------------------------------------------------
		// AttachEffect action: "release / strip these effects on myself".
		//
		// Unlike every other action this one runs entirely inside the responder.
		// Attaching from the key press would apply the effect twice on the machine
		// that pressed the key (once locally, once in the responder) and would draw
		// from the synchronized random number generator on only one machine - AE
		// durations can be scaled by firepower and armour multipliers can roll a
		// chance - which is a genuine desynchronization.
		// ---------------------------------------------------------------------

		// True when the target's iron curtain state lets this effect through.
		// Mirrors the first test of AttachEffectClass::CreateAndAttach so that the
		// pre-check below and the actual application can never disagree.
		bool PenetratesIronCurtain(TechnoClass* pTechno, AttachEffectTypeClass* pType)
		{
			if (!pTechno->IsIronCurtained())
				return true;

			return pTechno->ForceShielded
				? pType->PenetratesForceShield.Get(pType->PenetratesIronCurtain)
				: pType->PenetratesIronCurtain;
		}

		// The attach half of the pre-check: would CreateAndAttach get past its own
		// filters for this effect? The same three tests, in the same order, so "yes"
		// here means the attach is not a no-op.
		bool WouldAttachEffectApply(TechnoClass* pTechno, AttachEffectTypeClass* pType)
		{
			if (!pType || !PenetratesIronCurtain(pTechno, pType))
				return false;

			if (!EnumFunctions::IsTechnoEligible(pTechno, pType->AffectsTarget, true))
				return false;

			auto const pTargetType = pTechno->GetTechnoType();

			if ((!pType->AffectTypes.empty() && !pType->AffectTypes.Contains(pTargetType))
				|| pType->IgnoreTypes.Contains(pTargetType))
			{
				return false;
			}

			return true;
		}

		// Local, deterministic pre-check for the AttachEffect action.
		//
		// It has to exist because Attach() cannot be used as a success test: for a
		// non-cumulative effect the unit already has, CreateAndAttach merely calls
		// RefreshDuration and returns nullptr, so Attach() reports 0 even though the
		// duration was in fact refreshed. The question is therefore not "how many
		// layers were created" but "would this call change anything at all".
		//
		// Everything below reads synchronized state only - no random numbers, no
		// local UI - so every machine reaches the same answer. That is what lets the
		// key press decide whether to queue the event, play the "not ready" feedback
		// and start the cooldown, without the responder having to report back.
		bool WillAttachEffectDoSomething(TechnoClass* pTechno, const AEAttachInfoTypeClass& info)
		{
			auto const pExt = TechnoExt::TryFetch(pTechno);

			if (!pExt)
				return false;

			for (auto const pType : info.AttachTypes)
			{
				if (WouldAttachEffectApply(pTechno, pType))
					return true;
			}

			// Stripping is unconditional as far as the source goes, but there still
			// has to be something to remove or the press would do nothing.
			if (!info.RemoveTypes.empty())
			{
				for (auto const& pAE : pExt->AttachedEffects)
				{
					if (!pAE)
						continue;

					for (auto const pType : info.RemoveTypes)
					{
						if (pType && pAE->GetType() == pType)
							return true;
					}
				}
			}

			if (!info.RemoveGroups.empty())
			{
				for (auto const& pAE : pExt->AttachedEffects)
				{
					auto const pType = pAE ? pAE->GetType() : nullptr;

					if (pType && pType->HasGroups(info.RemoveGroups, false))
						return true;
				}
			}

			return false;
		}

		// Queues the synchronized "apply this unit's AttachEffect payload" event.
		bool QueueAttachEffect(TechnoClass* pTechno)
		{
			auto const pHouse = pTechno->Owner;

			if (!pHouse)
				return false;

			EventExt event {};
			event.Type = EventTypeExt::SpecialActionAttachEffect;
			event.HouseIndex = static_cast<char>(pHouse->ArrayIndex);
			event.Frame = Unsorted::CurrentFrame;
			event.SpecialActionAttachEffect.Whom = TargetClass(pTechno);

			return event.AddEvent();
		}

		// The key press only validates and queues; the effects themselves are applied
		// on every machine by the event responder.
		bool ExecuteAttachEffect(TechnoClass* pTechno, const SpecialActionData& data)
		{
			if (!WillAttachEffectDoSomething(pTechno, data.AttachEffect))
			{
				// Nothing would change: the same feedback a press during the cooldown
				// gets, and no cooldown is consumed.
				Debug::Log("[SpecialAction] unit %d: AttachEffect ability did nothing (no effect would apply) - not ready feedback played\n",
					pTechno->Fetch_ID());

				PlayNotReadyFeedback(pTechno, data);
				return false;
			}

			if (!QueueAttachEffect(pTechno))
			{
				Debug::Log("[SpecialAction] unit %d: AttachEffect ability -> EVENT QUEUE REFUSED IT\n",
					pTechno->Fetch_ID());

				return false;
			}

			return true;
		}
	}

	SpecialActionData GetData(TechnoClass* pTechno)
	{
		if (!pTechno)
			return {};

		auto const pTypeExt = TechnoTypeExt::Fetch(pTechno->GetTechnoType());
		auto result = pTypeExt->SpecialAction;
		auto const& elite = pTypeExt->EliteSpecialAction;

		if (!pTechno->Veterancy.IsElite() || elite.Action == SpecialActionType::None)
			return result;

		// The elite entry replaces the action, but every value it does not set falls
		// back to the regular entry - so overriding only the cooldown, the voice or
		// the weapon no longer silently drops the rest.
		bool const sameAction = elite.Action == result.Action;
		result.Action = elite.Action;

		if (elite.ROF.isset())
			result.ROF = elite.ROF;

		if (elite.Voice.isset())
			result.Voice = elite.Voice;

		if (elite.Sound.isset())
			result.Sound = elite.Sound;

		if (elite.NotReadySound.isset())
			result.NotReadySound = elite.NotReadySound;

		if (elite.NotReadyMessage)
			result.NotReadyMessage = elite.NotReadyMessage;

		// A parameter belongs to the action that uses it, so it only carries over
		// while the action itself is unchanged; otherwise the elite entry has to
		// supply its own.
		if (elite.WeaponID)
			result.WeaponID = elite.WeaponID;
		else if (!sameAction)
			result.WeaponID = nullptr;

		if (elite.SuperWeaponID)
			result.SuperWeaponID = elite.SuperWeaponID;
		else if (!sameAction)
			result.SuperWeaponID = nullptr;

		if (elite.ConvertToID)
			result.ConvertToID = elite.ConvertToID;
		else if (!sameAction)
			result.ConvertToID = nullptr;

		if (elite.DeploysIntoID)
			result.DeploysIntoID = elite.DeploysIntoID;
		else if (!sameAction)
			result.DeploysIntoID = nullptr;

		if (elite.Aim.isset())
			result.Aim = elite.Aim;
		else if (!sameAction)
			result.Aim.Reset();

		if (elite.Source.isset())
			result.Source = elite.Source;
		else if (!sameAction)
			result.Source.Reset();

		return result;
	}

	bool IsOnCooldown(TechnoClass* pTechno)
	{
		if (!pTechno)
			return false;

		auto const pExt = TechnoExt::Fetch(pTechno);
		return pExt && pExt->SpecialActionTimer.HasTimeLeft();
	}

	int GetEffectiveROF(TechnoClass* pTechno)
	{
		if (!pTechno)
			return -1;

		auto const pTypeExt = TechnoTypeExt::Fetch(pTechno->GetTechnoType());
		auto const& elite = pTypeExt->EliteSpecialAction;
		auto const& base = pTypeExt->SpecialAction;

		// The elite block only wins while it actually names an action, which is the
		// same rule GetData uses; its ROF then wins per field.
		if (pTechno->Veterancy.IsElite() && elite.Action != SpecialActionType::None)
		{
			if (elite.ROF.isset())
				return elite.ROF.Get();

			if (elite.Action != base.Action)
				return -1;
		}

		return base.ROF.isset() ? base.ROF.Get() : -1;
	}

	void StartCooldown(TechnoClass* pTechno, const SpecialActionData& data)
	{
		if (!pTechno || !data.ROF.isset())
			return;

		auto const pExt = TechnoExt::Fetch(pTechno);

		if (!pExt)
			return;

		pExt->SpecialActionTimer.Start(data.ROF.Get());
		pExt->LastSpecialActionFrame = Unsorted::CurrentFrame;
	}

	void PlayNotReadyFeedback(TechnoClass* pTechno, const SpecialActionData& data)
	{
		if (!pTechno)
			return;

		// The type's own key wins; [AudioVisual] only supplies the default for
		// types that do not configure one.
		auto const pGlobal = RulesExt::Global();

		int sound = data.NotReadySound.isset()
			? data.NotReadySound.Get()
			: pGlobal->SpecialAction_NotReadySound.Get();

		if (sound >= 0)
			VocClass::PlayAt(sound, pTechno->GetCoords());

		const char* pMessage = nullptr;

		if (data.NotReadyMessage)
			pMessage = data.NotReadyMessage;
		else if (pGlobal->SpecialAction_NotReadyMessage)
			pMessage = pGlobal->SpecialAction_NotReadyMessage;

		if (pMessage)
		{
			if (auto const pText = StringTable::TryFetchString(pMessage, L""))
			{
				if (pText[0])
					MessageListClass::Instance.PrintMessage(pText, static_cast<double>(RulesClass::Instance->MessageDelay), HouseClass::CurrentPlayer->ColorSchemeIndex, true);
			}
		}
	}

	// Defined below; both are producers of the synchronized arm/disarm event.
	bool QueueArmedWeapon(TechnoClass* pTechno, int slot);

	void DisarmWeapon(TechnoClass* pTechno)
	{
		auto const pExt = TechnoExt::TryFetch(pTechno);

		// Nothing armed, so nothing to tell the other machines about.
		//
		// TechnoClass::ClickedEvent runs for every order (Deploy, Stop, Scatter) on
		// every selected unit, and it is itself the function that appends to
		// EventClass::OutList. Broadcasting from there unconditionally would spam
		// the outgoing queue on every order and could push real orders out of it.
		if (!pExt || pExt->SpecialActionWeaponIndex < 0)
			return;

		QueueArmedWeapon(pTechno, -1);
	}

	// Queues the synchronized "arm this unit with this slot" event; a negative
	// slot disarms. This is the only path that ever changes the armed state, so
	// every machine ends up with the same value and therefore picks the same
	// weapon. The event is executed on all machines (the sender included) by the
	// EventExt responder, which calls ApplyArmedWeapon.
	bool QueueArmedWeapon(TechnoClass* pTechno, int slot)
	{
		if (!pTechno || !pTechno->Owner)
			return false;

		EventExt event {};
		event.Type = EventTypeExt::SpecialActionWeapon;
		event.HouseIndex = static_cast<char>(pTechno->Owner->ArrayIndex);
		event.Frame = Unsorted::CurrentFrame;
		event.SpecialActionWeapon.Whom = TargetClass(pTechno);
		event.SpecialActionWeapon.WeaponSlot = slot;
		return event.AddEvent();
	}

	// Simulation-side clear: writes the fields directly.
	//
	// This must NOT queue an event. It is reached from the firing path, which runs
	// on every machine, and queueing there would send one event per machine - the
	// exact mistake the event design exists to avoid. Only the local producers
	// (ArmWeapon / DisarmWeapon) queue.
	void ClearArmedWeapon(TechnoExt* pExt)
	{
		if (!pExt)
			return;

		pExt->SpecialActionWeaponIndex = -1;
		pExt->SpecialActionBurstShotsLeft = 0;
	}

	void ApplyArmedWeapon(TechnoClass* pTechno, int slot)
	{
		auto const pExt = TechnoExt::TryFetch(pTechno);

		if (!pExt)
			return;

		if (slot < 0)
		{
			ClearArmedWeapon(pExt);
			Debug::Log("[SpecialAction] unit %d: disarmed\n", pTechno->Fetch_ID());
			return;
		}

		// Armed, not started: SpecialActionBurstShotsLeft stays 0 until a firing
		// cycle boundary picks the slot up in GetForcedWeaponSlot. That is what
		// makes an attack already in progress finish with the weapon it started
		// with, and the ability apply to the cycle after it.
		//
		// Re-arming while the ability's own cycle is running must not interrupt
		// it, so the remaining shot count is deliberately left alone here.
		pExt->SpecialActionWeaponIndex = slot;
		Debug::Log("[SpecialAction] unit %d: armed with weapon slot %d (%d shots left = %d)\n",
			pTechno->Fetch_ID(), slot, pExt->SpecialActionWeaponIndex, pExt->SpecialActionBurstShotsLeft);
	}

	void FireAttachedSuperWeapon(TechnoClass* pTechno, int superIndex, const CellStruct& cell)
	{
		if (!pTechno || !pTechno->Owner || superIndex < 0)
			return;

		auto const pHouse = pTechno->Owner;
		auto const pSuper = pHouse->Supers.GetItemOrDefault(superIndex);

		if (!pSuper || !pSuper->Type)
			return;

		// SuperClass::ClickFire (0x6CB920) has two entirely different admission paths,
		// and the single call below has to get past whichever one applies:
		//
		//   not a charge-drain type (0x6CB933 jumps to 0x6CBA7D):
		//     (RechargeTimer.StartTime != -1 && IsPresent && IsReady) || Type->PostClick
		//     A super weapon that was never granted has StartTime == -1, which is why
		//     the timer is not simply left alone.
		//
		//   charge-drain type (0x6CB939 onwards, never reaches 0x6CBA7D):
		//     branches on ChargeDrainState - Ready(1) turns into Draining(2) and
		//     launches, Draining(2) turns back into Ready(1) and returns 0, and
		//     anything else, including the Charging(0) of a never-granted instance,
		//     returns 0 without launching.
		//
		// Everything ClickFire can write is saved here and put back afterwards:
		//   IsReady          - cleared when the type is not PostClick (0x6CBB15)
		//   RechargeTimer    - restarted at the end of ClickFire (0x6CBC4E..0x6CBC83);
		//                      saved whole, because the engine also writes its cached
		//                      current frame, not just StartTime and TimeLeft
		//   CameoChargeState - set to -1 (0x6CBB80 and 0x6CBC4E)
		//   ChargeDrainState - set to Ready/Draining (0x6CB954, 0x6CB9D9) and back to
		//                      Charging (0x6CBC91)
		//
		// IsPresent is saved/restored even though the design keeps it false: a house
		// that legitimately owns the same super weapon must keep owning it.
		//
		// Deliberately NOT restored: whatever Launch itself did (animations, damage,
		// spawned objects), and the engine's own cleanup of the pre-click Animation.
		// Those are the launch, not charge bookkeeping.
		bool const wasPresent = pSuper->IsPresent;
		bool const wasReady = pSuper->IsReady;
		int const wasCameoChargeState = pSuper->CameoChargeState;
		ChargeDrainState const wasChargeDrainState = pSuper->ChargeDrainState;
		CDTimerClass const savedTimer = pSuper->RechargeTimer;

		pSuper->IsPresent = true;
		pSuper->SetReadiness(true);

		if (pSuper->RechargeTimer.StartTime == -1)
			pSuper->RechargeTimer.StartTime = Unsorted::CurrentFrame;

		// Required, not cosmetic: a charge-drain type never reaches ClickFire's
		// StartTime/IsPresent/IsReady test at all, and the Charging(0) of a
		// never-granted instance would make it return 0 without launching.
		if (pSuper->Type->UseChargeDrain)
			pSuper->ChargeDrainState = ChargeDrainState::Ready;

		// The engine's own launch path derives isPlayer the same way, per machine:
		// HouseClass::Fire_SW (0x4FAE8E) compares the firing house against
		// HouseClass::CurrentPlayer. Matching it keeps this path identical to a
		// sidebar launch instead of inventing a second convention.
		bool const fired = pSuper->ClickFire(pHouse->IsCurrentPlayer(), cell) != 0;

		pSuper->IsPresent = wasPresent;
		pSuper->IsReady = wasReady;
		pSuper->CameoChargeState = wasCameoChargeState;
		pSuper->ChargeDrainState = wasChargeDrainState;
		pSuper->RechargeTimer = savedTimer;

		Debug::Log("[SpecialAction] unit %d: attached super weapon '%s' launched=%d at (%d,%d)\n",
			pTechno->Fetch_ID(), pSuper->Type->ID ? pSuper->Type->ID : "?", fired ? 1 : 0,
			cell.X, cell.Y);
	}

	void ApplyAttachEffect(TechnoClass* pTechno)
	{
		if (!pTechno)
			return;

		auto const data = GetData(pTechno);

		// Defensive: the event is only ever queued by this action, but an elite
		// override resolved on another machine must not turn this into something
		// else. AttachEffect has no elite variant, so in practice this is the base
		// entry either way.
		if (data.Action != SpecialActionType::AttachEffect)
			return;

		auto const& info = data.AttachEffect;

		// Warhead order: attach, then strip by type, then strip by group. Keeping it
		// identical means a payload moved over from a warhead behaves the same here,
		// including the "listed in both lists - attached and then removed again"
		// outcome.
		//
		// The source is deliberately nullptr rather than the unit itself. Attach()
		// treats pSource == pTarget as "self-owned", and a self-owned effect is
		// permanent (it ignores Duration and recreates itself); this action releases
		// a timed effect once. Passing nullptr makes the ability the source, which is
		// exactly what it is.
		int const attached = AttachEffectClass::Attach(pTechno, pTechno->Owner, pTechno, nullptr, info);
		int const detached = AttachEffectClass::Detach(pTechno, info);
		int const detachedByGroup = AttachEffectClass::DetachByGroups(pTechno, info);

		// Started here rather than in Execute: the responder runs on every machine in
		// the same synchronized frame, so every machine's cooldown begins on the same
		// frame. This is the only place this action starts its cooldown.
		StartCooldown(pTechno, data);

		Debug::Log("[SpecialAction] unit %d: AttachEffect ability: attached %d, detached %d, detached by group %d\n",
			pTechno->Fetch_ID(), attached, detached, detachedByGroup);
	}

	bool ArmWeapon(TechnoClass* pTechno, const SpecialActionData& data)
	{
		// Validate on the machine that pressed the key so a bad INI entry costs no
		// event; the slot itself is then broadcast to every machine.
		const int slot = ResolveWeaponSlot(pTechno, data);

		if (slot < 0 || !TechnoExt::TryFetch(pTechno))
		{
			auto const pType = pTechno ? pTechno->GetTechnoType() : nullptr;

			// List the slots the type actually has, so a rejected value can be
			// corrected from the log without another round trip.
			char slotList[0x100] = "";
			int usableSlots = 0;

			if (pType)
			{
				for (int i = 0; i < TechnoTypeClass::MaxWeapons; ++i)
				{
					auto pWeapon = pType->GetWeapon(i, false).WeaponType;

					if (!pWeapon)
						pWeapon = pType->GetWeapon(i, true).WeaponType;

					if (!pWeapon)
						continue;

					++usableSlots;

					const size_t length = strlen(slotList);
					_snprintf_s(slotList + length, sizeof(slotList) - length, _TRUNCATE, "%s%d=%s",
						length ? ", " : "", i, pWeapon->ID ? pWeapon->ID : "?");
				}
			}

			Debug::Log("[SpecialAction] unit %d: cannot arm - weapon '%s' matches none of the %d usable slot(s) of type '%s' [%s]\n",
				pTechno ? pTechno->Fetch_ID() : -1,
				data.WeaponID ? static_cast<const char*>(data.WeaponID) : "<unset>",
				usableSlots,
				pType && pType->ID ? pType->ID : "?",
				slotList);

			return false;
		}

		// False when the outgoing event queue refused it, so the caller reports the
		// ability as not executed rather than silently arming nothing.
		const bool queued = QueueArmedWeapon(pTechno, slot);

		Debug::Log("[SpecialAction] unit %d: arm request for weapon slot %d -> %s\n",
			pTechno->Fetch_ID(), slot, queued ? "queued" : "EVENT QUEUE REFUSED IT");

		return queued;
	}

	// The forced weapon slot while the ability's firing cycle is running, or -1
	// for "no override". Read by the force-weapon hook.
	int GetForcedWeaponSlot(TechnoClass* pTechno)
	{
		auto const pExt = TechnoExt::TryFetch(pTechno);

		if (!pExt || pExt->SpecialActionWeaponIndex < 0)
			return -1;

		if (pExt->SpecialActionBurstShotsLeft <= 0)
		{
			// An attack that is already running must finish with the weapon it started
			// with, so the ability waits for the next cycle. "Running" means a burst
			// index that has not wrapped yet AND an attack actually in progress: a
			// stale index left behind by an interrupted burst (target gone, order
			// cancelled) must not block the ability.
			if (pTechno->CurrentBurstIndex != 0
				&& pTechno->Target
				&& pTechno->CurrentMission == Mission::Attack)
			{
				Debug::Log("[SpecialAction] unit %d: armed with slot %d but mid-burst (index %d), deferring to the next cycle\n",
					pTechno->Fetch_ID(), pExt->SpecialActionWeaponIndex, pTechno->CurrentBurstIndex);

				return -1;
			}

			auto const pType = pTechno->GetTechnoType();

			if (!pType || pExt->SpecialActionWeaponIndex >= TechnoTypeClass::MaxWeapons)
			{
				Debug::Log("[SpecialAction] unit %d: armed slot %d no longer exists, clearing\n",
					pTechno->Fetch_ID(), pExt->SpecialActionWeaponIndex);

				ClearArmedWeapon(pExt);
				return -1;
			}

			// The Burst count has to come from the weapon the engine will actually
			// fire, and an elite unit fires its EliteWeapon array. Fall back to the
			// other array so a type that only fills one of them is not rejected.
			const bool elite = pTechno->Veterancy.IsElite();
			auto pWeapon = pType->GetWeapon(pExt->SpecialActionWeaponIndex, elite).WeaponType;

			if (!pWeapon)
				pWeapon = pType->GetWeapon(pExt->SpecialActionWeaponIndex, !elite).WeaponType;

			if (!pWeapon)
			{
				Debug::Log("[SpecialAction] unit %d: armed slot %d holds no weapon, clearing\n",
					pTechno->Fetch_ID(), pExt->SpecialActionWeaponIndex);

				ClearArmedWeapon(pExt);
				return -1;
			}

			// The override lasts exactly one firing cycle, including every burst
			// shot of it; NotifyWeaponCycleFinished then drops it again.
			pExt->SpecialActionBurstShotsLeft = Math::max(pWeapon->Burst, 1);

			Debug::Log("[SpecialAction] unit %d: ACTIVATED weapon slot %d for this cycle (%d shots)\n",
				pTechno->Fetch_ID(), pExt->SpecialActionWeaponIndex, pExt->SpecialActionBurstShotsLeft);
		}

		return pExt->SpecialActionWeaponIndex;
	}

	void NotifyWeaponCycleFinished(TechnoClass* pTechno)
	{
		auto const pExt = TechnoExt::TryFetch(pTechno);

		if (!pExt)
			return;

		ClearArmedWeapon(pExt);

		// Charged only now that the ability has actually fired. Arming it and then
		// cancelling it therefore costs nothing.
		StartCooldown(pTechno, GetData(pTechno));

		Debug::Log("[SpecialAction] unit %d: ability burst finished, weapon slot released and cooldown started\n",
			pTechno->Fetch_ID());
	}

	void NotifyOrderIssued(TechnoClass* pTechno, Mission mission)
	{
		// Any order other than the ability's own unload retires the unload marker, so
		// an abandoned unload mission cannot make the *next* deploy press unload
		// instead. (The marker is also dropped when the passenger phase completes.)
		if (mission != Mission::Unload)
		{
			if (auto const pExt = TechnoExt::TryFetch(pTechno))
				pExt->SpecialActionUnloading = false;
		}

		// Only orders that clearly send the unit off to do something else cancel the
		// pending ability. The combat missions are deliberately absent: a player's
		// attack order is precisely the attack the ability is waiting for, and an
		// object click also dispatches Area_Guard and friends, so a "everything
		// except Attack cancels" rule would silently drop the ability on the very
		// click it is meant to apply to. Cancelling too rarely only leaves the unit
		// armed, which is harmless; cancelling too eagerly breaks the feature.
		switch (mission)
		{
		case Mission::Move:
		case Mission::QMove:
		case Mission::Stop:
		case Mission::Guard:
		case Mission::Sleep:
		case Mission::Enter:
		case Mission::Capture:
		case Mission::Harvest:
		case Mission::Return:
		case Mission::Unload:
		case Mission::Sabotage:
		case Mission::Patrol:
		case Mission::Selling:
		case Mission::Repair:
		case Mission::Rescue:
			break;

		default:
			return;
		}

		auto const pExt = TechnoExt::TryFetch(pTechno);

		if (!pExt || pExt->SpecialActionWeaponIndex < 0)
			return;

		// Another order replaces the pending ability.
		Debug::Log("[SpecialAction] unit %d: pending ability cancelled by mission %d\n",
			pTechno->Fetch_ID(), static_cast<int>(mission));

		DisarmWeapon(pTechno);
	}

	// Key-press acknowledgement, the same idea as the deploy voice: the unit answers
	// when the ability is triggered.
	//
	// Resolution order, the first entry that is configured wins:
	//   1. SpecialAction.Voice        (the type's own unit voice)
	//   2. SpecialAction.Sound        (a sound played at the unit)
	//   3. VoiceSpecialAttack         (the type's vanilla special-attack voice)
	//   4. [AudioVisual] SpecialAction.Sound   (the global default)
	//   5. silence
	//
	// Everything above the global default is the type's own entry, so a unit that
	// configures anything at all never falls through to it.
	void PlayActionFeedback(TechnoClass* pTechno, const SpecialActionData& data)
	{
		// One line per key press. A group order runs Execute once per selected unit,
		// and the engine's deploy voice fix suppresses the group case for the same
		// reason: N stacked voices are worse than none.
		static int LastFeedbackFrame = -1;

		if (Unsorted::CurrentFrame == LastFeedbackFrame)
			return;

		int voice = -1;
		int sound = -1;

		if (data.Voice.isset())
			voice = data.Voice.Get();
		else if (data.Sound.isset())
			sound = data.Sound.Get();
		else if (auto const pType = pTechno->GetTechnoType(); pType && pType->VoiceSpecialAttack.Count > 0)
			voice = pType->VoiceSpecialAttack[0];
		else
			sound = RulesExt::Global()->SpecialAction_Sound.Get();

		if (voice < 0 && sound < 0)
			return;

		LastFeedbackFrame = Unsorted::CurrentFrame;

		if (voice >= 0)
			pTechno->QueueVoice(voice);
		else
			VocClass::PlayAt(sound, pTechno->GetCoords());

		Debug::Log("[SpecialAction] unit %d: feedback %s %d\n",
			pTechno->Fetch_ID(), voice >= 0 ? "voice" : "sound", voice >= 0 ? voice : sound);
	}

	// =====================================================================
	// Return action (AdvancedAircraftMissions)
	// =====================================================================

	bool HasReturnAction(TechnoClass* pTechno)
	{
		return pTechno && GetData(pTechno).Action == SpecialActionType::Return;
	}

	// Queues the synchronized "this unit returns to base now" event. The state
	// change itself is done by EventExt::RespondSpecialActionReturn on every
	// machine, which is also where the cooldown is charged.
	bool QueueReturnToBase(TechnoClass* pTechno, bool cancel)
	{
		if (!pTechno || !pTechno->Owner)
			return false;

		EventExt event {};
		event.Type = EventTypeExt::SpecialActionReturn;
		event.HouseIndex = static_cast<char>(pTechno->Owner->ArrayIndex);
		event.Frame = Unsorted::CurrentFrame;
		event.SpecialActionReturn.Whom = TargetClass(pTechno);
		event.SpecialActionReturn.Cancel = cancel;

		return event.AddEvent();
	}

	void StartReturnCooldown(TechnoClass* pTechno)
	{
		if (!pTechno)
			return;

		// Charged on every machine by the event responder so that both triggers -
		// the hotkey and the right-click on the unit's own airfield - agree on the
		// frame the cooldown began. Same reasoning as AttachEffect.
		StartCooldown(pTechno, GetData(pTechno));
	}

	// The key press only validates and queues; the return itself is executed by the
	// event responder on every machine.
	bool ExecuteReturn(TechnoClass* pTechno, const SpecialActionData& data)
	{
		auto const pAircraft = abstract_cast<AircraftClass*, true>(pTechno);

		if (!pAircraft || !AdvancedMissions::CanOrderReturn(pAircraft))
		{
			Debug::Log("[SpecialAction] unit %d: Return ability did nothing (not eligible)\n",
				pTechno->Fetch_ID());

			PlayNotReadyFeedback(pTechno, data);
			return false;
		}

		// Already on the way home: do not restart the window - that would let the
		// player refresh the boost forever - and report "not ready" instead.
		if (AircraftExt::Fetch(pAircraft)->ReturnOrderActive)
		{
			PlayNotReadyFeedback(pTechno, data);
			return false;
		}

		if (!QueueReturnToBase(pTechno))
		{
			Debug::Log("[SpecialAction] unit %d: Return ability -> EVENT QUEUE REFUSED IT\n",
				pTechno->Fetch_ID());

			return false;
		}

		return true;
	}

	bool Execute(TechnoClass* pTechno)
	{
		if (!pTechno || !IsOwnedByLocalPlayer(pTechno))
			return false;

		auto const data = GetData(pTechno);

		if (data.Action == SpecialActionType::None)
			return false;

		if (IsOnCooldown(pTechno))
		{
			PlayNotReadyFeedback(pTechno, data);
			return false;
		}

		bool executed = false;

		switch (data.Action)
		{
		case SpecialActionType::Deploy:
			executed = ExecuteDeploy(pTechno);
			break;

		case SpecialActionType::DeploysInto:
			executed = ExecuteDeploysInto(pTechno, data);
			break;

		case SpecialActionType::Convert:
			executed = ExecuteConvert(pTechno, data);
			break;

		case SpecialActionType::DeployFire:
			executed = ExecuteDeployFire(pTechno, data);
			break;

		case SpecialActionType::Unload:
			executed = ExecuteUnload(pTechno);
			break;

		case SpecialActionType::Weapon:
			executed = ArmWeapon(pTechno, data);
			break;

		case SpecialActionType::SuperWeapon:
			executed = ExecuteSuperWeapon(pTechno, data);
			break;

		case SpecialActionType::AttachEffect:
			executed = ExecuteAttachEffect(pTechno, data);
			break;

		case SpecialActionType::Return:
			executed = ExecuteReturn(pTechno, data);
			break;

		default:
			break;
		}

		if (executed)
		{
			// Audible confirmation on the key press, like the deploy voice.
			PlayActionFeedback(pTechno, data);

			// The Weapon action is charged when its weapon actually fires, not when it
			// is armed - see NotifyWeaponCycleFinished.
			//
			// AttachEffect is charged by its event responder instead, on every machine
			// at once, so that the cooldown starts on the same frame everywhere. See
			// ApplyAttachEffect.
			//
			// Return is charged by its own responder for the same reason: the
			// right-click trigger has to spend the very same cooldown, and it runs
			// through the same event. See StartReturnCooldown.
			if (data.Action != SpecialActionType::Weapon
				&& data.Action != SpecialActionType::AttachEffect
				&& data.Action != SpecialActionType::Return)
				StartCooldown(pTechno, data);
		}

		return executed;
	}
}

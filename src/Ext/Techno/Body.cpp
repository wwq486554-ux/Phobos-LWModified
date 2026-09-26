#include <Ext/Aircraft/Body.h>
#include <Ext/Anim/Body.h>
#include <Ext/Building/Body.h>
#include <Ext/BuildingType/Body.h>
#include <Ext/House/Body.h>
#include <Ext/Infantry/Body.h>
#include <Ext/InfantryType/Body.h>
#include <Ext/Unit/Body.h>
#include <Ext/Scenario/Body.h>
#include <Ext/WeaponType/Body.h>
#include <Ext/Event/Body.h>
#include <Ext/Bullet/Trajectories/EngraveLine.h>

#include <MapClass.h>
#include <CellClass.h>

#include <Utilities/AresFunctions.h>
#include <Utilities/AresHelper.h>
#include <Interop/TechnoExt.h>

TechnoExt::~TechnoExt()
{
	auto const pTypeExt = this->TypeExtData;
	auto const pType = pTypeExt->OwnerObject();
	auto const pThis = this->OwnerObject();
	// Besides BuildingClass, calling pThis->WhatAmI() here will only result in AbstractType::None
	auto const whatAmI = pType->WhatAmI();

	if (pTypeExt->AutoDeath_Behavior.isset())
	{
		auto& vec = ScenarioExt::Global()->AutoDeathObjects;
		vec.erase(std::remove(vec.begin(), vec.end(), this), vec.end());
	}

	if (whatAmI != AbstractType::AircraftType && whatAmI != AbstractType::BuildingType
		&& pType->Ammo > 0 && pTypeExt->ReloadInTransport.Get(RulesExt::Global()->ReloadInTransport))
	{
		auto& vec = ScenarioExt::Global()->TransportReloaders;
		vec.erase(std::remove(vec.begin(), vec.end(), this), vec.end());
	}

	if (this->IsSelected)
	{
		auto& vec = ScenarioExt::Global()->LimboLaunchers;
		vec.erase(std::remove(vec.begin(), vec.end(), this), vec.end());
	}

	if (this->AnimRefCount > 0)
		AnimExt::InvalidateTechnoPointers(pThis);

	if (pTypeExt->Harvester_Counted)
	{
		auto& vec = HouseExt::Fetch(pThis->Owner)->OwnedCountedHarvesters;
		vec.erase(std::remove(vec.begin(), vec.end(), pThis), vec.end());
	}

	for (auto const pBolt : this->ElectricBolts)
	{
		pBolt->Owner = nullptr;
	}

	this->ElectricBolts.clear();

	if (this->SpecialTracked)
		ScenarioExt::Global()->SpecialTracker.Remove(pThis);

	if (this->FallingDownTracked)
		ScenarioExt::Global()->FallingDownTracker.Remove(pThis);
}

bool TechnoExt::IsActive(TechnoClass* pThis)
{
	return pThis
		&& pThis->IsAlive
		&& pThis->Health > 0
		&& !pThis->InLimbo
		&& !pThis->TemporalTargetingMe
		&& !pThis->BeingWarpedOut
		&& !pThis->Deactivated
		&& !pThis->IsUnderEMP()
		;
}

bool TechnoExt::IsHarvesting(TechnoClass* pThis)
{
	if (!TechnoExt::IsActive(pThis))
		return false;

	auto const pSlaveManager = pThis->SlaveManager;

	if (pSlaveManager && pSlaveManager->State != SlaveManagerStatus::Ready)
		return true;

	if (pThis->WhatAmI() == AbstractType::Building)
		return pThis->IsPowerOnline();

	if (TechnoExt::HasAvailableDock(pThis))
	{
		switch (pThis->GetCurrentMission())
		{
		case Mission::Harvest:
			if (auto const pUnit = abstract_cast<UnitClass*, true>(pThis))
			{
				if (pUnit->HasAnyLink() && !TechnoExt::HasRadioLinkWithDock(pUnit)) // Probably still in factory.
					return false;

				if (pUnit->IsUseless) // Harvesters currently sitting without purpose are idle even if they are on harvest mission.
					return false;
			}
			return true;
		case Mission::Unload:
			return true;
		case Mission::Enter:
			if (pThis->HasAnyLink())
			{
				auto const pLink = pThis->GetNthLink(0);

				if (pLink->WhatAmI() != AbstractType::Building) // Enter mission + non-building link = not trying to unload
					return false;
			}
			return true;
		case Mission::Guard:
			if (auto const pUnit = abstract_cast<UnitClass*, true>(pThis))
			{
				if (pUnit->ArchiveTarget && pUnit->GetStoragePercentage() > 0.0 && pUnit->Locomotor->Is_Moving()) // Edge-case, waiting to be able to unload.
					return true;
			}
			return false;
		default:
			return false;
		}
	}

	return false;
}

bool TechnoExt::HasAvailableDock(TechnoClass* pThis)
{
	for (auto const pBld : pThis->GetTechnoType()->Dock)
	{
		if (pThis->Owner->CountOwnedAndPresent(pBld))
			return true;
	}

	return false;
}

bool TechnoExt::HasRadioLinkWithDock(TechnoClass* pThis)
{
	if (pThis->HasAnyLink())
	{
		auto const pLink = abstract_cast<BuildingClass*, true>(pThis->GetNthLink(0));

		if (pLink && pThis->GetTechnoType()->Dock.FindItemIndex(pLink->Type) >= 0)
			return true;
	}

	return false;
}

// Syncs Iron Curtain or Force Shield timer to another techno.
void TechnoExt::SyncInvulnerability(TechnoClass* pFrom, TechnoClass* pTo)
{
	if (pFrom->IsIronCurtained())
	{
		const auto pTypeExt = TechnoExt::Fetch(pFrom)->TypeExtData;
		const bool isForceShielded = pFrom->ForceShielded;
		const bool allowSyncing = !isForceShielded
			? pTypeExt->IronCurtain_KeptOnDeploy.Get(RulesExt::Global()->IronCurtain_KeptOnDeploy)
			: pTypeExt->ForceShield_KeptOnDeploy.Get(RulesExt::Global()->ForceShield_KeptOnDeploy);

		if (allowSyncing)
		{
			pTo->IronCurtainTimer = pFrom->IronCurtainTimer;
			pTo->IronTintStage = pFrom->IronTintStage;
			pTo->ForceShielded = isForceShielded;
		}
	}
}

bool TechnoExt::HasAdditionalAbility(TechnoClass* pThis, AdditionalAbility ability)
{
	if (!pThis || (!pThis->Veterancy.IsVeteran() && !pThis->Veterancy.IsElite()))
		return false;

	const auto index = static_cast<size_t>(ability);
	const auto pTypeExt = TechnoExt::Fetch(pThis)->TypeExtData;

	if (pThis->Veterancy.IsElite())
	{
		return pTypeExt->AdditionalVeteranAbilities.test(index)
			|| pTypeExt->AdditionalEliteAbilities.test(index);
	}

	return pTypeExt->AdditionalVeteranAbilities.test(index);
}

double TechnoExt::GetCurrentSpeedMultiplier(FootClass* pThis)
{
	double houseMultiplier = 1.0;
	auto const whatAmI = pThis->WhatAmI();

	if (whatAmI == AbstractType::Aircraft)
		houseMultiplier = pThis->Owner->Type->SpeedAircraftMult;
	else if (whatAmI == AbstractType::Infantry)
		houseMultiplier = pThis->Owner->Type->SpeedInfantryMult;
	else
		houseMultiplier = pThis->Owner->Type->SpeedUnitsMult;

	return pThis->SpeedMultiplier * houseMultiplier * TechnoExt::Fetch(pThis)->AE.SpeedMultiplier *
		(pThis->HasAbility(Ability::Faster) ? RulesClass::Instance->VeteranSpeed : 1.0);
}

double TechnoExt::GetCurrentFirepowerMultiplier(TechnoClass* pThis)
{
	double mult = pThis->FirepowerMultiplier * pThis->Owner->FirepowerMultiplier * TechnoExt::Fetch(pThis)->AE.FirepowerMultiplier *
		(pThis->HasAbility(Ability::Firepower) ? RulesClass::Instance->VeteranCombat : 1.0);

	if (const auto pBuilding = abstract_cast<BuildingClass*, true>(pThis))
	{
		const auto pBuildingType = pBuilding->Type;

		if (pBuildingType->CanBeOccupied && pBuildingType->CanOccupyFire && pBuildingType->MaxNumberOccupants)
		{
			const auto pBuildingTypeExt = BuildingTypeExt::Fetch(pBuildingType);
			mult *= pBuildingTypeExt->BuildingOccupyDamageMult.Get(RulesClass::Instance->OccupyDamageMultiplier);
		}
	}
	else if (const auto pBunker = abstract_cast<BuildingClass*>(pThis->BunkerLinkedItem))
	{
		const auto pBunkerTypeExt = BuildingTypeExt::Fetch(pBunker->Type);
		mult *= pBunkerTypeExt->BuildingBunkerDamageMult.Get(RulesClass::Instance->BunkerDamageMultiplier);
	}
	else if (pThis->InOpenToppedTransport && pThis->Transporter)
	{
		const auto pTransporterTypeExt = TechnoExt::Fetch(pThis->Transporter)->TypeExtData;
		mult *= pTransporterTypeExt->OpenTopped_DamageMultiplier.Get(RulesClass::Instance->OpenToppedDamageMultiplier);
		mult *= TechnoExt::Fetch(pThis)->TypeExtData->OpenTransport_DamageMultiplier.Get(RulesExt::Global()->OpenTransport_DamageMultiplier);
	}

	return mult;
}

double TechnoExt::GetCurrentArmorMultiplier(TechnoClass* pThis, TechnoTypeClass* pType, HouseClass* pSourceHouse, WarheadTypeClass* pWarhead)
{
	return pThis->ArmorMultiplier * pThis->Owner->GetArmorMultiplier(pType) * TechnoExt::CalculateArmorMultipliers(pThis, pWarhead, pSourceHouse) *
		(pThis->HasAbility(Ability::Stronger) ? RulesClass::Instance->VeteranArmor : 1.0);
}

CoordStruct TechnoExt::PassengerKickOutLocation(TechnoClass* pThis, FootClass* pPassenger, int maxAttempts = 1)
{
	if (!pThis || !pPassenger)
		return CoordStruct::Empty;

	if (maxAttempts < 1)
		maxAttempts = 1;

	const auto pTypePassenger = pPassenger->GetTechnoType();
	auto placeCoords = CellStruct::Empty;
	short extraDistance = 1;
	auto speedType = pTypePassenger->SpeedType;
	auto movementZone = pTypePassenger->MovementZone;

	if (pTypePassenger->WhatAmI() == AbstractType::AircraftType)
	{
		speedType = SpeedType::Track;
		movementZone = MovementZone::Normal;
	}
	do
	{
		placeCoords = pThis->GetMapCoords() - CellStruct { static_cast<short>(extraDistance / 2), static_cast<short>(extraDistance / 2) };
		placeCoords = MapClass::Instance.NearByLocation(placeCoords, speedType, -1, movementZone, false, extraDistance, extraDistance, true, false, false, false, CellStruct::Empty, false, false);

		if (placeCoords == CellStruct::Empty)
			return CoordStruct::Empty;

		const auto pCell = MapClass::Instance.GetCellAt(placeCoords);

		if (pThis->IsCellOccupied(pCell, FacingType::None, -1, nullptr, false) == Move::OK)
			break;

		extraDistance++;
	}
	while (extraDistance <= maxAttempts);

	if (const auto pCell = MapClass::Instance.TryGetCellAt(placeCoords))
		return pCell->GetCoordsWithBridge();

	return CoordStruct::Empty;
}

bool TechnoExt::AllowedTargetByZone(TechnoClass* pThis, TechnoClass* pTarget, TargetZoneScanType zoneScanType, WeaponTypeClass* pWeapon, bool useZone, int zone)
{
	if (!pThis || !pTarget)
		return false;

	if (pThis->WhatAmI() == AbstractType::Aircraft)
		return true;

	auto const pType = pThis->GetTechnoType();
	auto const mZone = pType->MovementZone;
	const int currentZone = useZone ? zone : MapClass::Instance.GetMovementZoneType(pThis->GetMapCoords(), mZone, pThis->OnBridge);

	if (currentZone != -1)
	{
		if (zoneScanType == TargetZoneScanType::Any)
			return true;

		const int targetZone = MapClass::Instance.GetMovementZoneType(pTarget->GetMapCoords(), mZone, pTarget->OnBridge);

		if (zoneScanType == TargetZoneScanType::Same)
		{
			if (currentZone != targetZone)
				return false;
		}
		else
		{
			if (currentZone == targetZone)
				return true;

			auto const speedType = pType->SpeedType;
			auto const cellStruct = MapClass::Instance.NearByLocation(CellClass::Coord2Cell(pTarget->Location),
				speedType, -1, mZone, false, 1, 1, true,
				false, false, speedType != SpeedType::Float, CellStruct::Empty, false, false);

			if (cellStruct == CellStruct::Empty)
				return false;

			auto const pCell = MapClass::Instance.TryGetCellAt(cellStruct);

			if (!pCell)
				return false;

			if (!pWeapon)
			{
				const int weaponIndex = pThis->SelectWeapon(pTarget);

				if (weaponIndex < 0)
					return false;

				pWeapon = pThis->GetWeapon(weaponIndex)->WeaponType;
			}

			const double distanceSq = pCell->GetCoordsWithBridge().DistanceFromSquared(pTarget->GetCenterCoords());
			const double range = (double)pWeapon->Range;

			if (distanceSq > range * range)
				return false;
		}
	}

	return true;
}

// Feature for common usage : TechnoType conversion -- Trsdy
// BTW, who said it was merely a Type pointer replacement and he could make a better one than Ares?
bool TechnoExt::ConvertToType(FootClass* pThis, TechnoTypeClass* pToType)
{
	const auto pType = pThis->GetTechnoType();

	// It really should be at the beginning.
	if (pType == pToType || pType->WhatAmI() != pToType->WhatAmI())
	{
		Debug::Log("Incompatible types between %s and %s\n", pThis->get_ID(), pToType->get_ID());
		return false;
	}

	if (AresFunctions::ConvertTypeTo)
	{
		if (AresFunctions::ConvertTypeTo(pThis, pToType))
		{
			FootExt::Fetch(pThis)->UpdateTypeData(pToType);
			return true;
		}

		return false;
	}

	// In case not using Ares 3.0. Only update necessary vanilla properties
	AbstractType rtti;
	TechnoTypeClass** nowTypePtr;

	// Different types prohibited
	switch (pThis->WhatAmI())
	{
	case AbstractType::Infantry:
		nowTypePtr = reinterpret_cast<TechnoTypeClass**>(&(static_cast<InfantryClass*>(pThis)->Type));
		rtti = AbstractType::InfantryType;
		break;
	case AbstractType::Unit:
		nowTypePtr = reinterpret_cast<TechnoTypeClass**>(&(static_cast<UnitClass*>(pThis)->Type));
		rtti = AbstractType::UnitType;
		break;
	case AbstractType::Aircraft:
		nowTypePtr = reinterpret_cast<TechnoTypeClass**>(&(static_cast<AircraftClass*>(pThis)->Type));
		rtti = AbstractType::AircraftType;
		break;
	default:
		Debug::Log("%s is not FootClass, conversion not allowed\n", pToType->get_ID());
		return false;
	}

	// Detach CLEG targeting
	auto const tempUsing = pThis->TemporalImUsing;
	if (tempUsing && tempUsing->Target)
		tempUsing->LetGo();

	auto const pOwner = pThis->Owner;

	// Remove tracking of old techno
	if (!pThis->InLimbo)
		pOwner->RegisterLoss(pThis, false);
	pOwner->RemoveTracking(pThis);

	const int oldHealth = pThis->Health;

	// Generic type-conversion
	auto const prevType = *nowTypePtr;
	*nowTypePtr = pToType;

	// Readjust health according to percentage
	pThis->SetHealthPercentage((double)(oldHealth) / (double)prevType->Strength);
	pThis->EstimatedHealth = pThis->Health;

	// Add tracking of new techno
	pOwner->AddTracking(pThis);
	if (!pThis->InLimbo)
		pOwner->RegisterGain(pThis, false);
	pOwner->RecheckTechTree = true;

	// Update Ares AttachEffects -- skipped
	// Ares RecalculateStats -- skipped

	// Adjust ammo
	const int originalAmmo = pThis->Ammo;
	const int maxAmmo = pToType->Ammo;
	pThis->Ammo = Math::min(originalAmmo, maxAmmo);

	if (originalAmmo > maxAmmo)
		pThis->Mark(MarkType::Change);

	// Ares ResetSpotlights -- skipped

	// Adjust ROT
	if (rtti == AbstractType::AircraftType)
		pThis->SecondaryFacing.SetROT(pToType->ROT);
	else
		pThis->PrimaryFacing.SetROT(pToType->ROT);
	// Adjust Ares TurretROT -- skipped
	//  pThis->SecondaryFacing.SetROT(TechnoTypeExt::Fetch(pToType)->TurretROT.Get(pToType->ROT));

	// Locomotor change, referenced from Ares 0.A's abduction code, not sure if correct, untested
	CLSID nowLocoID;
	ILocomotion* iloco = pThis->Locomotor;
	const auto& toLoco = pToType->Locomotor;
	if ((SUCCEEDED(static_cast<LocomotionClass*>(iloco)->GetClassID(&nowLocoID)) && nowLocoID != toLoco))
	{
		// because we are throwing away the locomotor in a split second, piggybacking
		// has to be stopped. otherwise the object might remain in a weird state.
		while (LocomotionClass::End_Piggyback(pThis->Locomotor));
		// throw away the current locomotor and instantiate
		// a new one of the default type for this unit.
		if (auto const newLoco = LocomotionClass::CreateInstance(toLoco))
		{
			newLoco->Link_To_Object(pThis);
			pThis->Locomotor = std::move(newLoco);
		}
	}

	const auto& jjLoco = LocomotionClass::CLSIDs::Jumpjet;
	if (pToType->BalloonHover && pToType->DeployToLand && prevType->Locomotor != jjLoco && toLoco == jjLoco)
		pThis->Locomotor->Move_To(pThis->Location);

	FootExt::Fetch(pThis)->UpdateTypeData(pToType);
	return true;
}

bool TechnoExt::IsTypeImmune(TechnoClass* pThis, TechnoClass* pSource)
{
	if (!pThis || !pSource)
		return false;

	auto const pType = pThis->GetTechnoType();

	if (!pType->TypeImmune)
		return false;

	if (pType == pSource->GetTechnoType() && pThis->Owner == pSource->Owner)
		return true;

	return false;
}

/// <summary>
/// Gets whether or not techno has listed AttachEffect types active on it
/// </summary>
/// <param name="attachEffectTypes">Attacheffect types.</param>
/// <param name="requireAll">Whether or not to require all listed types to be present or if only one will satisfy the check.</param>
/// <param name="ignoreSameSource">Ignore AttachEffects that come from set invoker and source.</param>
/// <param name="pInvoker">Invoker Techno used for same source check.</param>
/// <param name="pSource">Source AbstractClass instance used for same source check.</param>
/// <returns>True if techno has active AttachEffects that satisfy the source, false if not.</returns>
bool TechnoExt::HasAttachedEffects(std::vector<AttachEffectTypeClass*> attachEffectTypes, bool requireAll, bool ignoreSameSource,
	TechnoClass* pInvoker, AbstractClass* pSource, std::vector<int> const* minCounts, std::vector<int> const* maxCounts) const
{
	unsigned int foundCount = 0;
	unsigned int typeCounter = 1;
	const bool checkSource = ignoreSameSource && pInvoker && pSource;

	for (auto const& type : attachEffectTypes)
	{
		if (type->Cumulative)
		{
			const int cumulativeCount = this->GetAttachedEffectCumulativeCount(type, ignoreSameSource, pInvoker, pSource);
			bool matched = cumulativeCount > 0;
			const unsigned int minSize = minCounts ? minCounts->size() : 0;
			const unsigned int maxSize = maxCounts ? maxCounts->size() : 0;

			if (matched && minSize > 0)
			{
				if (cumulativeCount < minCounts->at(typeCounter - 1 >= minSize ? minSize - 1 : typeCounter - 1))
					matched = false;
			}

			if (matched && maxSize > 0)
			{
				if (cumulativeCount > maxCounts->at(typeCounter - 1 >= maxSize ? maxSize - 1 : typeCounter - 1))
					matched = false;
			}

			if (matched)
			{
				// Only need to find one match, can stop here.
				if (!requireAll)
					return true;

				foundCount++;
			}
		}
		else
		{
			for (auto const& attachEffect : this->AttachedEffects)
			{
				if (attachEffect->GetType() == type && attachEffect->IsActive())
				{
					if (checkSource && attachEffect->IsFromSource(pInvoker, pSource))
						continue;

					// Only need to find one match, can stop here.
					if (!requireAll)
						return true;

					foundCount++;
					break;
				}
			}
		}

		// One of the required types was not found, can stop here.
		if (requireAll && foundCount < typeCounter)
			return false;

		typeCounter++;
	}

	if (requireAll && foundCount == attachEffectTypes.size())
		return true;

	return false;
}

/// <summary>
/// Gets how many counts of same cumulative AttachEffect type instance techno has active on it.
/// </summary>
/// <param name="pAttachEffectType">AttachEffect type.</param>
/// <param name="ignoreSameSource">Ignore AttachEffects that come from set invoker and source.</param>
/// <param name="pInvoker">Invoker Techno used for same source check.</param>
/// <param name="pSource">Source AbstractClass instance used for same source check.</param>
/// <returns>Number of active cumulative AttachEffect type instances on the techno. 0 if the AttachEffect type is not cumulative.</returns>
int TechnoExt::GetAttachedEffectCumulativeCount(AttachEffectTypeClass* pAttachEffectType, bool ignoreSameSource, TechnoClass* pInvoker, AbstractClass* pSource) const
{
	unsigned int foundCount = 0;
	const bool checkSource = ignoreSameSource && pInvoker && pSource;

	for (auto const& attachEffect : this->AttachedEffects)
	{
		if (attachEffect->GetType() == pAttachEffectType && attachEffect->IsActive())
		{
			if (checkSource && attachEffect->IsFromSource(pInvoker, pSource))
				continue;

			foundCount++;
		}
	}

	return foundCount;
}

// Check adjacent cells from the center
// The current MapClass::Instance.PlacePowerupCrate(...) doesn't like slopes and maybe other cases
bool TechnoExt::TryToCreateCrate(CoordStruct location, Powerup selectedPowerup, int maxCellRange)
{
	CellStruct centerCell = CellClass::Coord2Cell(location);
	short currentRange = 0;
	bool placed = false;

	do
	{
		short x = -currentRange;
		short y = -currentRange;

		CellStruct checkedCell;
		checkedCell.Y = centerCell.Y + y;

		// Check upper line
		for (short i = -currentRange; i <= currentRange; i++)
		{
			checkedCell.X = centerCell.X + i;
			placed = MapClass::Instance.PlacePowerupCrate(checkedCell, selectedPowerup);

			if (placed)
				break;
		}

		if (placed)
			break;

		checkedCell.Y = centerCell.Y + (short)std::abs(y);

		// Check lower line
		for (short i = -currentRange; i <= currentRange; i++)
		{
			checkedCell.X = centerCell.X + i;
			placed = MapClass::Instance.PlacePowerupCrate(checkedCell, selectedPowerup);

			if (placed)
				break;
		}

		if (placed)
			break;

		checkedCell.X = centerCell.X + x;

		// Check left line
		for (short j = -currentRange + 1; j < currentRange; j++)
		{
			checkedCell.Y = centerCell.Y + j;
			placed = MapClass::Instance.PlacePowerupCrate(checkedCell, selectedPowerup);

			if (placed)
				break;
		}

		if (placed)
			break;

		checkedCell.X = centerCell.X + (short)std::abs(x);

		// Check right line
		for (short j = -currentRange + 1; j < currentRange; j++)
		{
			checkedCell.Y = centerCell.Y + j;
			placed = MapClass::Instance.PlacePowerupCrate(checkedCell, selectedPowerup);

			if (placed)
				break;
		}

		currentRange++;
	}
	while (!placed && currentRange < (short)maxCellRange);

	if (!placed)
		Debug::Log(__FUNCTION__": Failed to place a crate in the cell (%d,%d) and around that location.\n", centerCell.X, centerCell.Y, maxCellRange);

	return placed;
}

void TechnoExt::ResetDelayedFireTimer()
{
	this->DelayedFireTimer.Stop();
	this->DelayedFireWeaponIndex = -1;
	this->DelayedFireSequencePaused = false;

	if (this->CurrentDelayedFireAnim)
	{
		if (AnimExt::Fetch(this->CurrentDelayedFireAnim)->DelayedFireRemoveOnNoDelay)
			this->CurrentDelayedFireAnim->UnInit();
	}
}

void TechnoExt::CreateDelayedFireAnim(TechnoClass* pThis, AnimTypeClass* pAnimType, int weaponIndex, bool attach, bool center, bool removeOnNoDelay, bool onTurret, CoordStruct firingCoords)
{
	if (pAnimType)
	{
		CoordStruct coords;

		if (center)
			coords = pThis->GetCenterCoords();
		else
			coords = TechnoExt::GetFLHAbsoluteCoords(pThis, firingCoords, onTurret);

		auto const pAnim = GameCreate<AnimClass>(pAnimType, coords);

		if (attach)
			pAnim->SetOwnerObject(pThis);

		auto const pAnimExt = AnimExt::Fetch(pAnim);
		pAnim->Owner = pThis->Owner;
		pAnimExt->SetInvoker(pThis);

		if (attach)
		{
			pAnimExt->DelayedFireRemoveOnNoDelay = removeOnNoDelay;
			TechnoExt::Fetch(pThis)->CurrentDelayedFireAnim = pAnim;
		}
	}
}

bool TechnoExt::HandleDelayedFireWithPauseSequence(TechnoClass* pThis, WeaponTypeClass* pWeapon, int weaponIndex, int frame, int firingFrame)
{
	auto const pExt = TechnoExt::Fetch(pThis);
	auto& timer = pExt->DelayedFireTimer;
	auto const pWeaponExt = WeaponTypeExt::Fetch(pWeapon);

	if (pExt->DelayedFireWeaponIndex >= 0 && pExt->DelayedFireWeaponIndex != weaponIndex)
	{
		pExt->ResetDelayedFireTimer();
		pExt->DelayedFireSequencePaused = false;
	}

	if (pWeaponExt->DelayedFire_PauseFiringSequence && pWeaponExt->DelayedFire_Duration.isset() && (!pThis->Transporter || !pWeaponExt->DelayedFire_SkipInTransport))
	{
		if (pWeapon->Burst <= 1 || !pWeaponExt->DelayedFire_OnlyOnInitialBurst || pThis->CurrentBurstIndex == 0)
		{
			if (frame == firingFrame)
				pExt->DelayedFireSequencePaused = true;

			if (!timer.HasStarted())
			{
				pExt->DelayedFireWeaponIndex = weaponIndex;
				timer.Start(Math::max(GeneralUtils::GetRangedRandomOrSingleValue(pWeaponExt->DelayedFire_Duration), 0));
				auto pAnimType = pWeaponExt->DelayedFire_Animation;

				if (pThis->Transporter && pWeaponExt->DelayedFire_OpenToppedAnimation.isset())
					pAnimType = pWeaponExt->DelayedFire_OpenToppedAnimation;

				auto firingCoords = pThis->GetWeapon(weaponIndex)->FLH;

				if (pWeaponExt->DelayedFire_AnimOffset.isset())
					firingCoords = pWeaponExt->DelayedFire_AnimOffset;

				TechnoExt::CreateDelayedFireAnim(pThis, pAnimType, weaponIndex, pWeaponExt->DelayedFire_AnimIsAttached, pWeaponExt->DelayedFire_CenterAnimOnFirer,
					pWeaponExt->DelayedFire_RemoveAnimOnNoDelay, pWeaponExt->DelayedFire_AnimOnTurret, firingCoords);

				return true;
			}
			else if (timer.InProgress())
			{
				return true;
			}

			if (timer.Completed())
				pExt->ResetDelayedFireTimer();
		}

		pExt->DelayedFireSequencePaused = false;
	}

	return false;
}

bool TechnoExt::IsHealthInThreshold(TechnoClass* pObject, double min, double max)
{
	if (!pObject->Health && !pObject->GetType()->Strength)
		return true;

	const double hp = pObject->GetHealthPercentage();
	return (hp > 0 ? hp > min : hp >= min) && hp <= max;
}

void TechnoExt::ClickedApproachObject(FootClass* pThis, ObjectClass* pObject)
{
	if (Unsorted::MoveFeedback)
		pThis->VoiceMove();

	EventExt event {};
	event.Type = EventTypeExt::ApproachObject;
	event.HouseIndex = static_cast<char>(pThis->Owner->ArrayIndex);
	event.Frame = Unsorted::CurrentFrame;
	event.ApproachObject.Whom = TargetClass(pThis);
	event.ApproachObject.Target = TargetClass(pObject);
	event.AddEvent();
}

bool TechnoExt::CanBeRecruitedFix(FootClass* pThis, HouseClass* pHouse)
{
    if (pThis->Team != nullptr ||
        !pThis->IsAlive ||
        pThis->Health <= 0 ||
        pThis->InLimbo ||
        pThis->Owner != pHouse)
    {
        return false;
    }

    if (!(pThis->RecruitableA && pThis->RecruitableB))
    {
        return false;
    }

    const Mission mission = pThis->GetCurrentMission();
    if (!MissionClass::IsRecruitableMission(mission))
    {
        return false;
    }

    if (pThis->ShouldEnterAbsorber ||
        pThis->ShouldEnterOccupiable ||
        pThis->ShouldGarrisonStructure ||
        pThis->DrainTarget != nullptr ||
        pThis->BunkerLinkedItem ||
        pThis->LocomotorSource != nullptr)
    {
        return false;
    }

    return true;
}

bool TechnoExt::EjectRandomly(FootClass* pEjectee, const CoordStruct& coords, int distance, bool select)
{
	std::vector<CoordStruct> usableCoords;

	for (int direction = 0; direction < 8; ++direction)
	{
		const CellStruct tmpCoords = Unsorted::AdjacentCell[direction];
		CoordStruct ejectCoords { coords.X + tmpCoords.X * distance, coords.Y + tmpCoords.Y * distance, coords.Z };
		const auto pCell = MapClass::Instance.TryGetCellAt(ejectCoords);

		if (!pCell)
			continue;

		const auto occupied = pEjectee->IsCellOccupied(pCell, FacingType::None, -1, nullptr, true);

		if (occupied != Move::OK && occupied != Move::MovingBlock)
			continue;

		if (pEjectee->WhatAmI() == InfantryClass::AbsID)
		{
			ejectCoords = pCell->FindInfantrySubposition(ejectCoords, false, false, false);

			// Jan 31, 2026 - Starkku: FindInfantrySubposition has several code paths that return empty CoordStruct. We should ignore those.
			if (ejectCoords == CoordStruct::Empty)
				continue;

			ejectCoords.Z = coords.Z;
		}
		else
		{
			ejectCoords = CellClass::Cell2Coord(pCell->MapCoords, coords.Z);
		}

		usableCoords.emplace_back(ejectCoords);
	}

	const int count = static_cast<int>(usableCoords.size());

	if (!count)
		return false;

	return TechnoExt::EjectSurvivor(pEjectee, usableCoords[ScenarioClass::Instance->Random(0, count - 1)], select);
}

bool TechnoExt::EjectSurvivor(FootClass* pSurvivor, CoordStruct coords, bool select)
{
	const auto pCell = MapClass::Instance.GetCellAt(coords);

	pSurvivor->OnBridge = pCell->ContainsBridge();

	const int floorZ = pCell->GetCoordsWithBridge().Z;
	const bool chuted = (coords.Z - floorZ > 2 * Unsorted::LevelHeight);

	if (chuted)
	{
		pSurvivor->Limbo();

		++Unsorted::ScenarioInit;
		const bool result = pSurvivor->SpawnParachuted(coords);
		--Unsorted::ScenarioInit;

		if (!result)
			return false;
	}
	else
	{
		coords.Z = floorZ;

		++Unsorted::ScenarioInit;
		const bool result = pSurvivor->Unlimbo(coords, static_cast<DirType>(ScenarioClass::Instance->Random(0, 7)));
		--Unsorted::ScenarioInit;

		if (!result)
			return false;
	}

	if (const auto pTransporter = pSurvivor->Transporter)
	{
		if (pTransporter->GetTechnoType()->OpenTopped)
			pTransporter->ExitedOpenTopped(pSurvivor);

		pSurvivor->Transporter = nullptr;
	}

	pSurvivor->LastMapCoords = pCell->MapCoords;

	if (chuted)
	{
		const bool scat = pSurvivor->OnBridge;
		const auto occupation = scat ? pCell->AltOccupationFlags : pCell->OccupationFlags;

		if (occupation & 0x1C)
			pCell->ScatterContent(CoordStruct::Empty, true, true, scat);
	}
	else
	{
		pSurvivor->Scatter(CoordStruct::Empty, true, false);
		pSurvivor->QueueMission(pSurvivor->Owner->IsControlledByHuman() ? Mission::Guard : Mission::Hunt, 0);
	}

	pSurvivor->ShouldEnterOccupiable = false;
	pSurvivor->ShouldGarrisonStructure = false;

	if (select)
		pSurvivor->Select();

	return true;
}

struct DummyExtHere
{
	char _pad0[0x50];
	CDTimerClass DisableWeaponsTimer;
	char _pad1[0x40];
	bool DriverKilled; 
};

struct DummyTypeExtHere
{
	char _[0xF4];
	ValueableVector<TechnoTypeClass*> Operators;
	bool Operator_Any;
};

bool __fastcall TechnoExt::ApplyKillDriver(TechnoClass** pData, void*, HouseClass* pToHouse, TechnoClass* pKiller, bool resetVeterancy)
{
	const auto pThis = abstract_cast<FootClass*, true>(*pData);

	if (!pThis)
		return false;

	const bool passive = pToHouse->IsNeutral();
	const auto pExt_Ares = reinterpret_cast<DummyExtHere*>(pThis->align_154);
	pExt_Ares->DriverKilled = passive;

	if (pThis->Owner == pToHouse)
		return false;

	const auto pType = pThis->GetTechnoType();
	const auto pTypeExt_Ares = reinterpret_cast<DummyTypeExtHere*>(pType->align_2FC);
	auto& passengers = pThis->Passengers;

	do
	{
		if (!passengers.GetFirstPassenger())
			break;

		if (pTypeExt_Ares->Operator_Any)
		{
			const auto pOperator = pThis->RemoveFirstPassenger();
			pOperator->RegisterDestruction(pKiller);
			pOperator->UnInit();
		}
		else if (!pTypeExt_Ares->Operators.empty())
		{
			for (NextObject passenger(passengers.GetFirstPassenger()); passenger; ++passenger)
			{
				if (!pTypeExt_Ares->Operators.Contains(passenger->GetTechnoType()))
					continue;

				const auto pOperator = static_cast<FootClass*>(*passenger);
				passengers.RemovePassenger(pOperator);

				if (pType->Gunner && !passengers.NumPassengers)
					pThis->RemoveGunner(pOperator);

				pOperator->RegisterDestruction(pKiller);
				pOperator->UnInit();
				break;
			}
		}

		const auto pTypeExt = TechnoTypeExt::Fetch(pType);

		if (passive && pTypeExt->DriverKilled_KeptPassengers.Get(RulesExt::Global()->DriverKilled_KeptPassengers))
			break;

		const bool kill = pTypeExt->DriverKilled_KillPassengers.Get(RulesExt::Global()->DriverKilled_KillPassengers);

		while (auto pPassenger = passengers.GetFirstPassenger())
		{
			const auto pNextPassenger = abstract_cast<FootClass*>(pPassenger->NextObject);
			passengers.RemovePassenger(pPassenger);

			if (pType->Gunner && !passengers.NumPassengers)
				pThis->RemoveGunner(pPassenger);

			if (kill || !TechnoExt::EjectRandomly(pPassenger, pThis->Location, 128, false))
			{
				pPassenger->RegisterDestruction(nullptr);
				pPassenger->UnInit();
			}
			else if (pType->OpenTopped)
			{
				pThis->ExitedOpenTopped(pPassenger);
			}

			pPassenger = pNextPassenger;
		}
	}
	while (false);

	pThis->HijackerInfantryType = -1;

	if (resetVeterancy)
		pThis->Veterancy.SetRookie(false);

	if (const auto pControlledBy = pThis->MindControlledBy)
	{
		if (const auto pManager = pControlledBy->CaptureManager)
			pManager->FreeUnit(pThis);
	}

	pThis->MindControlledByAUnit = false;
	pThis->MindControlledByHouse = nullptr;

	if (const auto pRingAnim = pThis->MindControlRingAnim)
	{
		pRingAnim->UnInit();
		pThis->MindControlRingAnim = nullptr;
	}

	if (const auto pTeam = pThis->Team)
		pTeam->LiberateMember(pThis);

	if (const auto pManager = pThis->CaptureManager)
		pManager->FreeAll();

	if (const auto pManager = pThis->SpawnManager)
	{
		pManager->KillNodes();
		pManager->ResetTarget();
	}

	if (const auto pManager = pThis->SlaveManager)
	{
		pManager->Killed(pKiller);
		pManager->AllGuard();
		pManager->Owner = pThis;

		if (passive)
			pManager->SuspendWork();
		else
			pManager->ResumeWork();
	}

	pThis->SetOwningHouse(pToHouse);

	if (passive)
		pThis->QueueMission(Mission::Harmless, true);

	pThis->SetTarget(nullptr);
	pThis->SetDestination(nullptr, false);

	auto pTag = pThis->AttachedTag;

	if (pTag)
		pTag->RaiseEvent(static_cast<TriggerEvent>(0x44), pThis, CellStruct::Empty, false, pKiller);

	pTag = pThis->AttachedTag;

	if (pTag && pThis->IsAlive)
		pTag->RaiseEvent(static_cast<TriggerEvent>(0x43), pThis, CellStruct::Empty);

	return true;
}

int TechnoExt::GetSight()
{
	double sight = this->TypeExtData->OwnerObject()->Sight;

	for (auto& callback : TechnoExtInterop::CalculateSightCallbacks)
	{
		if (callback)
			sight = callback(this->OwnerObject(), sight);
	}

	return static_cast<int>(sight);
}

bool TechnoExt::CanReceiveEvent(TechnoClass* pThis, HouseClass* pHouse)
{
	if (pThis->Berzerk)
		return false;

	if (pThis->GetTechnoType()->Spawned)
		return false;

	if (pThis->SlaveOwner)
		return false;

	auto const pOwner = pThis->GetOwningHouse();

	if (pOwner != pHouse && !(pHouse->IsCurrentPlayer() && pOwner->IsControlledByCurrentPlayer()))
		return false;

	return true;
}

bool TechnoExt::HasWeaponsDisabled(TechnoClass* pThis)
{
	if (TechnoExt::Fetch(pThis)->AE.DisableWeapons)
		return true;

	if (AresHelper::CanUseAres)
	{
		const auto pExt_Ares = reinterpret_cast<DummyExtHere*>(pThis->align_154);

		if (pExt_Ares->DisableWeaponsTimer.InProgress())
			return true;
	}

	return false;
}

FireError TechnoExt::GetFireErrorIgnoreDisableWeapons(TechnoClass* pThis, AbstractClass* pTarget, int weaponIndex, bool ignoreRange)
{
	auto const pExt = TechnoExt::Fetch(pThis);
	auto const pExt_Ares = reinterpret_cast<DummyExtHere*>(pThis->align_154);
	bool const canUseAres = AresHelper::CanUseAres;
	bool const disableWeapons = pExt->AE.DisableWeapons;
	int timeLeft = 0;

	pExt->AE.DisableWeapons = false;

	if (canUseAres)
	{
		timeLeft = pExt_Ares->DisableWeaponsTimer.GetTimeLeft();
		pExt_Ares->DisableWeaponsTimer.Stop();
	}

	auto const fireError = pThis->GetFireError(pTarget, weaponIndex, ignoreRange);
	pExt->AE.DisableWeapons = disableWeapons;

	if (canUseAres && timeLeft > 0)
		pExt_Ares->DisableWeaponsTimer.Start(timeLeft);

	return fireError;
}

// =============================
// SweepFire

namespace SweepFireDiag
{
	int Remaining = 60;
	int HookEntryRemaining = 6;

	bool On()
	{
		return Enabled && WeaponTypeExt::SweepFireProbeRequested();
	}

	bool Allowed()
	{
		if (!On() || Remaining <= 0)
			return false;

		--Remaining;
		return true;
	}

	bool HookEntryAllowed()
	{
		if (!On() || HookEntryRemaining <= 0)
			return false;

		--HookEntryRemaining;
		return true;
	}

	namespace
	{
		struct SeenKey
		{
			int Site;
			const char* pWeaponId;
		};

		SeenKey SeenKeys[32];
		int SeenKeyCount = 0;
	}

	bool FirstTime(int site, const char* pWeaponId)
	{
		if (!On())
			return false;

		for (int i = 0; i < SeenKeyCount; ++i)
		{
			if (SeenKeys[i].Site == site && SeenKeys[i].pWeaponId == pWeaponId)
				return false;
		}

		if (SeenKeyCount < 32)
		{
			SeenKeys[SeenKeyCount].Site = site;
			SeenKeys[SeenKeyCount].pWeaponId = pWeaponId;
			++SeenKeyCount;
		}

		return true;
	}
}

namespace
{
	// Anchor of a sweep: the point the virtual line offsets are resolved against. A building
	// exposes its own target coordinate, everything else its centre.
	CoordStruct GetSweepAnchor(AbstractClass* pTarget)
	{
		const auto pBuilding = abstract_cast<BuildingClass*, true>(pTarget);

		return pBuilding ? pBuilding->GetTargetCoords() : pTarget->GetCenterCoords();
	}
}

CellClass* TechnoExt::TryStartSweepFire(WeaponTypeClass* pWeapon, int weaponIndex, AbstractClass* pTarget, int burstIndex, CoordStruct* pLineStartOut)
{
	const auto pWeaponExt = WeaponTypeExt::TryFetch(pWeapon);

	// Weapons that did not opt in stay completely silent, and this runs for every
	// shot of every weapon.
	if (!pWeaponExt || !pWeaponExt->SweepFire_Enable)
		return nullptr;

	if (!pWeaponExt->IsSweepFireEnabled())
	{
		if (SweepFireDiag::Allowed())
			Debug::Log("[SweepFire] %s: no sweep (weapon type cannot sweep: IsSonic / DiskLaser)\n", pWeapon->ID);

		return nullptr;
	}

	if (!pTarget)
	{
		if (SweepFireDiag::Allowed())
			Debug::Log("[SweepFire] %s: no sweep (no target)\n", pWeapon->ID);

		return nullptr;
	}

	if (!pWeapon->Projectile)
	{
		if (SweepFireDiag::Allowed())
			Debug::Log("[SweepFire] %s: no sweep (weapon has no Projectile)\n", pWeapon->ID);

		return nullptr;
	}

	// O-10, second half: when this shot is aimed at something other than the
	// target the running sweep belongs to, drop that sweep right here instead of
	// waiting for the next per-frame update. This closes the one-frame window in
	// which the old sweep would still occupy the container and make the new
	// trigger bail out with "another sweep is still running".
	if (!this->Sweeps.empty())
	{
		const DWORD targetID = pTarget->UniqueID;

		if (std::any_of(this->Sweeps.begin(), this->Sweeps.end(),
			[targetID](const SweepFireInstance& item) { return item.AnchorTargetID != 0 && item.AnchorTargetID != targetID; }))
		{
			this->AbortSweepFire(true);
		}
	}

	// A fresh trigger needs an idle techno whose cooldown has passed. The
	// follow-up shots of the same Burst always get their sweep, even though the
	// first one of them is still running (D-15).
	if (burstIndex <= 0 && !this->Sweeps.empty())
	{
		if (SweepFireDiag::Allowed())
			Debug::Log("[SweepFire] %s: no sweep (another sweep is still running, %d active)\n", pWeapon->ID, static_cast<int>(this->Sweeps.size()));

		return nullptr;
	}

	if (burstIndex <= 0 && !this->SweepFireCooldownTimer.Expired())
	{
		if (SweepFireDiag::Allowed())
			Debug::Log("[SweepFire] %s: no sweep (cooldown still has %d frames)\n", pWeapon->ID, this->SweepFireCooldownTimer.GetTimeLeft());

		return nullptr;
	}

	const auto pThis = this->OwnerObject();
	const CoordStruct anchor = GetSweepAnchor(pTarget);

	const auto& sourceOffset = pWeaponExt->SweepFire_SourceCoord.Get();
	const auto& targetOffset = pWeaponExt->SweepFire_TargetCoord.Get();
	Point2D virtualSource { sourceOffset.X, sourceOffset.Y };
	Point2D virtualTarget { targetOffset.X, targetOffset.Y };

	// The line is described relative to the target. With the default 0,0
	// coordinates both ends sit on the target, which leaves nothing to sweep;
	// the weapon then keeps firing its ordinary single shot.
	if (!EngraveLine::HasOffset(virtualSource) && !EngraveLine::HasOffset(virtualTarget))
	{
		if (SweepFireDiag::Allowed())
			Debug::Log("[SweepFire] %s: no sweep (SourceCoord and TargetCoord are both 0,0)\n", pWeapon->ID);

		return nullptr;
	}

	const double speed = pWeaponExt->SweepFire_Speed;

	if (speed <= 0.0)
	{
		if (SweepFireDiag::Allowed())
			Debug::Log("[SweepFire] %s: no sweep (Speed is %.1f, must be greater than 0)\n", pWeapon->ID, speed);

		return nullptr;
	}

	// Burst shots alternate the side the sweep starts from, mirroring engrave.
	const bool mirrored = pWeaponExt->SweepFire_MirrorCoord && (burstIndex % 2) != 0;

	if (mirrored)
		EngraveLine::MirrorVirtualCoord(virtualSource, virtualTarget);

	const double rotateRadian = EngraveLine::GetRotateRadian(pThis->GetCoords(), anchor);
	const CoordStruct lineSource = EngraveLine::AddVirtualOffset(anchor, virtualSource, rotateRadian);
	const CoordStruct lineTarget = EngraveLine::AddVirtualOffset(anchor, virtualTarget, rotateRadian);
	const double lineLength = BulletExt::Get2DDistance(lineSource, lineTarget);

	if (lineLength < BulletExt::Epsilon)
	{
		if (SweepFireDiag::Allowed())
			Debug::Log("[SweepFire] %s: no sweep (line length is %f)\n", pWeapon->ID, lineLength);

		return nullptr;
	}

	// The first shot is aimed at the exact start of the line. The projectile's own scatter is
	// applied to the *launch solution* by the record hook instead (once the engine has created
	// the bullet), never to the aim point: the aim point is the coordinate this shot was told to
	// fire at, and TargetCoords is what decides where it goes off. Scattering the aim point made
	// the sweep spread its impacts by the full BallisticScatter while a normal shot of the same
	// projectile stays inside the target's cell - measured as a clear difference between a
	// sweeping unit and a non-sweeping one carrying the same weapon.
	CoordStruct firstAim = lineSource;
	auto pAimCell = MapClass::Instance.TryGetCellAt(firstAim);

	if (!pAimCell)
	{
		// A scattered point may leave the map even though the line start did not; the shot
		// is still valid, so fall back to aiming at the unscattered start.
		firstAim = lineSource;
		pAimCell = MapClass::Instance.TryGetCellAt(lineSource);

		if (!pAimCell)
		{
			if (SweepFireDiag::Allowed())
				Debug::Log("[SweepFire] %s: no sweep (line start %d,%d,%d is off the map)\n", pWeapon->ID, lineSource.X, lineSource.Y, lineSource.Z);

			return nullptr;
		}
	}

	// Interval between two shots of the sweep (D-30). This is deliberately the
	// weapon's own ROF and not RearmDelay's return value, which is tiered by Burst
	// position and would hand out the 3-5 frame burst delay.
	const int shotInterval = Math::max(1, static_cast<int>(pWeapon->ROF * this->AE.ROFMultiplier));
	// Frames the aim point needs to walk the whole line.
	const int sweepFrames = Math::max(1, static_cast<int>(std::ceil(lineLength / speed)));

	if (SweepFireDiag::Allowed())
	{
		Debug::Log("[SweepFire] %s: sweep started burst=%d slot=%d length=%d frames=%d interval=%d mirror=%d source=(%d,%d,%d) target=(%d,%d,%d)\n",
			pWeapon->ID, burstIndex, weaponIndex, static_cast<int>(lineLength), sweepFrames, shotInterval, mirrored ? 1 : 0,
			lineSource.X, lineSource.Y, lineSource.Z, lineTarget.X, lineTarget.Y, lineTarget.Z);
	}

	SweepFireInstance sweep {};
	sweep.WeaponIndex = weaponIndex;
	sweep.BurstIndex = burstIndex;
	sweep.RotateRadian = rotateRadian;
	sweep.Mirrored = mirrored;
	sweep.LineSource = lineSource;
	sweep.LineTarget = lineTarget;
	sweep.AnchorCoord = anchor;
	sweep.AnchorTargetID = pTarget->UniqueID;
	sweep.ShotTimer.Start(shotInterval);
	sweep.SweepTimer.Start(sweepFrames);

	// The same firing cycle may be reported more than once; keying by Burst index
	// keeps the container from growing.
	const auto it = std::find_if(this->Sweeps.begin(), this->Sweeps.end(),
		[burstIndex](const SweepFireInstance& item) { return item.BurstIndex == burstIndex; });

	if (it != this->Sweeps.end())
		*it = sweep;
	else
		this->Sweeps.push_back(sweep);

	// The point the caller should aim the first shot at, once the engine has launched that
	// bullet (see the record hook). It mirrors AimMode exactly, which is what makes the mode
	// mean the same thing for the first shot as for the follow-ups:
	//   coord / auto -> the exact (scattered) start of the line;
	//   cell         -> the centre of the cell under it, i.e. the old gridded aim.
	// It must always be filled in: leaving it at 0,0,0 (as it used to) would hand the first
	// shot a target of the map corner.
	if (pLineStartOut)
		*pLineStartOut = (pWeaponExt->SweepFire_AimMode == SweepFireAimMode::Cell) ? pAimCell->GetCoords() : firstAim;

	return pAimCell;
}

// Re-resolves the world-space segment of a sweep from its virtual offsets. Two optional keys feed
// this, both carrying engrave semantics:
//   SweepFire.AttachToTarget: the anchor follows the target, so the whole line slides with it.
//   SweepFire.UpdateDirection: the orientation follows the firer, so the line turns around the
//     anchor when the firer moves or turns.
// Neither can change the line's length: one translates the segment, the other rotates it around
// the anchor. That is what keeps the sweep's frame schedule - and therefore the aim point's
// distance along the line - valid without restarting the timers.
void TechnoExt::RefreshSweepLine(SweepFireInstance& sweep, WeaponTypeClass* pWeapon)
{
	const auto pWeaponExt = WeaponTypeExt::TryFetch(pWeapon);

	if (!pWeaponExt)
		return;

	const bool attachToTarget = pWeaponExt->SweepFire_AttachToTarget;
	const bool updateDirection = pWeaponExt->SweepFire_UpdateDirection;

	if (!attachToTarget && !updateDirection)
		return;

	const auto pThis = this->OwnerObject();

	if (attachToTarget)
	{
		// A target that let go or died leaves the last anchor in place, so the line is still
		// swept out to its end exactly as D-7 requires. The per-frame update above already
		// aborted the sweep if the firer switched to a different target, so a live Target here
		// is the one this sweep belongs to.
		if (const auto pTarget = pThis->Target)
			sweep.AnchorCoord = GetSweepAnchor(pTarget);
	}

	if (updateDirection)
		sweep.RotateRadian = EngraveLine::GetRotateRadian(pThis->GetCoords(), sweep.AnchorCoord);

	const auto& sourceOffset = pWeaponExt->SweepFire_SourceCoord.Get();
	const auto& targetOffset = pWeaponExt->SweepFire_TargetCoord.Get();
	Point2D virtualSource { sourceOffset.X, sourceOffset.Y };
	Point2D virtualTarget { targetOffset.X, targetOffset.Y };

	if (sweep.Mirrored)
		EngraveLine::MirrorVirtualCoord(virtualSource, virtualTarget);

	const CoordStruct previousSource = sweep.LineSource;
	const CoordStruct previousTarget = sweep.LineTarget;

	sweep.LineSource = EngraveLine::AddVirtualOffset(sweep.AnchorCoord, virtualSource, sweep.RotateRadian);
	sweep.LineTarget = EngraveLine::AddVirtualOffset(sweep.AnchorCoord, virtualTarget, sweep.RotateRadian);

	// Only report a line that actually moved, so the trace stays readable and its budget is
	// spent on the frames that show the follow-up shots taking a new line into account.
	if (SweepFireDiag::On() && (!(sweep.LineSource == previousSource) || !(sweep.LineTarget == previousTarget)))
	{
		static int lineLines = 0;

		if (lineLines < 40)
		{
			++lineLines;
			Debug::Log("[SweepFire/line] w=[%s] attach=%d updir=%d anchor=(%d,%d,%d) src=(%d,%d,%d) tgt=(%d,%d,%d) rot=%.4f\n",
				pWeapon->ID, attachToTarget ? 1 : 0, updateDirection ? 1 : 0,
				sweep.AnchorCoord.X, sweep.AnchorCoord.Y, sweep.AnchorCoord.Z,
				sweep.LineSource.X, sweep.LineSource.Y, sweep.LineSource.Z,
				sweep.LineTarget.X, sweep.LineTarget.Y, sweep.LineTarget.Z, sweep.RotateRadian);
		}
	}
}

void TechnoExt::UpdateSweepFire()
{
	// Diagnostic: proves the per-frame TechnoExt hook runs at all, so a missing
	// Fire_At line can be told apart from "Phobos hooks do not run here".
	if (SweepFireDiag::On())
	{
		static bool loggedAlive = false;

		if (!loggedAlive)
		{
			loggedAlive = true;
			Debug::Log("[SweepFire] runtime alive: TechnoExt::OnEarlyUpdate reached\n");
		}
	}

	// Most technos never sweep. This runs every frame, so it goes first.
	if (this->Sweeps.empty())
		return;

	const auto pThis = this->OwnerObject();

	// A sweep cannot outlive its firer. The target dying is fine: the line keeps
	// its last coordinates and is swept to the end (D-7).
	if (!pThis->IsAlive || pThis->InLimbo || pThis->IsSinking || pThis->Health <= 0 || pThis->IsUnderEMP())
	{
		this->Sweeps.clear();
		return;
	}

	// Infantry stop firing when they are told to move, but the follow-up shots are
	// created by hand and never go through Fire_At / GetFireError, so nothing else
	// would stop them: the soldier would keep spraying from a fixed line while
	// walking away. Vehicles *can* fire on the move, so this rule is deliberately
	// infantry-only; a blanket "moving cancels" would break vehicle sweeps.
	if (const auto pInf = abstract_cast<InfantryClass*>(pThis))
	{
		if (!pInf->Locomotor || pInf->Locomotor->Is_Moving())
		{
			if (SweepFireDiag::Allowed())
				Debug::Log("[SweepFire] %s: sweep aborted (the infantry is moving)\n", pThis->get_ID());

			this->AbortSweepFire(true);
			return;
		}
	}

	// O-10: an explicit new target cancels the running sweep and lets the next
	// trigger start one immediately. A target that died leaves Target null (or
	// unchanged), and the line is swept to the end instead (D-7).
	if (const auto pTarget = pThis->Target)
	{
		const DWORD targetID = pTarget->UniqueID;

		for (const auto& sweep : this->Sweeps)
		{
			if (sweep.AnchorTargetID != 0 && sweep.AnchorTargetID != targetID)
			{
				if (SweepFireDiag::Allowed())
					Debug::Log("[SweepFire] %s: sweep aborted (the target changed, %u -> %u)\n", pThis->get_ID(), sweep.AnchorTargetID, targetID);

				this->AbortSweepFire(true);
				return;
			}
		}
	}

	int cooldown = -1;

	for (auto it = this->Sweeps.begin(); it != this->Sweeps.end(); )
	{
		auto& sweep = *it;
		const auto pWeaponStruct = pThis->GetWeapon(sweep.WeaponIndex);
		const auto pWeapon = pWeaponStruct ? pWeaponStruct->WeaponType : nullptr;
		const auto pWeaponExt = pWeapon ? WeaponTypeExt::TryFetch(pWeapon) : nullptr;

		if (!pWeaponExt || !pWeaponExt->IsSweepFireEnabled())
		{
			it = this->Sweeps.erase(it);
			continue;
		}

		bool failed = false;
		const int shotInterval = Math::max(1, static_cast<int>(pWeapon->ROF * this->AE.ROFMultiplier));

		// AttachToTarget / UpdateDirection move the line before this frame's shots are placed
		// on it, so a shot always lands on the line as it is now.
		this->RefreshSweepLine(sweep, pWeapon);

		while (sweep.ShotTimer.GetTimeLeft() <= 0 && sweep.SweepTimer.GetTimeLeft() > 0)
		{
			if (!this->FireSweepShot(sweep, pWeapon))
			{
				// Allocation failure: stop this sweep instead of retrying forever
				// (D-16, there is no shot cap).
				failed = true;
				break;
			}

			sweep.ShotTimer.Start(shotInterval);
		}

		// The aim point reached the end of the line, or the sweep was cut short.
		if (failed || sweep.SweepTimer.GetTimeLeft() <= 0)
		{
			// Optional closing shot (D-17, SweepFire.ShotAtEnd): the aim point moves
			// in ROF-sized steps, so the last step normally stops short of the end
			// of the line. Fire one more shot exactly on the end point.
			if (!failed && pWeaponExt->SweepFire_ShotAtEnd)
			{
				const double lineLength = BulletExt::Get2DDistance(sweep.LineSource, sweep.LineTarget);

				if (sweep.LastDistance < lineLength - 1.0)
				{
					const double previous = sweep.LastDistance;

					if (this->FireSweepShot(sweep, pWeapon, lineLength) && SweepFireDiag::Allowed())
						Debug::Log("[SweepFire] %s: closing shot at the end of the line (distance %.0f, previous %.0f)\n", pWeapon->ID, lineLength, previous);
				}
			}

			cooldown = Math::max(cooldown, pWeaponExt->SweepFire_Cooldown.Get());

			if (SweepFireDiag::Allowed())
				Debug::Log("[SweepFire] %s: sweep ended after %d follow-up shot(s) (failed=%d)\n", pWeapon->ID, sweep.ShotsFired, failed ? 1 : 0);

			it = this->Sweeps.erase(it);
			continue;
		}

		++it;
	}

	// The next trigger waits for the cooldown measured from the end of the sweep.
	if (this->Sweeps.empty())
	{
		if (cooldown >= 0)
			this->SweepFireCooldownTimer.Start(cooldown);
	}
	else
	{
		// Keep the unit's own firing animation running for the whole sweep. Every
		// sweeper shares one animation, so the slot of any sweep will do.
		this->UpdateInfantrySweepAnim(this->Sweeps.front().WeaponIndex);
	}
}

void TechnoExt::AbortSweepFire(bool releaseRearm)
{
	if (this->Sweeps.empty())
		return;

	this->Sweeps.clear();
	this->SweepFireCooldownTimer.Stop();

	// The synthetic reload written by the rearm hook was sized for the whole
	// sweep. The sweep is gone, so keeping it would only lock the weapon out for
	// a while with nothing on screen to explain it.
	if (releaseRearm)
		this->OwnerObject()->RearmTimer.Stop();
}

void TechnoExt::UpdateInfantrySweepAnim(int weaponIndex)
{
	const auto pThis = this->OwnerObject();

	if (pThis->WhatAmI() != AbstractType::Infantry)
		return;

	const auto pWeaponStruct = pThis->GetWeapon(weaponIndex);
	const auto pWeapon = pWeaponStruct ? pWeaponStruct->WeaponType : nullptr;
	const auto pWeaponExt = pWeapon ? WeaponTypeExt::TryFetch(pWeapon) : nullptr;

	if (!pWeaponExt || !pWeaponExt->SweepFire_InfantryFireAnim)
		return;

	const auto pInf = static_cast<InfantryClass*>(pThis);

	// Mirror of the sequence InfantryClass::FiringAI itself would pick
	// (0x52078F..0x5208FE), so a sweep never plays something the unit would not
	// have played for a normal shot.
	Sequence sequence;

	if (pInf->Type->Locomotor == LocomotionClass::CLSIDs::Jumpjet)
		sequence = Sequence::FireFly;
	else if (pInf->SequenceAnim == Sequence::Deploy || pInf->SequenceAnim == Sequence::Deployed
		|| pInf->SequenceAnim == Sequence::DeployedFire || pInf->SequenceAnim == Sequence::DeployedIdle)
		sequence = Sequence::DeployedFire;
	else if (InfantryTypeExt::Fetch(pInf->Type)->IsSecondaryFireAnim(weaponIndex))
		sequence = pInf->Crawling ? Sequence::SecondaryProne : Sequence::SecondaryFire;
	else
		sequence = pInf->Crawling ? Sequence::FireProne : Sequence::FireUp;

	// PlayAnim returns early - without doing anything - when the requested
	// sequence is the one already playing (0x51D911), and force does not change
	// that. Calling it every frame therefore loops the firing animation exactly
	// once per cycle: a no-op while it plays, a restart the frame after it ends.
	pInf->PlayAnim(sequence, true);
}

bool TechnoExt::FireSweepShot(SweepFireInstance& sweep, WeaponTypeClass* pWeapon, double distanceOverride)
{
	const auto pThis = this->OwnerObject();
	const auto pWeaponExt = WeaponTypeExt::Fetch(pWeapon);
	const auto pOwner = pThis->Owner;

	// Where the aim point is on this frame: the line is walked at a constant
	// speed, so the distance only depends on how long the sweep has been running.
	// An explicit distance is used for the optional closing shot, which lands
	// exactly on the end of the line instead of on the last ROF-sized step.
	const int elapsed = sweep.SweepTimer.TimeLeft - sweep.SweepTimer.GetTimeLeft();
	const double distance = distanceOverride >= 0.0 ? distanceOverride : pWeaponExt->SweepFire_Speed * elapsed;

	CoordStruct aim {};

	if (!EngraveLine::GetCoordAtDistance(BulletExt::Coord2Point(sweep.LineSource), BulletExt::Coord2Point(sweep.LineTarget),
			distance, sweep.LineTarget.Z, aim))
		return false;

	sweep.LastDistance = distance;

	// The projectile's own scatter belongs to the *launch solution*, exactly where it sits for a
	// normal shot of the same projectile: the engine scatters the firing offset and leaves the
	// destination on the target. `aim` is the coordinate this shot fires at and stays exact -
	// only the point the launch is solved for moves. Projectiles with a Phobos trajectory are
	// skipped because their own path scatters them (path 3).
	CoordStruct launchAim = aim;

	if (pWeapon->Projectile && BulletExt::IsScatterEligible(pWeapon->Projectile) && !BulletExt::HasTrajectory(pWeapon->Projectile))
	{
		const CoordStruct firerCoord = pThis->GetCoords();
		launchAim = BulletExt::GetScatteredCoord(aim, firerCoord, aim - firerCoord, pWeapon->Projectile, pWeapon);
	}

	const auto pAimCell = MapClass::Instance.TryGetCellAt(aim);

	if (!pAimCell)
	{
		if (SweepFireDiag::Allowed())
			Debug::Log("[SweepFire] %s: follow-up shot dropped (aim point %d,%d,%d is off the map)\n", pWeapon->ID, aim.X, aim.Y, aim.Z);

		return false;
	}

	// aim: the shot aims at the exact point the aim point is on this frame.
	// cell: the shot aims at the centre of the cell containing that point, which
	// is what the first version did and what quantizes the line into cell-sized
	// steps. Auto behaves like aim; only Cell keeps the old gridded behaviour.
	const bool exactAim = pWeaponExt->SweepFire_AimMode != SweepFireAimMode::Cell;

	{
		static int probeLines = 0;

		if (SweepFireDiag::On() && probeLines < 4)
		{
			++probeLines;
			Debug::Log("[SweepFire/probe] follow-up setup exactAim=%d aimMode=%d proj=[%s] arcing=%d invis=%d aim=(%d,%d,%d) launchAim=(%d,%d,%d)\n",
				exactAim ? 1 : 0, static_cast<int>(pWeaponExt->SweepFire_AimMode.Get()),
				pWeapon->Projectile->ID, pWeapon->Projectile->Arcing ? 1 : 0, pWeapon->Projectile->Inviso ? 1 : 0,
				aim.X, aim.Y, aim.Z, launchAim.X, launchAim.Y, launchAim.Z);
		}
	}

	const int damage = static_cast<int>(pWeapon->Damage * TechnoExt::GetCurrentFirepowerMultiplier(pThis));
	const auto pBullet = pWeapon->Projectile->CreateBullet(pAimCell, pThis, damage, pWeapon->Warhead, pWeapon->Speed, pWeapon->Bright);

	if (!pBullet)
	{
		if (SweepFireDiag::Allowed())
			Debug::Log("[SweepFire] %s: follow-up shot dropped (CreateBullet returned null)\n", pWeapon->ID);

		return false;
	}

	const auto pExt = BulletExt::Fetch(pBullet);

	// Diagnostic: remember what this shot's launch was actually solved for.
	pExt->SweepFireLaunchAim = launchAim;

	if (exactAim)
	{
		// The bullet keeps the cell as its target (the engine needs a valid target
		// to fly at and detonate over), but its aim coordinate is the exact point
		// on the line. That coordinate is what the projectile AI actually reads to
		// decide where it arrives: BulletClass::AI at 0x4677F1 loads [bullet+0x140]
		// (TargetCoords) and compares it against the bullet's own cell at 0x467840.
		// Set here as well so the launch velocity below is computed towards the aim.
		pBullet->TargetCoords = aim;
		pExt->SweepFireAim = true;
	}

	// Muzzle of the Burst shot this sweep belongs to.
	bool flhFound = false;
	CoordStruct flh = TechnoExt::GetBurstFLH(pThis, sweep.WeaponIndex, flhFound, sweep.BurstIndex);

	if (!flhFound)
	{
		const auto pWeaponStruct = pThis->GetWeapon(sweep.WeaponIndex);
		flh = pWeaponStruct ? pWeaponStruct->FLH : CoordStruct::Empty;

		if (sweep.Mirrored)
			flh.Y = -flh.Y;
	}

	const CoordStruct muzzle = TechnoExt::GetFLHAbsoluteCoords(pThis, flh, pThis->HasTurret());

	pExt->NotMainWeapon = false;
	pExt->FLHCoord = flh;
	// Suppress the trajectory's own OpenFire so the aim point can be set first. The launch
	// is solved for launchAim (the aim point with the projectile's scatter applied), so the
	// scatter steers the shot while the coordinate it was aimed at stays exact.
	pExt->DispersedTrajectory = true;
	BulletExt::SimulatedFiringUnlimbo(pBullet, pOwner, pWeapon, muzzle, true, {}, exactAim ? &launchAim : nullptr);
	pExt->DispersedTrajectory = false;

	if (exactAim)
	{
		// Set it again *after* the launch, which is the point at which it is known to
		// survive: BulletClass::MoveTo (0x468670) rewrites the bullet, and for an
		// Inviso projectile such as MP5Proj it does not even get that far - the
		// unlimbo it performs fails (0x46868B -> 0x468B7D returns false), leaving the
		// bullet sitting on the cell it was created for with no velocity at all. The
		// value written before the launch is lost in that path, which is why every
		// follow-up shot used to detonate on its cell centre. The first shot writes
		// its aim at 0x6FF08B, i.e. after the launch, and that one did stick.
		pBullet->TargetCoords = aim;
		pExt->SweepFireAim = true;

		static int probeLines = 0;

		if (SweepFireDiag::On() && probeLines < 6)
		{
			++probeLines;
			Debug::Log("[SweepFire/probe] after launch tc=(%d,%d,%d) loc=(%d,%d,%d) vel=(%.1f,%.1f,%.1f) aim=(%d,%d,%d)\n",
				pBullet->TargetCoords.X, pBullet->TargetCoords.Y, pBullet->TargetCoords.Z,
				pBullet->Location.X, pBullet->Location.Y, pBullet->Location.Z,
				pBullet->Velocity.X, pBullet->Velocity.Y, pBullet->Velocity.Z,
				aim.X, aim.Y, aim.Z);
		}
	}

	if (const auto pTraj = pExt->Trajectory.get())
	{
		pTraj->CurrentBurst = sweep.Mirrored ? -1 : 0;
		pTraj->CountOfBurst = 1;
		pTraj->OpenFire();
	}

	// Every follow-up shot is a full shot, effects included (D-10/D-24).
	BulletExt::SimulatedFiringEffects(pBullet, pOwner, nullptr, true, true);

	++sweep.ShotsFired;

	if (SweepFireDiag::Allowed())
	{
		Debug::Log("[SweepFire] %s: follow-up #%d fired at distance %d -> aim (%d,%d)\n",
			pWeapon->ID, sweep.ShotsFired, static_cast<int>(distance), aim.X, aim.Y);
	}

	return true;
}

SweepFireInstance* TechnoExt::FindSweep(int burstIndex)
{
	const auto it = std::find_if(this->Sweeps.begin(), this->Sweeps.end(),
		[burstIndex](const SweepFireInstance& item) { return item.BurstIndex == burstIndex; });

	return it != this->Sweeps.end() ? &*it : nullptr;
}

int TechnoExt::GetSweepFireRearmTime(WeaponTypeClass* pWeapon)
{
	const auto pWeaponExt = WeaponTypeExt::TryFetch(pWeapon);

	if (!pWeaponExt || !pWeaponExt->IsSweepFireEnabled())
		return -1;

	const auto pThis = this->OwnerObject();

	// Only the last shot of a Burst arms the rearm timer. The earlier barrels keep
	// the vanilla Burst delays, so a multi-barrel weapon still fires all of them
	// and only the finished sweep blocks the next trigger (8.2 / R-17).
	if (pThis->CurrentBurstIndex < pWeapon->Burst)
		return -1;

	// Taking this shot bumped CurrentBurstIndex, so the sweep it belongs to is the
	// one keyed by the previous value.
	const auto pSweep = this->FindSweep(pThis->CurrentBurstIndex - 1);

	if (!pSweep)
		return -1;

	// The engine writes this into RearmTimer, which is what makes the weapon look
	// busy for the whole sweep and keeps the attack cursor and the AI honest
	// (D-23). Berzerk's halving is discarded along with the vanilla value (D-29).
	return pSweep->SweepTimer.TimeLeft + pWeaponExt->SweepFire_Cooldown.Get();
}

// =============================
// load / save

template <typename T>
void TechnoExt::Serialize(T& Stm)
{
	Stm
		.Process(this->TypeExtData)
		.Process(this->Shield)
		.Process(this->LaserTrails)
		.Process(this->AttachedEffects)
		.Process(this->AE)
		.Process(this->AnimRefCount)
		.Process(this->PassengerDeletionTimer)
		.Process(this->CurrentShieldType)
		.Process(this->ChargeTurretTimer)
		.Process(this->AutoDeathTimer)
		.Process(this->MindControlRingAnimType)
		.Process(this->DamageNumberOffset)
		.Process(this->HasBeenPlacedOnMap)
		.Process(this->ForceFullRearmDelay)
		.Process(this->LastRearmWasFullDelay)
		.Process(this->CanCloakDuringRearm)
		.Process(this->WHAnimRemainingCreationInterval)
		.Process(this->LastWeaponType)
		.Process(this->LastWeaponFLH)
		.Process(this->TrajectoryGroup)
		.Process(this->FiringObstacleCell)
		.Process(this->IsDetachingForCloak)
		.Process(this->BeControlledThreatFrame)
		.Process(this->LastTargetID)
		.Process(this->AccumulatedGattlingValue)
		.Process(this->ShouldUpdateGattlingValue)
		.Process(this->AirstrikeTargetingMe)
		.Process(this->DelayedFireSequencePaused)
		.Process(this->DelayedFireTimer)
		.Process(this->DelayedFireWeaponIndex)
		.Process(this->CurrentDelayedFireAnim)
		.Process(this->DropCrate)
		.Process(this->DropCrateType)
		.Process(this->AttachedEffectInvokerCount)
		.Process(this->IsSelected)
		.Process(this->SpecialActionTimer)
		.Process(this->LastSpecialActionFrame)
		.Process(this->SpecialActionWeaponIndex)
		.Process(this->SpecialActionBurstShotsLeft)
		.Process(this->TintColorOwner)
		.Process(this->TintColorAllies)
		.Process(this->TintColorEnemies)
		.Process(this->TintIntensityOwner)
		.Process(this->TintIntensityAllies)
		.Process(this->TintIntensityEnemies)
		.Process(this->SpecialTracked)
		.Process(this->FallingDownTracked)
		.Process(this->OnParachuted)
		.Process(this->HoverShutdown)
		.Process(this->LastTargetCrd)
		.Process(this->LastTargetCrdClearTimer)
		.Process(this->ShouldBeDead)
		.Process(this->PreventCrewEscape)
		.Process(this->Sweeps)
		.Process(this->SweepFireCooldownTimer)
		;
}

bool SweepFireInstance::Load(PhobosStreamReader& stm, bool registerForChange)
{
	return this->Serialize(stm);
}

bool SweepFireInstance::Save(PhobosStreamWriter& stm) const
{
	return const_cast<SweepFireInstance*>(this)->Serialize(stm);
}

template <typename T>
bool SweepFireInstance::Serialize(T& stm)
{
	return stm
		.Process(this->WeaponIndex)
		.Process(this->BurstIndex)
		.Process(this->ShotTimer)
		.Process(this->SweepTimer)
		.Process(this->LineSource)
		.Process(this->LineTarget)
		.Process(this->AnchorCoord)
		.Process(this->RotateRadian)
		.Process(this->Mirrored)
		.Process(this->ShotsFired)
		.Process(this->LastDistance)
		.Process(this->AnchorTargetID)
		.Success();
}

void TechnoExt::OnDetach(AirstrikeClass* pTarget, bool removed)
{
	if (removed)
		AnnounceInvalidPointer(this->AirstrikeTargetingMe, pTarget);
}

void TechnoExt::LoadFromStream(PhobosStreamReader& Stm)
{
	RadioExt::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void TechnoExt::SaveToStream(PhobosStreamWriter& Stm)
{
	RadioExt::SaveToStream(Stm);
	this->Serialize(Stm);
}

bool TechnoExt::LoadGlobals(PhobosStreamReader& Stm)
{
	return Stm
		.Success();
}

bool TechnoExt::SaveGlobals(PhobosStreamWriter& Stm)
{
	return Stm
		.Success();
}

// =============================
// container hooks

// The extension is allocated by the concrete leaf constructors (UnitClass/InfantryClass/
// BuildingClass/AircraftClass), not here at the abstract TechnoClass level.

// The extension is removed by the leaf destructor hooks; this only keeps the
// tech tree recheck side effect at the shared base destructor.
DEFINE_HOOK(0x6F4500, TechnoClass_DTOR, 0x5)
{
	GET(TechnoClass*, pItem, ECX);

	if (pItem->AbstractFlags & AbstractFlags::Foot)
		pItem->Owner->RecheckTechTree = true; // for SW.AuxTechons and SW.NegTechnos

	return 0;
}

DEFINE_HOOK(0x710415, TechnoClass_DetachAnim, 0x6)
{
	GET(TechnoClass*, pThis, ECX);
	GET(AbstractClass*, pTarget, EAX);

	auto const pExt = TechnoExt::Fetch(pThis);

	if (pExt->CurrentDelayedFireAnim == pTarget)
		pExt->CurrentDelayedFireAnim = nullptr;

	return 0;
}

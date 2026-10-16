#include "Body.h"

#include <Ext/Anim/Body.h>
#include <Ext/CaptureManager/Body.h>
#include <Ext/WeaponType/Body.h>
#include <Ext/Scenario/Body.h>
#include <Utilities/Helpers.Alex.h>

DEFINE_HOOK(0x4690D4, BulletClass_Logics_NewChecks, 0x6)
{
	enum { SkipShaking = 0x469130, GoToExtras = 0x469AA4 };

	GET(BulletClass*, pBullet, ESI);
	GET(WarheadTypeClass*, pWarhead, EAX);
	GET_BASE(CoordStruct const* const, pCoords, 0x8);

	if (BulletExt::Fetch(pBullet)->Status & TrajectoryStatus::Vanish)
		return GoToExtras;

	auto const pExt = WarheadTypeExt::Fetch(pWarhead);

	if (auto const pTarget = abstract_cast<TechnoClass*>(pBullet->Target))
	{
		// Check if the WH should affect the techno target or skip it
		if (BulletTypeExt::Fetch(pBullet->Type)->Shrapnel_ObeyWarheadTriggerConditions.Get(RulesExt::Global()->Shrapnel_ObeyWarheadTriggerConditions))
		{
			if (!pExt->IsHealthInThreshold(pTarget)
				|| !pExt->IsVeterancyInThreshold(pTarget)
				|| (!pExt->AffectsNeutral && pTarget->Owner->IsNeutral())
				|| !pExt->IsInvokerAllowed(pTarget, pBullet->Owner))
				return GoToExtras;
		}
	}

	// Check for ScreenShake
	if (Phobos::Config::HideShakeEffects)
		return SkipShaking;

	auto&& [_, visible] = TacticalClass::Instance->CoordsToClient(*pCoords);

	if (pExt->ShakeIsLocal.Get(RulesExt::Global()->ShakeIsLocal) && !visible)
		return SkipShaking;

	return 0;
}

DEFINE_HOOK(0x4692BD, BulletClass_Logics_ApplyMindControl, 0x6)
{
	enum { SkipGameCode = 0x4692D5 };

	GET(BulletClass*, pThis, ESI);

	auto const pWHExt = WarheadTypeExt::Fetch(pThis->WH);
	auto const pControlledAnimType = pWHExt->MindControl_Anim.Get(RulesClass::Instance->ControlledAnimationType);
	auto const threatDelay = pWHExt->MindControl_ThreatDelay.Get(RulesExt::Global()->MindControl_ThreatDelay);
	R->AL(CaptureManagerExt::CaptureUnit(pThis->Owner->CaptureManager, pThis->Target, pControlledAnimType, threatDelay));

	return SkipGameCode;
}

DEFINE_HOOK(0x46954C, BulletClass_Logics_IsLocomotor_Bunker, 0x6)
{
	enum { CannotImbue = 0x4695E6 };

	GET(UnitClass*, pTarget, ECX);

	return pTarget->BunkerLinkedItem ? CannotImbue : 0;
}

DEFINE_HOOK(0x469A75, BulletClass_Logics_DamageHouse, 0x7)
{
	GET(BulletClass*, pThis, ESI);
	GET(HouseClass*, pHouse, ECX);

	if (!pHouse)
		R->ECX(BulletExt::Fetch(pThis)->FirerHouse);

	return 0;
}

#pragma region DetonateOnAllMapObjects

DEFINE_HOOK(0x4690C1, BulletClass_Logics_DetonateOnAllMapObjects, 0x8)
{
	enum { ReturnFromFunction = 0x46A2FB };

	GET(BulletClass*, pThis, ESI);

	auto const pWHExt = WarheadTypeExt::Fetch(pThis->WH);

	if (pWHExt->DetonateOnAllMapObjects && !pWHExt->WasDetonatedOnAllMapObjects
		&& pWHExt->DetonateOnAllMapObjects_AffectsTarget != AffectedTarget::None
		&& pWHExt->DetonateOnAllMapObjects_AffectsHouse != AffectedHouse::None)
	{
		pWHExt->WasDetonatedOnAllMapObjects = true;
		auto const originalLocation = pThis->Location;
		auto const pOriginalTarget = pThis->Target;
		auto const isFull = pWHExt->DetonateOnAllMapObjects_Full;
		auto const pOwner = pThis->Owner ? pThis->Owner->Owner : BulletExt::Fetch(pThis)->FirerHouse;

		auto copy_dvc = []<typename T>(const DynamicVectorClass<T>&dvc)
		{
			std::vector<T> vec(dvc.Count);
			std::copy(dvc.begin(), dvc.end(), vec.begin());
			return vec;
		};

		auto tryDetonate = [pThis, pWHExt, pOwner, isFull](TechnoClass* pTechno, TechnoTypeClass* pType)
			{
				if (pWHExt->EligibleForFullMapDetonation(pTechno, pType, pOwner))
				{
					if (isFull)
					{
						pThis->Target = pTechno;
						pThis->Location = pTechno->GetCoords();
						pThis->Detonate(pTechno->GetCoords());
					}
					else
					{
						const int damage = (pThis->Health * pThis->DamageMultiplier) >> 8;
						pWHExt->DamageAreaWithTarget(pTechno->GetCoords(), damage, pThis->Owner, pThis->WH, true, pOwner, pTechno);
					}
				}
			};

		if ((pWHExt->DetonateOnAllMapObjects_AffectsTarget & AffectedTarget::Aircraft) != AffectedTarget::None)
		{
			auto const aircraft = copy_dvc(AircraftClass::Array);

			for (auto const pAircraft : aircraft)
				tryDetonate(pAircraft, pAircraft->Type);
		}

		if ((pWHExt->DetonateOnAllMapObjects_AffectsTarget & AffectedTarget::Building) != AffectedTarget::None)
		{
			auto const buildings = copy_dvc(BuildingClass::Array);

			for (auto const pBuilding : buildings)
				tryDetonate(pBuilding, pBuilding->Type);
		}

		if ((pWHExt->DetonateOnAllMapObjects_AffectsTarget & AffectedTarget::Infantry) != AffectedTarget::None)
		{
			auto const infantry = copy_dvc(InfantryClass::Array);

			for (auto const pInf : infantry)
				tryDetonate(pInf, pInf->Type);
		}

		if ((pWHExt->DetonateOnAllMapObjects_AffectsTarget & AffectedTarget::Unit) != AffectedTarget::None)
		{
			auto const units = copy_dvc(UnitClass::Array);

			for (auto const pUnit : units)
				tryDetonate(pUnit, pUnit->Type);
		}

		pThis->Target = pOriginalTarget;
		pThis->Location = originalLocation;
		pWHExt->WasDetonatedOnAllMapObjects = false;

		return ReturnFromFunction;
	}

	return 0;
}

#pragma endregion

DEFINE_HOOK(0x469D1A, BulletClass_Logics_Debris, 0x6)
{
	enum { SkipGameCode = 0x469EBA };

	GET(BulletClass*, pThis, ESI);

	const auto pWH = pThis->WH;
	const auto pWHExt = WarheadTypeExt::Fetch(pWH);

	if (pWHExt->Debris_Conventional.Get(RulesExt::Global()->Debris_Conventional) && pThis->GetCell()->LandType == LandType::Water)
		return SkipGameCode;

	// Fix the debris count to be in range of Min, Max instead of Min, Max-1.
	int totalSpawnAmount = ScenarioClass::Instance->Random.RandomRanged(pWH->MinDebris, pWH->MaxDebris);

	if (totalSpawnAmount > 0)
	{
		const auto& debrisTypes = pWH->DebrisTypes;
		const auto& debrisMaximums = pWH->DebrisMaximums;

		const auto pOwner = pThis->Owner ? pThis->Owner->Owner : BulletExt::Fetch(pThis)->FirerHouse;
		auto coord = pThis->GetCoords();

		const int count = Math::min(debrisTypes.Count, debrisMaximums.Count);

		// Restore DebrisMaximums logic
		// Make DebrisTypes generate completely in accordance with DebrisMaximums,
		// without continuously looping until it exceeds totalSpawnAmount
		if (count > 0)
		{
			const auto& debrisMinimums = pWHExt->DebrisMinimums;
			const bool limit = pWHExt->DebrisTypes_Limit.Get(count > 1);
			const int minimumsMaxIndex = static_cast<int>(debrisMinimums.size()) - 1;
			int currentIndex = 0;

			while (totalSpawnAmount > 0)
			{
				const int currentMaxDebris = Math::max(1, debrisMaximums[currentIndex]);
				const int currentMinDebris = (minimumsMaxIndex >= 0) ? Math::max(0, debrisMinimums[Math::min(currentIndex, minimumsMaxIndex)]) : 0;
				int amountToSpawn = Math::min(totalSpawnAmount, ScenarioClass::Instance->Random.RandomRanged(currentMinDebris, currentMaxDebris));
				totalSpawnAmount -= amountToSpawn;

				for (; amountToSpawn > 0; --amountToSpawn)
					GameCreate<VoxelAnimClass>(debrisTypes[currentIndex], &coord, pOwner);

				if (totalSpawnAmount <= 0)
					break;

				if (++currentIndex < count)
					continue;

				if (limit)
					break;

				currentIndex = 0;
			}
		}
		// Record the ownership of the animation
		// Different from technos, the original judging condition here were mutually exclusive
		else
		{
			const auto debrisAnims = pWHExt->DebrisAnims.GetElements(RulesClass::Instance->MetallicDebris);
			const int maxIndex = static_cast<int>(debrisAnims.size()) - 1;

			if (maxIndex >= 0)
			{
				do
				{
					const int debrisIndex = ScenarioClass::Instance->Random.RandomRanged(0, maxIndex);
					const auto pAnim = GameCreate<AnimClass>(debrisAnims[debrisIndex], coord);
					AnimExt::SetAnimOwnerHouseKind(pAnim, pOwner, nullptr, false, true);
				}
				while (--totalSpawnAmount > 0);
			}
		}
	}

	return SkipGameCode;
}

DEFINE_HOOK(0x469B44, BulletClass_Logics_LandTypeCheck, 0x6)
{
	enum { SkipChecks = 0x469BA2 };

	GET(BulletClass*, pThis, ESI);

	auto const pWHExt = WarheadTypeExt::Fetch(pThis->WH);

	if (pWHExt->Conventional_IgnoreUnits.Get(RulesExt::Global()->Conventional_IgnoreUnits))
		return SkipChecks;

	return 0;
}

DEFINE_JUMP(LJMP, 0x469AC1, 0x469AF0) // Skip random scatter in vanilla code

DEFINE_HOOK(0x469C46, BulletClass_Logics_DamageAnimSelected, 0x8)
{
	enum { SkipGameCode = 0x469C98 };

	GET(BulletClass*, pThis, ESI);
	GET(AnimTypeClass*, pAnimType, EBX);
	LEA_STACK(CoordStruct*, coords, STACK_OFFSET(0xA4, -0x40));

	bool createdAnim = false;

	if (pAnimType)
	{
		auto const pWHExt = WarheadTypeExt::Fetch(pThis->WH);
		const int cellHeight = MapClass::Instance.GetCellFloorHeight(*coords);
		auto const newCrds = pWHExt->PlayAnimAboveSurface ? CoordStruct { coords->X, coords->Y, Math::max(cellHeight, coords->Z) } : *coords;

		if (cellHeight > newCrds.Z && !pWHExt->PlayAnimUnderground)
		{
			R->EAX(createdAnim);
			return SkipGameCode;
		}

		auto const pOwner = pThis->Owner;
		const bool splashed = pWHExt->Splashed;
		const int creationInterval = splashed ? pWHExt->SplashList_CreationInterval : pWHExt->AnimList_CreationInterval;
		int* remainingInterval = &pWHExt->RemainingAnimCreationInterval;
		const int scatterMin = splashed ? pWHExt->SplashList_ScatterMin.Get() : pWHExt->AnimList_ScatterMin.Get();
		const int scatterMax = splashed ? pWHExt->SplashList_ScatterMax.Get() : pWHExt->AnimList_ScatterMax.Get();
		const bool allowScatter = scatterMax >= 0 || scatterMin >= 0 || pThis->Type->Inviso;

		if (creationInterval > 0 && pOwner)
			remainingInterval = &TechnoExt::Fetch(pOwner)->WHAnimRemainingCreationInterval;

		if (creationInterval < 1 || *remainingInterval <= 0)
		{
			*remainingInterval = creationInterval;

			HouseClass* pInvoker = pOwner ? pOwner->Owner : BulletExt::Fetch(pThis)->FirerHouse;
			HouseClass* pVictim = nullptr;

			if (auto const pTarget = abstract_cast<TechnoClass*>(pThis->Target))
				pVictim = pTarget->Owner;

			auto types = make_iterator_single(pAnimType);

			if (pWHExt->SplashList_CreateAll && splashed)
			{
				types = pWHExt->SplashList.GetElements(RulesClass::Instance->SplashList);
			}
			else if (!splashed)
			{
				bool createAll = pWHExt->AnimList_CreateAll;

				if (pWHExt->Crit_Active && pWHExt->Crit_AnimList.size() > 0
					&& !pWHExt->Crit_AnimOnAffectedTargets.Get(RulesExt::Global()->Crit_AnimOnAffectedTargets))
				{
					createAll = pWHExt->Crit_AnimList_CreateAll.Get(createAll);

					if (createAll)
						types = pWHExt->Crit_AnimList;
				}
				else if (createAll)
				{
					types = pWHExt->OwnerObject()->AnimList;
				}
			}

			auto& random = ScenarioClass::Instance->Random;

			for (auto const& pType : types)
			{
				if (!pType)
					continue;

				auto animCoords = newCrds;

				if (allowScatter)
				{
					int distance = 32;

					if (scatterMax >= 0 || scatterMin >= 0)
						distance = random(scatterMin, scatterMax);

					animCoords = MapClass::GetRandomCoordsNear(animCoords, distance, false);
				}

				auto const pAnim = GameCreate<AnimClass>(pType, animCoords, 0, 1, 0x2600, pWHExt->AnimZAdjust.Get(RulesExt::Global()->WarheadAnimZAdjust), false);
				createdAnim = true;
				AnimExt::SetAnimOwnerHouseKind(pAnim, pInvoker, pVictim);

				if (!pAnim->Owner)
					pAnim->Owner = pInvoker;

				if (pOwner)
				{
					auto const pExt = AnimExt::Fetch(pAnim);
					pExt->SetInvoker(pOwner);
				}
			}
		}
		else
		{
			(*remainingInterval)--;
		}
	}

	R->EAX(createdAnim);
	return SkipGameCode;
}

DEFINE_HOOK(0x469AA4, BulletClass_Logics_Extras, 0x5)
{
	GET(BulletClass*, pThis, ESI);
	GET_BASE(CoordStruct const* const, coords, 0x8);

	auto const pTechno = pThis->Owner;
	auto const pBulletExt = BulletExt::Fetch(pThis);
	auto const pOwner = pTechno ? pTechno->Owner : pBulletExt->FirerHouse;

	// Extra warheads
	if (auto const pWeapon = pThis->WeaponType)
	{
		auto const pWeaponExt = WeaponTypeExt::Fetch(pWeapon);
		auto const& extraWarheads = pWeaponExt->ExtraWarheads;
		auto const& damageOverrides = pWeaponExt->ExtraWarheads_DamageOverrides;
		auto const& detonationChances = pWeaponExt->ExtraWarheads_DetonationChances;
		auto const& rollChances = pWeaponExt->ExtraWarheads_RollChances;
		auto const& fullDetonation = pWeaponExt->ExtraWarheads_FullDetonation;
		const int defaultDamage = pWeapon->Damage;
		auto& random = ScenarioClass::Instance->Random;

		auto const pTarget = abstract_cast<TechnoClass*>(pThis->Target);

		auto detonateWarhead = [&](int index)
			{
				if (index < 0 || index >= static_cast<int>(extraWarheads.size()))
					return;

				auto const pWH = extraWarheads[index];
				auto const pWHExt = WarheadTypeExt::Fetch(pWH);

				if (pTarget && (!pWHExt->IsHealthInThreshold(pTarget)
					|| !pWHExt->IsVeterancyInThreshold(pTarget)
					|| !pWHExt->IsInvokerAllowed(pTarget, pTechno)))
					return;

				int damage = defaultDamage;
				size_t size = damageOverrides.size();
				if (size > static_cast<size_t>(index))
					damage = damageOverrides[index];
				else if (size > 0)
					damage = damageOverrides[size - 1];

				size = fullDetonation.size();
				bool isFull = true;
				if (size > static_cast<size_t>(index))
					isFull = fullDetonation[index];
				else if (size > 0)
					isFull = fullDetonation[size - 1];

				if (isFull)
					WarheadTypeExt::DetonateAt(pWH, *coords, pTechno, damage, pOwner, pThis->Target);
				else
					pWHExt->DamageAreaWithTarget(*coords, damage, pTechno, pWH, true, pOwner, pTarget);
			};

		if (pWeaponExt->ExtraWarheads_WeightsData.size() > 0)
		{
			size_t rollCount = rollChances.size();
			if (rollCount == 0)
				rollCount = 1;

			for (size_t i = 0; i < rollCount; i++)
			{
				double dice = random.RandomDouble();
				if (rollChances.size() > 0 && dice > rollChances[i])
					continue;

				const size_t weightIndex = std::min(i, pWeaponExt->ExtraWarheads_WeightsData.size() - 1);
				const auto& weights = pWeaponExt->ExtraWarheads_WeightsData[weightIndex];

				int selectedIndex = GeneralUtils::ChooseOneWeighted(dice, &weights);

				bool detonate = true;
				size_t chanceSize = detonationChances.size();
				if (chanceSize > 0)
				{
					double chanceDice = random.RandomDouble();
					if (chanceSize > static_cast<size_t>(selectedIndex))
						detonate = detonationChances[selectedIndex] >= chanceDice;
					else
						detonate = detonationChances[chanceSize - 1] >= chanceDice;
				}

				if (detonate)
					detonateWarhead(selectedIndex);
			}
		}
		else
		{
			for (size_t i = 0; i < extraWarheads.size(); i++)
			{
				size_t size = detonationChances.size();
				bool detonate = true;
				if (size > i)
					detonate = detonationChances[i] >= random.RandomDouble();
				else if (size > 0)
					detonate = detonationChances[size - 1] >= random.RandomDouble();

				if (!detonate)
					continue;

				detonateWarhead(static_cast<int>(i));
			}
		}
	}

	// Return to sender
	if (pTechno)
	{
		auto const pTypeExt = BulletTypeExt::Fetch(pThis->Type);

		if (auto const pWeapon = pTypeExt->ReturnWeapon)
		{
			int damage = pWeapon->Damage;

			if (pTypeExt->ReturnWeapon_ApplyFirepowerMult.Get(RulesExt::Global()->ReturnWeapon_ApplyFirepowerMult))
				damage = static_cast<int>(damage * pBulletExt->FirepowerMult);

			if (auto const pBullet = pWeapon->Projectile->CreateBullet(pTechno, pTechno,
				damage, pWeapon->Warhead, pWeapon->Speed, pWeapon->Bright))
			{
				BulletExt::Fetch(pBullet)->FirepowerMult = pBulletExt->FirepowerMult;
				BulletExt::SimulatedFiringUnlimbo(pBullet, pOwner, pWeapon, pThis->Location, true);
				BulletExt::SimulatedFiringEffects(pBullet, pOwner, nullptr, false, true);
			}
		}
	}

	// Unlimbo Detonate
	const auto pWH = pThis->WH;
	const auto pWHExt = WarheadTypeExt::Fetch(pWH);
	pWHExt->InDamageArea = true;

	if (pTechno && pTechno->InLimbo && !pWH->Parasite && pWHExt->UnlimboDetonate)
	{
		CoordStruct location = *coords;
		const auto pTarget = pThis->Target;
		const bool isInAir = pTarget && pTarget->AbstractFlags & AbstractFlags::Foot ? static_cast<FootClass*>(pTarget)->IsInAir() : false;
		bool success = false;

		if (!pWHExt->UnlimboDetonate_ForceLocation)
		{
			const auto pType = pTechno->GetTechnoType();
			const auto nCell = MapClass::Instance.NearByLocation(CellClass::Coord2Cell(location),
									pType->SpeedType, -1, pType->MovementZone, false, 1, 1, true,
									false, false, true, CellStruct::Empty, false, false);

			const auto pCell = MapClass::Instance.TryGetCellAt(nCell);

			if (pCell)
			{
				pTechno->OnBridge = pCell->ContainsBridge();
				location = pCell->GetCoordsWithBridge();
			}
			else
			{
				pTechno->OnBridge = false;
			}

			if (isInAir)
				location.Z = coords->Z;

			success = pTechno->Unlimbo(location, pTechno->PrimaryFacing.Current().GetDir());
		}
		else
		{
			const auto pCell = MapClass::Instance.TryGetCellAt(location);

			if (pCell)
				pTechno->OnBridge = pCell->ContainsBridge();
			else
				pTechno->OnBridge = false;

			++Unsorted::ScenarioInit;
			success = pTechno->Unlimbo(location, pTechno->PrimaryFacing.Current().GetDir());
			--Unsorted::ScenarioInit;
		}

		const auto pTechnoExt = TechnoExt::Fetch(pTechno);

		if (success)
		{
			if (isInAir)
			{
				pTechno->IsFallingDown = true;
				TechnoExt::Fetch(pTechno)->OnParachuted = true;
			}

			if (pWHExt->UnlimboDetonate_KeepTarget
				&& pTarget && pTarget->AbstractFlags & AbstractFlags::Object)
			{
				pTechno->SetTarget(pTarget);
			}
			else
			{
				pTechno->SetTarget(nullptr);
			}

			if (pTechnoExt->IsSelected)
			{
				auto& vec = ScenarioExt::Global()->LimboLaunchers;
				vec.erase(std::remove(vec.begin(), vec.end(), pTechnoExt), vec.end());
				pTechno->Select();
				pTechnoExt->IsSelected = false;
			}
		}
		else
		{
			if (pTechnoExt->IsSelected)
			{
				auto& vec = ScenarioExt::Global()->LimboLaunchers;
				vec.erase(std::remove(vec.begin(), vec.end(), pTechnoExt), vec.end());
				pTechnoExt->IsSelected = false;
			}

			pTechno->SetLocation(location);
			pTechno->ReceiveDamage(&pTechno->Health, 0, RulesClass::Instance->C4Warhead, nullptr, true, false, pTechno->Owner);
		}
	}

	return 0;
}

#pragma region Airburst

static bool IsAllowedSplitsTarget(TechnoClass* pSource, HouseClass* pOwner, WeaponTypeClass* pWeapon, TechnoClass* pTarget, bool useWeaponTargeting)
{
	auto const pWH = pWeapon->Warhead;

	if (useWeaponTargeting)
	{
		auto const pType = pTarget->GetTechnoType();

		if (!pType->LegalTarget || GeneralUtils::GetWarheadVersusArmor(pWH, pTarget, pType) == 0.0)
			return false;

		auto const pWeaponExt = WeaponTypeExt::Fetch(pWeapon);

		if (pWeaponExt->SkipWeaponPicking)
			return true;

		if (!EnumFunctions::CanTargetHouse(pWeaponExt->CanTargetHouses, pOwner, pTarget->Owner)
			|| !EnumFunctions::IsCellEligible(pTarget->GetCell(), pWeaponExt->CanTarget, true, true)
			|| !EnumFunctions::IsTechnoEligible(pTarget, pWeaponExt->CanTarget)
			|| !pWeaponExt->IsHealthInThreshold(pTarget)
			|| !pWeaponExt->IsVeterancyInThreshold(pTarget))
		{
			return false;
		}

		if (!pWeaponExt->HasRequiredAttachedEffects(pTarget, pSource))
			return false;
	}
	else
	{
		if (!WarheadTypeExt::Fetch(pWH)->CanTargetHouse(pOwner, pTarget))
			return false;
	}

	return true;
}

static bool SplitsProjectileCheck(BulletTypeClass* pProjectile, WeaponTypeClass* pWeapon, TechnoClass* pTarget, bool useWeaponTargeting)
{
	if (!useWeaponTargeting)
		return pProjectile->AA || !pTarget->IsInAir();

	auto const pType = pWeapon->Projectile;
	const bool inAir = pTarget->IsInAir();

	if (!pType->AA && inAir)
		return false;

	if (pType->AA && !inAir)
	{
		auto const pTypeExt = BulletTypeExt::Fetch(pType);

		if (pTypeExt->AAOnly)
			return false;
	}

	return true;
}

// Disable Ares' Airburst implementation.
DEFINE_PATCH(0x469EBA, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90);

DEFINE_HOOK(0x468EB3, BulletClass_Explodes_AirburstCheck1, 0x6)
{
	enum { Continue = 0x468EC7, Skip = 0x468FF4 };

	GET(BulletClass*, pThis, ESI);

	auto const pType = pThis->Type;

	R->EAX(pType);
	return !(pType->Airburst || BulletTypeExt::Fetch(pType)->Splits) ? Continue : Skip;
}

DEFINE_HOOK(0x468FF4, BulletClass_Explodes_AirburstCheck2, 0x6)
{
	enum { Continue = 0x46909A, Skip = 0x469008 };

	GET(BulletClass*, pThis, ESI);

	auto const pType = pThis->Type;

	R->EAX(pType);
	return (pType->Airburst || BulletTypeExt::Fetch(pType)->Splits) ? Continue : Skip;
}

DEFINE_HOOK(0x469EC0, BulletClass_Logics_AirburstWeapon, 0x6)
{
	enum { SkipGameCode = 0x46A290 };

	GET(BulletClass*, pThis, ESI);

	auto const pType = pThis->Type;
	auto const pTypeExt = BulletTypeExt::Fetch(pType);
	auto const pWeapon = pType->AirburstWeapon;

	if ((pType->Airburst || pTypeExt->Splits) && pWeapon)
	{
		auto const pSource = pThis->Owner;
		auto const pBulletExt = BulletExt::Fetch(pThis);
		auto pOwner = pSource ? pSource->Owner : pBulletExt->FirerHouse;

		if (!pOwner || pOwner->Defeated)
		{
			if (const auto pNeutral = HouseClass::FindNeutral())
				pOwner = pNeutral;
			else
				return SkipGameCode;
		}

		auto& random = ScenarioClass::Instance->Random;
		int clusterCount = pType->Cluster;

		auto const coordsTarget = pTypeExt->AroundTarget.Get(pTypeExt->Splits) ? pThis->GetTargetCoords() : pThis->GetCoords();
		auto const cellTarget = CellClass::Coord2Cell(coordsTarget);
		bool const allowRepeatTargets = pTypeExt->Splits_AllowRepeatTargets.Get(RulesExt::Global()->Splits_AllowRepeatTargets);
		DynamicVectorClass<AbstractClass*> targets;

		if (!pTypeExt->Splits)
		{
			CellRangeIterator<CellClass>{}(cellTarget, pTypeExt->AirburstSpread, [&targets](CellClass* pCell) -> bool
			{
				targets.AddItem(pCell);
				return true;
			});

			if (pTypeExt->Airburst_UseCluster.Get(RulesExt::Global()->Airburst_UseCluster))
			{
				DynamicVectorClass<AbstractClass*> newTargets;

				if (pTypeExt->Airburst_RandomClusters)
				{
					// Do random cells for amount matching Cluster.
					int count = 0;
					const int targetCount = targets.Count;

					while (count < clusterCount)
					{
						const int index = random.RandomRanged(0, targetCount);
						auto const pTarget = targets.GetItem(index);

						if (count > targetCount || newTargets.FindItemIndex(pTarget) < 0)
						{
							newTargets.AddItem(pTarget);
							count++;
						}
					}
				}
				else
				{
					// Do evenly selected cells for amount matching Cluster.
					const double stepSize = (targets.Count - 1.0) / (clusterCount - 1.0);

					for (int i = 0; i < clusterCount; i++)
					{
						newTargets.AddItem(targets.GetItem(static_cast<int>(round(stepSize * i))));
					}
				}

				targets = newTargets;
			}
			else
			{
				clusterCount = targets.Count;
			}
		}
		else
		{
			const double cellSpread = static_cast<double>(pTypeExt->Splits_TargetingDistance.Get()) / (double)Unsorted::LeptonsPerCell;
			const bool retargetSelf = pTypeExt->RetargetSelf;
			const bool useWeaponTargeting = pTypeExt->Splits_UseWeaponTargeting.Get(RulesExt::Global()->Splits_UseWeaponTargeting);
			const bool cylindrical = pTypeExt->Splits_TargetingDistance_Cylindrical.Get(RulesExt::Global()->Splits_TargetingDistance_Cylindrical);
			auto const& technos = Helpers::Alex::getCellSpreadItemsExt(coordsTarget, cellSpread, true, cylindrical);

			for (auto const pTechno : technos)
			{
				if (pTechno->IsInPlayfield && pTechno->IsOnMap && pTechno->IsAlive && pTechno->Health > 0 && !pTechno->InLimbo
					&& (retargetSelf || pTechno != pSource))
				{
					if (SplitsProjectileCheck(pType, pWeapon, pTechno, useWeaponTargeting) &&
						IsAllowedSplitsTarget(pSource, pOwner, pWeapon, pTechno, useWeaponTargeting))
					{
						targets.AddItem(pTechno);
					}
				}
			}

			const int range = pTypeExt->Splits_TargetCellRange;

			if (range >= 0 && (!allowRepeatTargets || targets.Count < 1))
			{
				while (targets.Count < clusterCount)
				{
					const int x = random.RandomRanged(-range, range);
					const int y = random.RandomRanged(-range, range);

					const CellStruct cell = { static_cast<short>(cellTarget.X + x), static_cast<short>(cellTarget.Y + y) };
					auto const pCell = MapClass::Instance.GetCellAt(cell);

					targets.AddItem(pCell);
				}
			}
		}

		auto const pTypeSplits = pWeapon->Projectile;
		int damage = pWeapon->Damage;

		if (pTypeExt->AirburstWeapon_ApplyFirepowerMult.Get(RulesExt::Global()->AirburstWeapon_ApplyFirepowerMult))
			damage = static_cast<int>(damage * pBulletExt->FirepowerMult);

		// Cache all pointer variables before the loop
		AbstractClass* const targetForDir = MapClass::Instance.TryGetCellAt(coordsTarget);
		auto const targetDir = pThis->GetTargetDirection(targetForDir ? targetForDir: pThis);
		auto const pWH = pWeapon->Warhead;
		auto const location = pThis->Location;
		bool const bright = pWeapon->Bright;
		bool const airburst = pType->Airburst;
		bool const splits = pTypeExt->Splits;
		bool const targetAsSource = pTypeExt->Airburst_TargetAsSource;
		bool const skipHeight = pTypeExt->Airburst_TargetAsSource_SkipHeight.Get(RulesExt::Global()->Airburst_TargetAsSource_SkipHeight);
		double const retargetAccuracy = pTypeExt->RetargetAccuracy;
		double const retargetSelfProbability = pTypeExt->RetargetSelf_Probability;
		int const speed = pWeapon->Speed;
		int const scatterMin = pTypeExt->AirburstWeapon_SourceScatterMin.Get();
		int const scatterMax = pTypeExt->AirburstWeapon_SourceScatterMax.Get();
		bool const useFiringEffects = pTypeExt->AirburstWeapon_UseFiringEffects.Get(RulesExt::Global()->AirburstWeapon_UseFiringEffects);
		bool const headToTarget = pTypeExt->AirburstWeapon_HeadToTarget.Get(RulesExt::Global()->AirburstWeapon_HeadToTarget);
		int const radialFireSegments = pTypeExt->AirburstWeapon_RadialFireSegments;
		int cycledTargetIndex = 0;
		int radialFireCounter = 0;
		auto radialFire = RadialFireStruct {};

		if (allowRepeatTargets)
			cycledTargetIndex = random.RandomRanged(0, targets.Count - 1);

		for (int i = 0; i < clusterCount; ++i)
		{
			auto pTarget = pThis->Target;

			if (!splits)
			{
				pTarget = targets.GetItem(i);
			}
			else if (!pTarget || retargetAccuracy < random.RandomDouble())
			{
				if (targets.Count < 1)
					continue; // Check remaining retarget dice rolls instead of breaking out of loop.

				int index = allowRepeatTargets ? cycledTargetIndex : random.RandomRanged(0, targets.Count - 1);
				pTarget = targets.GetItem(index);

				if (pTarget == pSource)
				{
					if (random.RandomDouble() > retargetSelfProbability)
					{
						if (allowRepeatTargets)
						{
							++cycledTargetIndex %= targets.Count;
							index = cycledTargetIndex;
						}
						else
						{
							index = random.RandomRanged(0, targets.Count - 1);
						}

						pTarget = targets.GetItem(index);
					}
				}

				if (allowRepeatTargets)
					++cycledTargetIndex %= targets.Count;
				else
					targets.RemoveItem(index);
			}

			if (pTarget)
			{
				if (auto const pBullet = pTypeSplits->CreateBullet(pTarget, pSource, damage, pWH, speed, bright))
				{
					CoordStruct coords = location;
					const int minScatter = scatterMin;
					const int maxScatter = scatterMax;

					if (airburst && targetAsSource)
					{
						coords = pTarget->GetCoords();

						if (skipHeight)
							coords.Z = location.Z;
					}

					if (minScatter > 0 || maxScatter > 0)
					{
						const int distance = random.RandomRanged(scatterMin, scatterMax);
						coords = MapClass::GetRandomCoordsNear(coords, distance, false);
					}

					if (radialFireSegments > 0)
					{
						radialFire.Segments = radialFireSegments;
						radialFire.Index = radialFireCounter;
						radialFire.Direction = targetDir;
						radialFireCounter = (radialFireCounter + 1) % radialFireSegments;
					}

					auto const pNewBulletExt = BulletExt::Fetch(pBullet);
					pNewBulletExt->FirepowerMult = pBulletExt->FirepowerMult;
					pNewBulletExt->IsSplitFromAirburst = true;
					BulletExt::SimulatedFiringUnlimbo(pBullet, pOwner, pWeapon, coords, headToTarget, radialFire);
					BulletExt::SimulatedFiringEffects(pBullet, pOwner, nullptr, useFiringEffects, true);
				}
			}
		}
	}

	return SkipGameCode;
}

#pragma endregion

#pragma region AffectsUnderground

// In vanilla, only ground is allowed, and Ares added air and top.
// But it seems that underground and surface is also working fine?
DEFINE_HOOK(0x469453, BulletClass_Logics_TemporalUnderGround, 0x6)
{
	enum { NotOK = 0x469AA4, OK = 0x469475 };

	GET(FootClass*, pTarget, EAX);

	if (pTarget->InWhichLayer() != Layer::None)
		return OK;

	return NotOK;
}

DEFINE_HOOK(0x4899DA, MapClass_DamageArea_DamageUnderGround, 0x7)
{
	GET_STACK(const bool, isNullified, STACK_OFFSET(0xE0, -0xC9));
	GET_STACK(int, damage, STACK_OFFSET(0xE0, -0xBC));
	GET_STACK(CoordStruct*, pCrd, STACK_OFFSET(0xE0, -0xB8));
	GET_BASE(WarheadTypeClass*, pWH, 0xC);
	GET_BASE(TechnoClass*, pSrcTechno, 0x8);
	GET_BASE(HouseClass*, pSrcHouse, 0x14);
	GET_STACK(bool, hitted, STACK_OFFSET(0xE0, -0xC1)); // bHitted = true

	if (isNullified)
		return 0;

	auto const pWHExt = WarheadTypeExt::Fetch(pWH);

	if (!pWHExt->AffectsUnderground)
		return 0;

	const bool cylinder = pWHExt->CellSpread_Cylinder;
	const float spread = pWH->CellSpread * (float)Unsorted::LeptonsPerCell;

	for (auto const& pTechno : ScenarioExt::Global()->UndergroundTracker)
	{
		if (pTechno->InWhichLayer() == Layer::Underground // Layer.
			&& pTechno->IsAlive && !pTechno->IsIronCurtained()
			&& !pTechno->IsOnMap // Underground is not on map.
			&& !pTechno->InLimbo)
		{
			double dist = 0.0;
			auto const technoCoords = pTechno->GetCoords();

			if (cylinder)
				dist = CoordStruct { technoCoords.X - pCrd->X, technoCoords.Y - pCrd->Y, 0 }.Magnitude();
			else
				dist = technoCoords.DistanceFrom(*pCrd);

			if (dist <= spread)
			{
				pTechno->ReceiveDamage(&damage, (int)dist, pWH, pSrcTechno, false, false, pSrcHouse);
				hitted = true;
			}
		}
	}

	R->Stack8(STACK_OFFSET(0xE0, -0xC1), true);
	return 0;
}

#pragma endregion

#pragma region TemporalCellSpread

// A Temporal warhead normally warps exactly one unit. The engine's single call to
// TemporalClass::Fire (0x71AF20) is handed the bullet's target and the warhead's
// CellSpread is never reached, because this branch leaves for the end of the function
// before the area damage runs. CellSpread on such a warhead therefore did nothing at
// all; here every unit inside it is warped.
//
// The engine's own call is left where it is and still warps the unit the engine picked
// (the bullet's target, or the transport an infantry inside it stands for). That keeps
// every caster-side mechanism the engine has working for that unit untouched: it
// refuses to fire again at the unit the caster is warping, it releases the warp when the
// caster stops attacking, retargets, dies or is limboed, and it hands the victim over to
// the next caster in the chain. Only the *rest* of the radius is delivered from here,
// one TemporalClass per victim - the same sourceless delivery the AnySlot fix below
// introduced. Each victim's own AI drives its own instance, so one caster holds several
// units at once, and a victim another caster is already warping gets chained by Fire
// exactly the way vanilla chains two casters (a relay: the next instance takes the
// victim over once the current one lets go).
//
// One case has no unit for the engine to hand to Fire - a bullet aimed at a cell or at
// terrain, or one that lost its target. Vanilla calls Fire with a null target and warps
// nobody there, so this side does the whole delivery and afterwards promotes one victim
// into the caster's TemporalImUsing slot (see DeliverAll), which is the slot every
// engine release path is wired to.
//
// Who gets hit is decided by the warhead itself - WarheadTypeExt::CanTargetHouse is
// exactly AffectsOwner / AffectsAllies / AffectsEnemies / AffectsNeutral, the same
// filter the warhead's other per-target effects go through.
//
// CellSpread = 0 leaves everything to vanilla: both hooks below fall through untouched.

namespace TemporalCellSpread
{
	bool Applies(BulletClass* pBullet, TechnoClass* pOwner)
	{
		if (!pBullet || !pOwner)
			return false;

		const auto pWH = pBullet->WH;

		return pWH && pWH->Temporal && pWH->CellSpread > 0.0f;
	}

	// Whether the shot has to warp this victim itself. A unit that is already being warped - by
	// this caster or by another one - is left alone: TemporalClass::Fire does not take such a unit
	// over, it chains the new instance onto the warp that holds it as a helper, and the holder's
	// per-frame erase step then grows by the helper's (0x71AB10). That helper is a live pointer to
	// a unit whose warp belongs to somebody else, and the chain that erases the unit does not clear
	// a helper's target - letting go of one later dereferences a dead unit through whatever the
	// freed memory now holds (the wild jump that crashed 2026-10-15 22:07: LetGo's
	// `call [target_vt+0x2c]` at 0x71AC22 jumping into gamemd's data). Not warping a unit twice also
	// keeps it from being drained twice as fast as the rest of the radius.
	bool ShallDeliver(TechnoClass* pVictim)
	{
		return pVictim && !pVictim->TemporalTargetingMe;
	}

	// Warps one victim with an instance of its own. Fire bails out without touching the
	// instance when the engine's own gate (TemporalClass::CanWarpTarget: an iron curtain, an
	// occupied building, ...) refuses the target, so an instance it left empty is deleted
	// again instead of being kept registered and warping nobody.
	void Fire(TechnoClass* pOwner, TechnoClass* pTarget)
	{
		auto const pInstance = GameCreate<TemporalClass>(pOwner);
		pInstance->Fire(pTarget);

		if (!pInstance->Target)
			GameDelete(pInstance);
	}

	// Every unit in the warhead's radius that the warhead's own filter accepts.
	std::vector<TechnoClass*> GetVictims(BulletClass* pBullet, TechnoClass* pOwner)
	{
		const auto pWH = pBullet->WH;
		const auto pWHExt = WarheadTypeExt::Fetch(pWH);
		const auto pHouse = pOwner->Owner;
		std::vector<TechnoClass*> victims;

		for (auto const pTarget : Helpers::Alex::getCellSpreadItemsExt(
			pBullet->GetCoords(), pWH->CellSpread, true, pWHExt->CellSpread_Cylinder))
		{
			if (!pTarget || !pTarget->IsAlive || pTarget->InLimbo || pTarget->IsSinking)
				continue;

			if (!pWHExt->CanTargetHouse(pHouse, pTarget))
				continue;

			victims.push_back(pTarget);
		}

		return victims;
	}

	// Works out how long this shot's freeze lasts and what has to happen on the way, once, from the
	// victims the shot will hold itself. Two things come out of it:
	//
	//   * the caster's reload is pushed out to cover the whole freeze. That is what keeps it from
	//     firing again while the units this shot covered are still frozen: the engine's own "this
	//     techno is holding a warp" gate (0x741206) also wants TechnoClass +0x5A4 to be set, which
	//     is a locomotion-side field, so which units it actually holds back depends on the class -
	//     the reload timer is consulted by every class' firing path instead. It is the same timer
	//     Temporal.Interval and SweepFire use, and only ever extended.
	//   * with CellSpread.TemporalEraseMode=pooled, the combined erase time is left on the caster
	//     for the per-victim warps to pick up as they are created (TechnoExt::TemporalAreaEraseAt),
	//     so that they all leave together on that frame.
	//
	// A victim another caster is already warping is only chained onto that warp by the engine's own
	// relay, so its erasure is not this shot's to schedule and it is neither counted nor named in
	// the plan. A victim *this* caster is already warping is a different case: it is counted and
	// named like any other, and because there is no Fire of its own to pick the plan up, its
	// record is re-timed here instead (BeginTemporalFreezeFor). Skipping those victims was the bug
	// behind both halves of the "it fires again while someone is still frozen, and that someone
	// disappears before the others" report: they ended up with no schedule at all and fell back to
	// the engine's per-victim erase counter.
	//
	// Later arrivals are not counted: the victim list is the snapshot the detonation produced.
	//
	// The pooled total is only good for the frame it was worked out on - the shot's warps are all
	// created on that frame, the engine's own hit on the direct target included - and only for the
	// victims it counted, both of which are stamped on with it. A delivery that is not pooled
	// clears them.
	void ScheduleShot(BulletClass* pBullet, TechnoClass* pOwner, TechnoClass* pDirect,
		std::vector<TechnoClass*> const& victims)
	{
		const auto pExt = TechnoExt::TryFetch(pOwner);

		if (!pExt)
			return;

		pExt->TemporalAreaEraseAt = 0;
		pExt->TemporalAreaFrame = Unsorted::CurrentFrame;
		pExt->TemporalAreaVictims.clear();

		const auto pWH = pBullet->WH;

		if (!pWH)
			return;

		const auto pWHExt = WarheadTypeExt::Fetch(pWH);
		const bool pooled = pWHExt->CellSpread_TemporalEraseMode == TemporalEraseMode::Pooled;
		long long total = 0;
		int longest = 0;
		// How long the radius stays occupied by units this shot must not touch (already being
		// warped, by this caster or another one). The caster's reload is held back for at least
		// that long: firing again into a radius that still holds those units is what used to
		// leave the other caster's chain hung with helpers from this one.
		int alreadyWarped = 0;
		std::vector<TechnoClass*> reTimed;

		auto const weigh = [&](TechnoClass* pVictim)
			{
				if (!pVictim)
					return;

				int victimFrames = TechnoExt::GetTemporalEraseFrames(pOwner, pVictim, pWH);

				// A unit this shot is about to warp must never be left out of the plan because its
				// own estimate came out as nothing: GetTemporalEraseFrames goes to zero whenever the
				// warp's per-frame step truncates away, and that step is divided by a *per unit*
				// armour multiplier (AE multiplier x house x type x VeteranArmor). A unit whose step
				// comes out zero then had no recording at all and fell back to the engine's own
				// counter, so it left the radius on its own while the rest waited out the pool - a
				// thin unit erased first, a thick one still frozen afterwards. One frame stands in
				// for "counted, and scheduled with the rest of the shot".
				if (victimFrames <= 0)
					victimFrames = 1;

				if (const auto pHolder = pVictim->TemporalTargetingMe)
				{
					// Already warped, so this shot does not warp it (see ShallDeliver). The erasure
					// is not this shot's to schedule either - but the radius is not clear while the
					// unit is still standing in it.
					if (victimFrames > alreadyWarped)
						alreadyWarped = victimFrames;

					// A unit of this caster's own is the shot's business all the same: it leaves
					// with the shot, so its record is re-timed to it below (it gets no Fire of its
					// own to pick the plan up from).
					if (pHolder->Owner != pOwner)
						return;

					reTimed.push_back(pVictim);
				}

				total += victimFrames;

				if (victimFrames > longest)
					longest = victimFrames;

				// Only pooled mode reads the plan back, and only the victims it names pick it up.
				if (pooled)
					pExt->TemporalAreaVictims.push_back(pVictim->UniqueID);
			};

		if (pDirect)
			weigh(pDirect);

		for (auto const pVictim : victims)
		{
			if (pVictim != pDirect)
				weigh(pVictim);
		}

		// How long the shot's freeze lasts: pooled holds the radius for the combined time, while
		// per-unit victims go one after another, so the last one leaves after the longest of them.
		// A Temporal.Duration freeze hands every victim back on its own frame together, so the
		// window is what the shot lasts then - unless pooled cuts it short, which it does by
		// erasing every victim on its own frame.
		int shotFrames = 0;

		if (pooled)
		{
			shotFrames = static_cast<int>(total);
		}
		else if (pWHExt->Temporal_Duration > 0)
		{
			shotFrames = pWHExt->Temporal_Duration;
		}
		else
		{
			shotFrames = longest;
		}

		// Never nothing, or the caster goes straight back to firing on its ROF while the units
		// this shot covered are still frozen - and the shot that follows then finds every one of
		// them already warped and cannot schedule any of them.
		if (shotFrames <= 0)
			shotFrames = longest;

		// A radius that still holds units another caster is warping is not clear either: the shot
		// waits for the longer of the two rather than firing into it again. Without this, a radius
		// taken over entirely by one caster left every other caster firing on its ROF (42 probe
		// lines, 27 of them with nothing to schedule).
		if (alreadyWarped > shotFrames)
			shotFrames = alreadyWarped;

		// One frame past the freeze. The victims are erased on the frame the counter runs out, and
		// the caster's own AI runs before theirs, so without the extra frame it fires on that very
		// frame with the previous shot's units still on the field.
		//
		// What the engine itself still owed when this shot landed is captured first: the hold below
		// only ever lengthens the timer, and a freeze broken off mid-way (the player stops the
		// attack) must give back the freeze's part of it and not the weapon's own reload.
		pExt->TemporalRearmBaseFrame = Unsorted::CurrentFrame + Math::max(pOwner->RearmTimer.GetTimeLeft(), 0);
		TechnoExt::ExtendTemporalRearm(pOwner, shotFrames > 0 ? shotFrames + 1 : 0);

		if (pooled && total > 0)
		{
			pExt->TemporalAreaEraseAt = static_cast<int>(total);

			for (auto const pVictim : reTimed)
				pExt->BeginTemporalFreezeFor(pVictim);
		}
	}

	// Direct hit: the engine's own call picks pDirect up right after this hook returns, so
	// only the rest of the radius is delivered from here. The extras have to be fired before
	// that call - a Fire lets go of whatever the caster's slot holds, so an extra fired after
	// the engine's own would drop the direct target again.
	void DeliverExtras(BulletClass* pBullet, TechnoClass* pOwner, TechnoClass* pDirect)
	{
		const auto victims = GetVictims(pBullet, pOwner);
		ScheduleShot(pBullet, pOwner, pDirect, victims);

		for (auto const pTarget : victims)
		{
			if (pTarget != pDirect && ShallDeliver(pTarget))
				Fire(pOwner, pTarget);
		}
	}

	// The engine has no unit to hand to Fire (see the region comment). Every victim is held by
	// an instance of its own and one of them is promoted into the caster's TemporalImUsing slot,
	// because that slot is what the engine's own release paths act on, and letting go of it is
	// what releases the rest of the shot (see the LetGo hooks in Ext/Techno/Hooks.cpp).
	//
	// Which victim that is decides when the shot's attack is over: the engine refuses to fire
	// while the slot holds a warp, so the attack lasts exactly as long as the victim in it. If
	// the one promoted were simply the first the enumeration returns, the attack would end the
	// moment *that* victim is erased - and the caster would immediately fire a second shot at the
	// same ground while the rest of the radius is still frozen, stacking a second set of warps on
	// them. So the victim that outlives the others is the one promoted.
	//
	// Outliving the others is what the engine's own counter measures: TemporalClass::Fire sets
	// WarpRemaining to the victim's *type* Strength times ten (0x71AFA1-0x71AFB9), and every
	// victim of one shot loses the same per-frame step, because that step is read from the
	// caster's weapon rather than from the victim (0x71A876-0x71A888). The largest health is
	// therefore erased last - and Temporal.UseCurrentHealth decides whether that is the type's
	// Strength or the health the unit actually has. (A Temporal.Duration freeze ends every victim
	// on its own frame together, so the choice does not matter there.)
	//
	// The promotion is the last Fire of the shot on purpose: every TemporalClass::Fire lets go
	// of whatever the caster's slot holds before it takes its own target, so an instance fired
	// after the promoted one would drop the promoted victim again.
	//
	// Returns false when the warhead covers no eligible unit at all, which lets the caller leave
	// the engine's own path alone.
	//
	// promote is off when the engine has a unit to warp of its own: that unit (or whatever the
	// caster's slot already holds) is the shot's anchor then, and moving the slot onto one of these
	// victims would let go of it first.
	bool DeliverAll(BulletClass* pBullet, TechnoClass* pOwner, bool promote)
	{
		const auto victims = GetVictims(pBullet, pOwner);

		if (victims.empty())
			return false;

		ScheduleShot(pBullet, pOwner, nullptr, victims);

		// The victim that outlives the others - only one this shot may actually hold, so never a
		// unit another warp is already holding.
		TechnoClass* pPromoted = nullptr;

		if (promote)
		{
			for (auto const pVictim : victims)
			{
				if (!ShallDeliver(pVictim))
					continue;

				if (!pPromoted || TechnoExt::GetFreezeHealth(pVictim, pBullet->WH) > TechnoExt::GetFreezeHealth(pPromoted, pBullet->WH))
					pPromoted = pVictim;
			}
		}

		for (auto const pVictim : victims)
		{
			if (pVictim != pPromoted && ShallDeliver(pVictim))
				Fire(pOwner, pVictim);
		}

		// A caster without a Temporal instance of its own (TemporalWeapon.AnySlot off and the
		// warhead came from a weapon that never entered the type's slots) has nothing to promote
		// into; the whole radius then stays on instances of its own.
		if (pPromoted)
		{
			if (const auto pSlot = pOwner->TemporalImUsing)
				pSlot->Fire(pPromoted);
		}

		return true;
	}
}

// The same warhead detonating with nothing under it at all: the bullet has no target, so
// vanilla leaves for the end of the function one branch earlier and warps nobody. The units
// the warhead covers are still named by its CellSpread, so they are delivered here. The return
// address is spelled out instead of re-running the stolen branch, which keeps this independent
// of the flags.
DEFINE_HOOK(0x469447, BulletClass_Detonate_TemporalCellSpread_NoTarget, 0x6)
{
	enum { NoTarget = 0x469AA4, HasTarget = 0x46944D };

	GET(BulletClass*, pThis, ESI);

	if (pThis->Target || !TemporalCellSpread::Applies(pThis, pThis->Owner))
		return pThis->Target ? HasTarget : NoTarget;

	TemporalCellSpread::DeliverAll(pThis, pThis->Owner, true);

	return NoTarget;
}

#pragma endregion

#pragma region TemporalFromAnyWeaponSlot

// Vanilla creates a techno's Temporal instance (TemporalImUsing, +0x274) in
// TechnoClass::Init and only when weapon slot 0's warhead is Temporal. The detonation
// path right below then loads that instance and calls TemporalClass::Fire on it with
// no null check, so a Temporal warhead delivered through any other slot - or through
// a weapon that never entered the type's slots at all, such as an AirburstWeapon's -
// crashed at 0x71AF4D, an address gamemd.edb already documents as a known crash.
//
// Creating the instance here, on the first Temporal detonation, covers every path such
// a warhead can take. TemporalClass' constructor registers itself in
// TemporalClass::Array and draws no random numbers, and detonation is part of the
// synchronized simulation, so every machine creates the instance on the same frame.
//
// The stolen instruction (mov ecx,[ecx+0x274]) re-runs with ECX still holding the
// owner, so the call that follows it picks up the instance created here.
//
// EDI already holds the argument for that call (it was pushed at 0x4694BA), which is the
// target the engine is about to warp, or null when the bullet is aimed at a cell or at
// terrain and the engine has no unit to warp at all. A CellSpread Temporal warhead is
// delivered off that distinction; see the TemporalCellSpread region above.
DEFINE_HOOK(0x4694BB, BulletClass_Detonate_EnsureTemporal, 0x6)
{
	GET(BulletClass*, pThis, ESI);
	GET(TechnoClass*, pOwner, ECX);
	GET(TechnoClass*, pDirect, EDI);

	if (RulesExt::Global()->TemporalWeapon_AnySlot && pOwner)
	{
		// Remember which weapon produced this warhead. A Temporal warp's erase rate is
		// worked out from that weapon, but the engine re-derives it from the owner's
		// "current" weapon selection, which by the time the bullet lands no longer has
		// anything to do with the shot that was fired. See TechnoExt::TemporalWarpWeapon.
		if (pThis->WeaponType)
		{
			if (auto const pExt = TechnoExt::TryFetch(pOwner))
				pExt->TemporalWarpWeapon = pThis->WeaponType;
		}

		if (!pOwner->TemporalImUsing)
			pOwner->TemporalImUsing = GameCreate<TemporalClass>(pOwner);
	}

	if (!TemporalCellSpread::Applies(pThis, pOwner))
		return 0;

	// The engine is about to warp pDirect itself, so only the rest of the radius is delivered from
	// here and its call is left alone - but only while pDirect is free to be warped. Firing the
	// caster's slot at a unit that is already being warped does not take it over: it chains a
	// second instance onto that warp as a helper (and, when the warp is this caster's own, lets go
	// of it first). Either way it is a warp this shot has no business restructuring, so the whole
	// radius is delivered here and the engine's call skipped instead.
	if (pDirect && !pDirect->TemporalTargetingMe)
	{
		TemporalCellSpread::DeliverExtras(pThis, pOwner, pDirect);

		return 0;
	}

	// Either nothing for the engine to warp, or a unit that must not be warped again. In both cases
	// its own call is skipped: with no target it would let go of whatever the caster's slot holds
	// and then warp nobody, and with an already-warped target it would chain onto that warp.
	// Skipping it means the argument pushed at 0x4694BA is never popped (Fire is __thiscall
	// and pops it itself), hence the stack pointer adjustment - SyringeEx's ESPModification
	// feature, which this Phobos refuses to run without (see Phobos::ExeRun).
	//
	// The slot is only promoted when the engine had no unit of its own to warp: otherwise the warp
	// that is already running is the shot's anchor.
	if (pDirect == nullptr)
	{
		if (!TemporalCellSpread::DeliverAll(pThis, pOwner, true))
			return 0;
	}
	else
	{
		TemporalCellSpread::DeliverAll(pThis, pOwner, false);
	}

	R->ESP(R->ESP() + 4);
	return 0x4694C6;
}

#pragma endregion

#include <Ext/BuildingType/Body.h>
#include <Ext/Infantry/Body.h>

#include <BuildingClass.h>
#include <InputManagerClass.h>

DEFINE_HOOK(0x51B2BD, InfantryClass_UpdateTarget_IsControlledByHuman, 0x6)
{
	GET(InfantryClass*, pThis, ESI);
	GET(AbstractClass*, pTarget, EDI);

	return (!pTarget || pThis->Owner->IsControlledByHuman()) ? 0x51B33F : 0;
}

// Deploy case: DoAction(Deployed)
DEFINE_HOOK(0x520B3E, InfantryClass_DoingAI_DeployConvert_Deploy, 0x6)
{
	GET(InfantryClass*, pThis, ESI);
	auto const pExt = InfantryExt::Fetch(pThis);
	auto const pTypeExt = pExt->TypeExtData;

	if (pTypeExt->Convert_Deploy && !pExt->HasDeployConverted)
	{
		pExt->HasDeployConverted = true;
		pExt->HasUndeployConverted = false;
		TechnoExt::ConvertToType(pThis, pTypeExt->Convert_Deploy);
	}

	return 0;
}

// Undeploy case: DoAction(Ready)
DEFINE_HOOK(0x520B99, InfantryClass_DoingAI_DeployConvert_Undeploy, 0x6)
{
	GET(InfantryClass*, pThis, ESI);
	auto const pExt = InfantryExt::Fetch(pThis);
	auto const pTypeExt = pExt->TypeExtData;

	if (pTypeExt->Convert_Undeploy && !pExt->HasUndeployConverted)
	{
		pExt->HasUndeployConverted = true;
		pExt->HasDeployConverted = false;
		TechnoExt::ConvertToType(pThis, pTypeExt->Convert_Undeploy);
	}

	return 0;
}

// Reset mark when Deploy/Undeploy
DEFINE_HOOK(0x520E75, InfantryClass_DoingAI_DeployConvert_ResetFlags, 0x6)
{
	GET(InfantryClass*, pThis, ESI);
	const auto curSeq = pThis->SequenceAnim;

	if (curSeq != Sequence::Deploy && curSeq != Sequence::Undeploy)
	{
		auto const pExt = InfantryExt::Fetch(pThis);
		pExt->HasDeployConverted = false;
		pExt->HasUndeployConverted = false;
	}

	return 0;
}

#pragma region WhatActionObjectFix

namespace WhatActionObjectTemp
{
	bool Fire = false;
	bool Move = false;
}

DEFINE_HOOK(0x51E462, InfantryClass_WhatAction_ObjectClass_SkipBomb, 0x6)
{
	enum { Skip = 0x51E668, SkipBomb = 0x51E49E, CanBomb = 0x51E48F };

	GET(InfantryClass*, pThis, EDI);
	GET(ObjectClass*, pTarget, ESI);
	GET_STACK(const bool, ignoreForce, STACK_OFFSET(0x38, 0x8));

	WhatActionObjectTemp::Fire = !ignoreForce && InputManagerClass::Instance->IsForceFireKeyPressed();
	WhatActionObjectTemp::Move = !ignoreForce && InputManagerClass::Instance->IsForceMoveKeyPressed();

	if (!pThis->Type->Engineer)
		return Skip;

	if (pThis->Owner->IsControlledByCurrentPlayer()
		&& pTarget->AttachedBomb && pTarget->BombVisible)
	{
		if (WhatActionObjectTemp::Move)
			return SkipBomb;

		const int index = pThis->SelectWeapon(pTarget);
		const auto pWeaponType = pThis->GetWeapon(index)->WeaponType;

		return pWeaponType && pWeaponType->Warhead->BombDisarm ? CanBomb : SkipBomb;
	}

	return SkipBomb;
}

// An engineer carrying a real weapon may attack enemy units like any other armed
// infantry. Vanilla hardcodes
//   if (Type->Engineer && action == Action::Attack) return Action::NoMove;
// at 0x51E6B4, so an armed engineer can never get an attack cursor on a unit.
// Only engineers ever reach this point (0x51E6C2 diverts everyone else) and
// 0x51E6D8 and beyond contain no engineer special-casing, so from there an armed
// engineer behaves exactly like a plain armed infantry.
//
// Force-fired buildings are deliberately NOT handled here: deeper in the general
// path the engine rewrites any non-allied, capturable building back to
// Action::Capture (0x51EE91 -> 0x51EEEF), which no jump at this level can dodge.
// That case is rewritten at the outermost layer instead, see
// InfantryClass__WhatAction_Wrapper in Ext/Techno/Hooks.TargetEvaluation.cpp.
DEFINE_HOOK(0x51E6C4, InfantryClass_WhatAction_EngineerAttack, 0x5)
{
	enum { SkipEngineerAttackClamp = 0x51E6D8 };

	GET(InfantryClass*, pThis, EDI);
	GET(ObjectClass*, pTarget, ESI);
	GET(Action, baseAction, EBP);

	if (baseAction == Action::Attack
		&& pThis->Owner->IsControlledByCurrentPlayer()
		&& InfantryExt::HasAttackWeapon(pThis, pTarget))
	{
		return SkipEngineerAttackClamp;
	}

	return 0;
}

DEFINE_HOOK(0x51E4FB, InfantryClass_WhatAction_ObjectClass_EnigneerEnterBuilding, 0x6)
{
	enum { Skip = 0x51E668, Continue = 0x51E501 };

	GET(InfantryClass*, pThis, EDI);
	GET(BuildingClass*, pBuilding, ESI);
	GET(BuildingTypeClass*, pBuildingType, EAX);

	if (WhatActionObjectTemp::Fire)
		return Skip;

	const bool bridgeRepairHut = pBuildingType->BridgeRepairHut;

	if (!bridgeRepairHut && pThis->Owner->IsAlliedWith(pBuilding->Owner))
	{
		if (WhatActionObjectTemp::Move)
			return Skip;

		if (pBuilding->Health >= pBuildingType->Strength)
		{
			const auto pTypeExt = BuildingTypeExt::Fetch(pBuildingType);

			if (!pTypeExt->RubbleIntact && !pTypeExt->RubbleIntactRemove)
				return Skip;
		}
	}

	R->CL(bridgeRepairHut);
	return Continue;
}

DEFINE_HOOK(0x51EE6B, InfantryClass_WhatAction_ObjectClass_InfiltrateForceAttack, 0x6)
{
	return WhatActionObjectTemp::Fire ? 0x51F05E : 0;
}

DEFINE_HOOK(0x51ECC0, InfantryClass_WhatAction_ObjectClass_IsAreaFire, 0xA)
{
	enum { IsAreaFire = 0x51ECE5, NotAreaFire = 0x51ECEC };

	GET(InfantryClass*, pThis, EDI);
	GET(ObjectClass*, pObject, ESI);
	const int deployWeaponIdx = pThis->Type->DeployFireWeapon;
	const auto deployWeapon = pThis->GetWeapon(deployWeaponIdx >= 0 ? deployWeaponIdx : pThis->SelectWeapon(pObject))->WeaponType;

	return deployWeapon && deployWeapon->AreaFire ? IsAreaFire : NotAreaFire;
}

#pragma endregion

DEFINE_HOOK(0x7093F8, TechnoClass_709290_DeployWeapon, 0x5)
{
	enum { ReturnTrue = 0x70944F, ReturnFalse = 0x709449 };

	GET(TechnoClass*, pThis, ESI);

	if (const auto pInfantry = abstract_cast<InfantryClass*, true>(pThis))
	{
		const int deployWeaponIdx = pInfantry->Type->DeployFireWeapon;

		if (deployWeaponIdx >= 0)
		{
			const auto pWeaponStruct = pThis->GetWeapon(deployWeaponIdx);

			if (pWeaponStruct && pWeaponStruct->WeaponType && pWeaponStruct->WeaponType->AreaFire && deployWeaponIdx == pThis->SelectWeapon(pThis->Target))
				return ReturnFalse;
		}
		else
		{
			const auto pWeaponStruct = pThis->GetWeapon(pThis->SelectWeapon(pThis->Target));

			if (pWeaponStruct && pWeaponStruct->WeaponType && pWeaponStruct->WeaponType->AreaFire)
				return ReturnFalse;
		}
	}
	else
	{
		const int weaponIdx = pThis->IsNotSprayAttack();
		const auto pWeaponStruct = pThis->GetWeapon(weaponIdx);

		if (pWeaponStruct && pWeaponStruct->WeaponType && pWeaponStruct->WeaponType->AreaFire && weaponIdx == pThis->SelectWeapon(pThis->Target))
			return ReturnFalse;
	}

	return ReturnTrue;
}

// Skip incorrect retn to restore the auto deploy behavior of infantry
DEFINE_HOOK(0x522373, InfantryClass_ApproachTarget_InfantryAutoDeploy, 0x5)
{
	enum { Deploy = 0x522378 };
	GET(InfantryClass*, pThis, ESI);
	return InfantryTypeExt::Fetch(pThis->Type)->InfantryAutoDeploy.Get(RulesExt::Global()->InfantryAutoDeploy) ? Deploy : 0;
}

DEFINE_HOOK(0x51A002, InfantryClass_UpdatePosition_InfiltrateBuilding, 0x6)
{
	GET(InfantryClass*, pThis, ESI);
	GET(BuildingClass*, pBuilding, EDI);

	if (const auto pTag = pBuilding->AttachedTag)
		pTag->RaiseEvent(TriggerEvent::SpiedBy, pThis, CellStruct::Empty);

	if (const auto pTag = pBuilding->AttachedTag)
		pTag->RaiseEvent(TriggerEvent::SpyAsHouse, pThis, CellStruct::Empty);

	if (const auto pTag = pBuilding->AttachedTag)
		pTag->RaiseEvent(TriggerEvent::SpyAsInfantry, pThis, CellStruct::Empty);

	return 0;
}

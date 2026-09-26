#include "Body.h"

InfantryTypeExt::ExtContainer InfantryTypeExt::ExtMap;

// =============================
// load / save

// Reads one per-weapon firing animation override (Weapon%dFire).
// Returns -1 if unset or invalid, 0 for the primary animation group (FireUp/FireProne)
// and 1 for the secondary one (SecondaryFire/SecondaryProne).
static int ReadWeaponFireAnimation(INI_EX& exArtINI, const char* pArtSection, const char* pSequenceSection, int nWeaponIndex)
{
	char key[32];
	_snprintf_s(key, sizeof(key), "Weapon%dFire", nWeaponIndex + 1);

	const char* pSection = nullptr;

	if (exArtINI.ReadString(pArtSection, key) && !exArtINI.empty())
		pSection = pArtSection;
	else if (exArtINI.ReadString(pSequenceSection, key) && !exArtINI.empty())
		pSection = pSequenceSection;
	else
		return -1;

	const char* pValue = exArtINI.value();

	if (!_strcmpi(pValue, "Primary") || !_strcmpi(pValue, "FireUp") || !_strcmpi(pValue, "Main") || !_strcmpi(pValue, "0"))
		return 0;

	if (!_strcmpi(pValue, "Secondary") || !_strcmpi(pValue, "SecondaryFire") || !_strcmpi(pValue, "Sub") || !_strcmpi(pValue, "1"))
		return 1;

	Debug::INIParseFailed(pSection, key, pValue, "Expected Primary or Secondary");
	return -1;
}

void InfantryTypeExt::LoadFromINIFile(CCINIClass* const pINI)
{
	TechnoTypeExt::LoadFromINIFile(pINI);

	auto pThis = this->OwnerObject();
	const char* pSection = pThis->ID;
	INI_EX exINI(pINI);

	this->Slaved_OwnerWhenMasterKilled.Read(exINI, pSection, "Slaved.OwnerWhenMasterKilled");
	this->SlavesFreeSound.Read(exINI, pSection, "SlavesFreeSound");
	this->NotHuman_RandomDeathSequence.Read(exINI, pSection, "NotHuman.RandomDeathSequence");
	this->DefaultDisguise.Read(exINI, pSection, "DefaultDisguise");
	this->ProneSpeed.Read(exINI, pSection, "ProneSpeed");
	this->InfantryAutoDeploy.Read(exINI, pSection, "InfantryAutoDeploy");

	const auto pArtINI = &CCINIClass::INI_Art;
	INI_EX exArtINI(pArtINI);
	auto pArtSection = pThis->ImageFile;

	this->ParseBurstFLHs(exArtINI, pArtSection, this->DeployedWeaponBurstFLHs, this->EliteDeployedWeaponBurstFLHs, "Deployed");
	this->ParseBurstFLHs(exArtINI, pArtSection, this->CrouchedWeaponBurstFLHs, this->EliteCrouchedWeaponBurstFLHs, "Prone");

	this->OnlyUseLandSequences.Read(exArtINI, pArtSection, "OnlyUseLandSequences");
	this->SecondaryFireSequenceLandOnly.Read(exArtINI, pArtSection, "SecondaryFireSequenceLandOnly");
	this->PronePrimaryFireFLH.Read(exArtINI, pArtSection, "PronePrimaryFireFLH");
	this->ProneSecondaryFireFLH.Read(exArtINI, pArtSection, "ProneSecondaryFireFLH");
	this->DeployedPrimaryFireFLH.Read(exArtINI, pArtSection, "DeployedPrimaryFireFLH");
	this->DeployedSecondaryFireFLH.Read(exArtINI, pArtSection, "DeployedSecondaryFireFLH");

	// Resolve the [<image>Sequence] section so Weapon%dFire can also be put next to the sequence definitions.
	char sequenceSection[0x20];

	if (exArtINI.ReadString(pArtSection, "Sequence") && !exArtINI.empty())
		strncpy_s(sequenceSection, exArtINI.value(), _TRUNCATE);
	else
		_snprintf_s(sequenceSection, sizeof(sequenceSection), "%sSequence", pArtSection);

	const int weaponCount = Math::max(pThis->WeaponCount, 2);
	this->WeaponFireAnimations.assign(weaponCount, -1);

	for (int i = 0; i < weaponCount; i++)
		this->WeaponFireAnimations[i] = ReadWeaponFireAnimation(exArtINI, pArtSection, sequenceSection, i);
}

bool InfantryTypeExt::IsSecondaryFireAnim(int nWeaponIndex) const
{
	if (nWeaponIndex >= 0 && static_cast<size_t>(nWeaponIndex) < this->WeaponFireAnimations.size())
	{
		const int value = this->WeaponFireAnimations[nWeaponIndex];

		if (value >= 0)
			return value != 0;
	}

	return this->IsSecondary(nWeaponIndex);
}

template <typename T>
void InfantryTypeExt::Serialize(T& Stm)
{
	Stm
		.Process(this->Slaved_OwnerWhenMasterKilled)
		.Process(this->SlavesFreeSound)
		.Process(this->NotHuman_RandomDeathSequence)
		.Process(this->DefaultDisguise)
		.Process(this->ProneSpeed)
		.Process(this->OnlyUseLandSequences)
		.Process(this->SecondaryFireSequenceLandOnly)
		.Process(this->PronePrimaryFireFLH)
		.Process(this->ProneSecondaryFireFLH)
		.Process(this->DeployedPrimaryFireFLH)
		.Process(this->DeployedSecondaryFireFLH)
		.Process(this->CrouchedWeaponBurstFLHs)
		.Process(this->EliteCrouchedWeaponBurstFLHs)
		.Process(this->DeployedWeaponBurstFLHs)
		.Process(this->EliteDeployedWeaponBurstFLHs)
		.Process(this->InfantryAutoDeploy)
		.Process(this->WeaponFireAnimations)
		;
}

void InfantryTypeExt::LoadFromStream(PhobosStreamReader& Stm)
{
	TechnoTypeExt::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void InfantryTypeExt::SaveToStream(PhobosStreamWriter& Stm)
{
	TechnoTypeExt::SaveToStream(Stm);
	this->Serialize(Stm);
}

// =============================
// container

InfantryTypeExt::ExtContainer::ExtContainer() : Container("InfantryTypeClass") { }
InfantryTypeExt::ExtContainer::~ExtContainer() = default;

// =============================
// container hooks

DEFINE_HOOK(0x5236B3, InfantryTypeClass_CTOR, 0xA)
{
	GET(InfantryTypeClass*, pItem, ESI);

	InfantryTypeExt::ExtMap.Allocate(pItem);

	return 0;
}

// The extension chain is read at the end of each concrete type class's LoadFromINI,
// once every native field - inherited and own alike - has been parsed.
DEFINE_HOOK(0x52473F, InfantryTypeClass_LoadFromINI, 0x5)
{
	GET(InfantryTypeClass*, pItem, ESI);
	GET_STACK(CCINIClass*, pINI, 0xD0);

	if (auto const pExt = InfantryTypeExt::TryFetch(pItem))
		pExt->LoadFromINI(pINI);

	return 0;
}

// Late in every destructor body of the class, right before it chains into the
// base destructor: the last point where the extension is no longer used.
DEFINE_HOOK_AGAIN(0x524E90, InfantryTypeClass_DTOR, 0xE)
DEFINE_HOOK(0x523AF0, InfantryTypeClass_DTOR, 0xE)
{
	GET(InfantryTypeClass*, pItem, ESI);

	InfantryTypeExt::ExtMap.Remove(pItem);

	return 0;
}

#pragma once

#include <Ext/TechnoType/Body.h>
#include <InfantryTypeClass.h>

// Concrete leaf extension for InfantryTypeClass (empty).
class InfantryTypeExt final : public TechnoTypeExt
{
public:
	using base_type = InfantryTypeClass;

	static constexpr DWORD Canary = 0xF5F6F7F8;

	Valueable<SlaveChangeOwnerType> Slaved_OwnerWhenMasterKilled;
	NullableIdx<VocClass> SlavesFreeSound;
	Nullable<bool> NotHuman_RandomDeathSequence;
	Valueable<InfantryTypeClass*> DefaultDisguise;
	Nullable<double> ProneSpeed;
	Nullable<bool> OnlyUseLandSequences;
	Nullable<bool> SecondaryFireSequenceLandOnly;
	Nullable<CoordStruct> PronePrimaryFireFLH;
	Nullable<CoordStruct> ProneSecondaryFireFLH;
	Nullable<CoordStruct> DeployedPrimaryFireFLH;
	Nullable<CoordStruct> DeployedSecondaryFireFLH;
	std::vector<std::vector<CoordStruct>> CrouchedWeaponBurstFLHs;
	std::vector<std::vector<CoordStruct>> EliteCrouchedWeaponBurstFLHs;
	std::vector<std::vector<CoordStruct>> DeployedWeaponBurstFLHs;
	std::vector<std::vector<CoordStruct>> EliteDeployedWeaponBurstFLHs;
	Nullable<bool> InfantryAutoDeploy;

	// Per weapon index override of which firing animation group is played.
	// -1 = unset (fall back to TechnoTypeExt::IsSecondary), 0 = primary (FireUp/FireProne), 1 = secondary (SecondaryFire/SecondaryProne).
	ValueableVector<int> WeaponFireAnimations;

	explicit InfantryTypeExt(InfantryTypeClass* const OwnerObject) : TechnoTypeExt(OwnerObject)
		, Slaved_OwnerWhenMasterKilled { SlaveChangeOwnerType::Killer }
		, SlavesFreeSound {}
		, NotHuman_RandomDeathSequence {}
		, DefaultDisguise {}
		, ProneSpeed {}
		, OnlyUseLandSequences {}
		, SecondaryFireSequenceLandOnly {}
		, PronePrimaryFireFLH {}
		, ProneSecondaryFireFLH {}
		, DeployedPrimaryFireFLH {}
		, DeployedSecondaryFireFLH {}
		, InfantryAutoDeploy {}
		, WeaponFireAnimations {}
	{ }

	InfantryTypeClass* OwnerObject() const
	{
		return static_cast<InfantryTypeClass*>(this->GetAttachedObject());
	}

	class ExtContainer final : public Container<InfantryTypeExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	static InfantryTypeExt* Fetch(const InfantryTypeClass* pThis)
	{
		return AbstractExt::Fetch<InfantryTypeExt>(pThis);
	}

	static InfantryTypeExt* TryFetch(const InfantryTypeClass* pThis)
	{
		return AbstractExt::TryFetch<InfantryTypeExt>(pThis);
	}

	virtual void LoadFromINIFile(CCINIClass* pINI) override;
	virtual void LoadFromStream(PhobosStreamReader& Stm) override;
	virtual void SaveToStream(PhobosStreamWriter& Stm) override;

	// Returns whether the weapon index plays the secondary firing animation group.
	// Falls back to TechnoTypeExt::IsSecondary when no per-weapon override is set.
	bool IsSecondaryFireAnim(int nWeaponIndex) const;


private:
	template <typename T>
	void Serialize(T& Stm);
};

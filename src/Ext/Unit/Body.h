#pragma once

#include <Ext/Foot/Body.h>
#include <Ext/UnitType/Body.h>
#include <TargetClass.h>
#include <UnitClass.h>

// Carryall state of one unit. None is the resting state. The values are written to
// the savegame, so new ones are only ever appended.
//
// Every transition runs from UnitClass::AI on every machine and reads only the engine
// order fields (Mission/Target/Destination) plus the shared world state, so all
// machines run the same sequence without a custom event of our own.
enum class CarryallState : uint8_t
{
	None = 0,   // not pursuing and not carrying anything
	Pursuing,   // carrier: flying to the vehicle it has been ordered to pick up
	Landing,    // carrier: over the vehicle, coming down on it
	Carrying,   // carrier: holding a payload
	Dropping,   // carrier: putting the payload down on CarryallDropCell
	Ascending   // carrier: climbing back to CarryallStartHeight
};

// Concrete leaf extension for UnitClass. Empty for now: all techno-level data lives
// in TechnoExt; this leaf only exists so a unit's extension has its own
// concrete type (TechnoExt itself is never instantiated).
class UnitExt final : public FootExt
{
public:
	using base_type = UnitClass;

	static constexpr DWORD Canary = 0xE1E2E3E4;

	int SubterraneanHarvStatus; // 0 = none, 1 = created, 2 = out from factory
	AbstractClass* SubterraneanHarvRallyPoint;
	bool ReceiveDamage;
	CDTimerClass DeployFireTimer;
	bool KeepTargetOnMove;
	CDTimerClass SimpleDeployerAnimationTimer;
	bool IsBurrowed;
	bool UndergroundTracked;

	// Carryall: where the unit stands, and the vehicle it is dealing with.
	//
	// Both ends of that link are engine TargetClass references rather than raw pointers.
	// The engine resolves them by id and returns null once the object is gone
	// (TargetClass::As_Unit, 0x6E79B0), so a vehicle destroyed in the middle of a pick-up
	// can never leave the other end dereferencing freed memory, which is what a raw
	// pointer did here before.
	//
	// CarryallCarrier is only ever written by our own attach step, never by the vanilla
	// aircraft carryall: an empty value therefore means "not ours" and must never be
	// read as "the carrier died" - that assumption killed vanilla cargo before.
	CarryallState CarryallStatus;
	TargetClass CarryallPayload;     // carrier side: the vehicle it holds / is after
	TargetClass CarryallCarrier;     // payload side: set only when OUR carrier hooked us

	// Raw pointers, not serialized. A limboed payload is not resolvable through the
	// engine's id index (TargetClass), so the live pointer is kept here and only used
	// while its Fetch_ID still matches CarryallPayload.
	UnitClass* CarryallPayloadRaw;
	UnitClass* CarryallCarrierRaw;
	CellStruct CarryallDropCell;     // carrier side: the cell chosen for the drop
	CellStruct CarryallApproachCell; // carrier side: cell the approach order was last given for
	int CarryallStartHeight;         // carrier side: altitude the descent started from
	CoordStruct CarryallTargetCoords;// carrier side: where the payload stood when the dive began
	bool CarryallDescending;         // carrier side: committed to a descent
	int CarryallVerticalSpeed;       // carrier side: current vertical speed (leptons/frame)

	std::vector<RecoilData> ExtraTurretRecoil;
	std::vector<RecoilData> ExtraBarrelRecoil;

	explicit UnitExt(UnitClass* const OwnerObject) : FootExt(OwnerObject)
		, SubterraneanHarvStatus { 0 }
		, SubterraneanHarvRallyPoint { nullptr }
		, ReceiveDamage { false }
		, DeployFireTimer {}
		, KeepTargetOnMove { false }
		, SimpleDeployerAnimationTimer {}
		, IsBurrowed { false }
		, UndergroundTracked { false }
		, CarryallStatus { CarryallState::None }
		, CarryallPayload {}
		, CarryallCarrier {}
		, CarryallPayloadRaw { nullptr }
		, CarryallCarrierRaw { nullptr }
		, CarryallDropCell { CellStruct::Empty }
		, CarryallApproachCell { CellStruct::Empty }
		, CarryallStartHeight { 0 }
		, CarryallTargetCoords { CoordStruct::Empty }
		, CarryallDescending { false }
		, CarryallVerticalSpeed { 0 }
		, ExtraTurretRecoil {}
		, ExtraBarrelRecoil {}
	{ }

	virtual ~UnitExt() override;

	virtual bool IsBurrowedState() const override { return this->IsBurrowed; }

	void UpdateSubterraneanHarvester();
	void UpdateKeepTargetOnMove();
	void DepletedAmmoActions();
	void InitializeRecoilData();
	void UpdateRecoilData();
	void RecordRecoilData();

	static UnitClass* Deployer;

	static bool CannotMove(UnitClass* pThis);
	static bool HasAmmoToDeploy(UnitClass* pThis);
	static void HandleOnDeployAmmoChange(UnitClass* pThis, int maxAmmoOverride = -1);
	static bool SimpleDeployerAllowedToDeploy(UnitClass* pThis, bool defaultValue, bool alwaysCheckLandTypes);
	static bool CanDeployIntoBuilding(UnitClass* pThis, bool noDeploysIntoDefaultValue = false);
	static UnitTypeClass* GetUnitTypeExtra(UnitClass* pUnit, UnitTypeExt* pData);

	UnitClass* OwnerObject() const
	{
		return static_cast<UnitClass*>(this->GetAttachedObject());
	}

	// a unit's type extension is always the UnitTypeExt leaf
	UnitTypeExt* GetTypeExtData() const
	{
		return static_cast<UnitTypeExt*>(this->TypeExtData);
	}

	class ExtContainer final : public Container<UnitExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	static UnitExt* Fetch(const UnitClass* pThis)
	{
		return AbstractExt::Fetch<UnitExt>(pThis);
	}

	static UnitExt* TryFetch(const UnitClass* pThis)
	{
		return AbstractExt::TryFetch<UnitExt>(pThis);
	}

	virtual void LoadFromStream(PhobosStreamReader& Stm) override;
	virtual void SaveToStream(PhobosStreamWriter& Stm) override;

private:
	template <typename T>
	void Serialize(T& Stm);
};

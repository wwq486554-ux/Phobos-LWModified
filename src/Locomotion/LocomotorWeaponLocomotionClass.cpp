#include "LocomotorWeaponLocomotionClass.h"

#include <Utilities/Debug.h>

#include <AnimClass.h>
#include <FootClass.h>
#include <RulesClass.h>
#include <WarheadTypeClass.h>
#include <Misc/SyncLogging.h>

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Trigger-layer scratch space.
//
// The 0x4696CE hook fills these in right before it calls ImbueLocomotor and
// clears them right after. Begin_Piggyback is called synchronously inside that
// call, copies everything into its own members, and never reads them again.
// ---------------------------------------------------------------------------
CLSID LocomotorWeapon::CurrentRequestedCLSID { };
LocomotorWeaponConfig LocomotorWeapon::CurrentConfig { };
bool LocomotorWeapon::Applying = false;

bool LocomotorWeapon::IsJumpjetCLSID(const CLSID& clsid)
{
	return clsid == LocomotionClass::CLSIDs::Jumpjet;
}

bool LocomotorWeapon::IsDropPodCLSID(const CLSID& clsid)
{
	return clsid == LocomotionClass::CLSIDs::Droppod;
}

bool LocomotorWeapon::IsNullCLSID(const CLSID& clsid)
{
	return clsid == CLSID { };
}

namespace
{
	// A "Stuck:" report is written only after the effect has been active for
	// this many frames (~10 s at 60 fps) and then at most once per interval, so
	// that a genuinely stuck victim is visible in debug.log without flooding it.
	constexpr int StuckReportDelay = 600;
	constexpr int StuckReportInterval = 300;

	// How long we refuse to read "IsAttackedByLocomotor == 0" as a release while
	// the engine is still arming us (see WantsToEnd). The engine polls us inside
	// the same frame it applies us, so a couple of frames of grace is enough.
	constexpr int ArmedGraceFrames = 2;

	// Values stored in LocomotorWeaponLocomotionClass::EndReason. Only used by the
	// "End:" log line, so that a bug report tells us straight away why it ended.
	constexpr int EndReasonNone = 0;
	constexpr int EndReasonFirerReleased = 1;
	constexpr int EndReasonFlagCleared = 2;
	constexpr int EndReasonTimeout = 3;
	constexpr int EndReasonArrival = 4;
	constexpr int EndReasonTargetGone = 5;

	const char* EndReasonLabel(int reason)
	{
		switch (reason)
		{
		case EndReasonFirerReleased:
			return "firer-released(source-cleared)";
		case EndReasonFlagCleared:
			return "firer-released(flag-cleared)";
		case EndReasonTimeout:
			return "timeout";
		case EndReasonArrival:
			return "arrival";
		case EndReasonTargetGone:
			return "target-gone";
		default:
			return "none";
		}
	}

	// Small rotating buffer so that several labels can be used in one printf
	// call. Must be at least as large as the widest diagnostic line below.
	constexpr int LabelBufferCount = 16;
	constexpr int LabelBufferSize = 96;
	char LabelBuffers[LabelBufferCount][LabelBufferSize];
	int LabelBufferIndex = 0;

	char* NextLabelBuffer()
	{
		char* const pBuffer = LabelBuffers[LabelBufferIndex];
		LabelBufferIndex = (LabelBufferIndex + 1) % LabelBufferCount;
		pBuffer[0] = '\0';

		return pBuffer;
	}
}

const char* LocomotorWeapon::TargetLabel(FootClass* pTarget)
{
	char* const pBuffer = NextLabelBuffer();

	if (!pTarget)
	{
		strcpy_s(pBuffer, LabelBufferSize, "<none>");
		return pBuffer;
	}

	const char* pTypeId = "<notype>";

	if (auto const pType = pTarget->GetTechnoType())
	{
		if (pType->ID)
			pTypeId = pType->ID;
	}

	sprintf_s(pBuffer, LabelBufferSize, "%s#%d", pTypeId, pTarget->UniqueID);

	return pBuffer;
}

const char* LocomotorWeapon::TechnoLabel(const TechnoClass* pTechno)
{
	char* const pBuffer = NextLabelBuffer();

	if (!pTechno)
	{
		strcpy_s(pBuffer, LabelBufferSize, "<none>");
		return pBuffer;
	}

	const char* pTypeId = "<notype>";

	if (auto const pType = pTechno->GetTechnoType())
	{
		if (pType->ID)
			pTypeId = pType->ID;
	}

	sprintf_s(pBuffer, LabelBufferSize, "%s#%d", pTypeId, pTechno->UniqueID);

	return pBuffer;
}

const char* LocomotorWeapon::PointerLabel(const void* pPointer)
{
	char* const pBuffer = NextLabelBuffer();

	if (!pPointer)
	{
		strcpy_s(pBuffer, LabelBufferSize, "null");
	}
	else
	{
		sprintf_s(pBuffer, LabelBufferSize, "%08X", reinterpret_cast<unsigned int>(pPointer));
	}

	return pBuffer;
}

// ---------------------------------------------------------------------------
// Post-release victim watch (diagnostics only, see the header).
// ---------------------------------------------------------------------------
namespace
{
	struct TrackedVictim
	{
		DWORD UniqueID { 0 };
		int WatchUntilFrame { 0 };
		int LastProbeFrame { 0 };
	};

	TrackedVictim TrackedVictims[LocomotorWeapon::TrackedVictimCount] { };

	TrackedVictim* FindTrackedVictim(DWORD uniqueID)
	{
		for (auto& entry : TrackedVictims)
		{
			if (entry.UniqueID == uniqueID && Unsorted::CurrentFrame <= entry.WatchUntilFrame)
				return &entry;
		}

		return nullptr;
	}
}

void LocomotorWeapon::TrackVictim(const AbstractClass* pVictim)
{
	if (!pVictim)
		return;

	const DWORD uniqueID = pVictim->UniqueID;
	const int frame = Unsorted::CurrentFrame;
	TrackedVictim* pSlot = nullptr;

	for (auto& entry : TrackedVictims)
	{
		if (entry.UniqueID == uniqueID)
		{
			pSlot = &entry;
			break;
		}

		// Otherwise take the entry whose window expires first.
		if (!pSlot || entry.WatchUntilFrame < pSlot->WatchUntilFrame)
			pSlot = &entry;
	}

	pSlot->UniqueID = uniqueID;
	pSlot->WatchUntilFrame = frame + TrackedVictimFrames;
	pSlot->LastProbeFrame = frame - TrackedVictimProbeInterval;
}

bool LocomotorWeapon::IsTrackedVictim(const AbstractClass* pVictim, bool* pProbeDue)
{
	if (pProbeDue)
		*pProbeDue = false;

	if (!pVictim)
		return false;

	auto const pEntry = FindTrackedVictim(pVictim->UniqueID);

	if (!pEntry)
		return false;

	if (pProbeDue)
	{
		const int frame = Unsorted::CurrentFrame;

		if (frame - pEntry->LastProbeFrame >= TrackedVictimProbeInterval)
		{
			pEntry->LastProbeFrame = frame;
			*pProbeDue = true;
		}
	}

	return true;
}

// Defaults for the modes this class moves itself. Kept in leptons / leptons per
// frame; LocomotorWeapon.* can override all of them.
namespace
{
	constexpr int DefaultFlightHeight = 768;  // ~3 cells
	constexpr int DefaultClimbRate = 24;
	constexpr int DefaultDescendRate = 32;
	constexpr int DefaultWarpDelay = 0;

	// Stable pseudo-caller for the synchronization log. MakeCallerRelative()
	// rebases anything inside the Phobos module, so both machines log the same
	// value regardless of where the DLL was loaded.
	int LocomotorWeaponSyncLogAnchor = 0;

	unsigned int GetSyncLogCaller()
	{
		return reinterpret_cast<unsigned int>(&LocomotorWeaponSyncLogAnchor);
	}

	// Moves v one step towards target without overshooting.
	void StepTowards(int& v, int target, int step)
	{
		if (v < target)
			v = std::min(v + step, target);
		else if (v > target)
			v = std::max(v - step, target);
	}
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------
HRESULT LocomotorWeaponLocomotionClass::QueryInterface(REFIID iid, LPVOID* ppvObject)
{
	if (!ppvObject)
		return E_POINTER;

	HRESULT hr = LocomotionClass::QueryInterface(iid, ppvObject);

	if (hr != E_NOINTERFACE)
		return hr;

	if (iid == __uuidof(IPiggyback))
	{
		*ppvObject = static_cast<IPiggyback*>(this);
		this->AddRef();
		return S_OK;
	}

	*ppvObject = nullptr;
	return E_NOINTERFACE;
}

// ---------------------------------------------------------------------------
// IPersist / IPersistStream
// ---------------------------------------------------------------------------
HRESULT LocomotorWeaponLocomotionClass::GetClassID(CLSID* pClassID)
{
	if (!pClassID)
		return E_POINTER;

	*pClassID = __uuidof(LocomotorWeaponLocomotionClass);

	return S_OK;
}

HRESULT LocomotorWeaponLocomotionClass::Load(IStream* pStm)
{
	HRESULT hr = LocomotionClass::Load(pStm);

	if (FAILED(hr))
		return hr;

	// The stale pointers below belong to the previous session; drop them and
	// refresh the vtable pointers, which are meaningless after a reload.
	this->Piggybacker.Detach();
	this->Mover.Detach();

	new (this) LocomotorWeaponLocomotionClass(noinit_t());

	bool present = false;

	hr = pStm->Read(&present, sizeof(present), nullptr);
	if (FAILED(hr) || !present)
		return hr;

	hr = OleLoadFromStream(pStm, __uuidof(ILocomotion), reinterpret_cast<LPVOID*>(&this->Piggybacker));
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&present, sizeof(present), nullptr);
	if (FAILED(hr))
		return hr;

	if (present)
	{
		hr = OleLoadFromStream(pStm, __uuidof(ILocomotion), reinterpret_cast<LPVOID*>(&this->Mover));
		if (FAILED(hr))
			return hr;
	}

	hr = pStm->Read(&this->RequestedCLSID, sizeof(this->RequestedCLSID), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->Config, sizeof(this->Config), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->Timer.StartTime, sizeof(this->Timer.StartTime), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->Timer.TimeLeft, sizeof(this->Timer.TimeLeft), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->DestinationCached, sizeof(this->DestinationCached), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->HasDestination, sizeof(this->HasDestination), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->LastIssuedMoverDestination, sizeof(this->LastIssuedMoverDestination), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->HasIssuedMoverDestination, sizeof(this->HasIssuedMoverDestination), nullptr);
	if (FAILED(hr))
		return hr;

	// Own-movement state (air / meteor / warp).
	hr = pStm->Read(&this->Phase, sizeof(this->Phase), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->GroundZ, sizeof(this->GroundZ), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->FlightZ, sizeof(this->FlightZ), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->FallDistance, sizeof(this->FallDistance), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->Arrived, sizeof(this->Arrived), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->ImpactApplied, sizeof(this->ImpactApplied), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Read(&this->WarpDone, sizeof(this->WarpDone), nullptr);
	if (FAILED(hr))
		return hr;

	// Nothing to re-link here: LocomotionClass::Load registered this
	// locomotor's Owner/LinkedTo with the swizzle manager, which zeroes both
	// fields and fills them in once the whole savegame has been read. The very
	// same happens for the Mover and the Piggybacker (they serialized
	// themselves through LocomotionClass::Save as well), so writing
	// this->LinkedTo into them at this point would only store the placeholder
	// value. See Link_To_Object for the case where the engine links this
	// locomotor to its object again after the load.

	return hr;
}

HRESULT LocomotorWeaponLocomotionClass::Save(IStream* pStm, BOOL fClearDirty)
{
	HRESULT hr = LocomotionClass::Save(pStm, fClearDirty);

	if (FAILED(hr))
		return hr;

	bool present = this->Piggybacker != nullptr;
	hr = pStm->Write(&present, sizeof(present), nullptr);
	if (FAILED(hr) || !present)
		return hr;

	IPersistStreamPtr piggyPersist(this->Piggybacker);
	hr = OleSaveToStream(piggyPersist, pStm);
	if (FAILED(hr))
		return hr;

	present = this->Mover != nullptr;
	hr = pStm->Write(&present, sizeof(present), nullptr);
	if (FAILED(hr) || !present)
		return hr;

	IPersistStreamPtr moverPersist(this->Mover);
	hr = OleSaveToStream(moverPersist, pStm);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->RequestedCLSID, sizeof(this->RequestedCLSID), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->Config, sizeof(this->Config), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->Timer.StartTime, sizeof(this->Timer.StartTime), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->Timer.TimeLeft, sizeof(this->Timer.TimeLeft), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->DestinationCached, sizeof(this->DestinationCached), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->HasDestination, sizeof(this->HasDestination), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->LastIssuedMoverDestination, sizeof(this->LastIssuedMoverDestination), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->HasIssuedMoverDestination, sizeof(this->HasIssuedMoverDestination), nullptr);
	if (FAILED(hr))
		return hr;

	// Own-movement state (air / meteor / warp).
	hr = pStm->Write(&this->Phase, sizeof(this->Phase), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->GroundZ, sizeof(this->GroundZ), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->FlightZ, sizeof(this->FlightZ), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->FallDistance, sizeof(this->FallDistance), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->Arrived, sizeof(this->Arrived), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->ImpactApplied, sizeof(this->ImpactApplied), nullptr);
	if (FAILED(hr))
		return hr;

	hr = pStm->Write(&this->WarpDone, sizeof(this->WarpDone), nullptr);
	if (FAILED(hr))
		return hr;

	return hr;
}

// ---------------------------------------------------------------------------
// Mover plumbing
// ---------------------------------------------------------------------------
ILocomotion* LocomotorWeaponLocomotionClass::GetMover()
{
	return this->Mover.GetInterfacePtr();
}

ILocomotion* LocomotorWeaponLocomotionClass::GetQuerySource()
{
	if (this->Mover)
		return this->Mover.GetInterfacePtr();

	return this->Piggybacker.GetInterfacePtr();
}

void LocomotorWeaponLocomotionClass::ResolveMode()
{
	if (this->Config.Mode != LocoWeaponMode::Auto)
		return;

	// DropPod is instantiated by the game as a real locomotor that crashes
	// vehicles and IEs aircraft, so it never gets a Mover.
	if (LocomotorWeapon::IsDropPodCLSID(this->RequestedCLSID))
		this->Config.Mode = LocoWeaponMode::Meteor;
	else
		this->Config.Mode = LocoWeaponMode::Mover;
}

void LocomotorWeaponLocomotionClass::CreateMover()
{
	if (this->Mover || LocomotorWeapon::IsNullCLSID(this->RequestedCLSID))
		return;

	ILocomotionPtr pNewMover { };
	const HRESULT hr = LocomotionClass::CreateInstance(&pNewMover, &this->RequestedCLSID,
		nullptr, CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER);

	if (FAILED(hr) || !pNewMover)
	{
		// The GUID cannot be instantiated - freeze the victim instead of
		// leaving it in a broken state.
		Debug::Log("[LocomotorWeapon] Failed to create Mover (hr=0x%X); falling back to hold.\n", hr);
		this->Config.Mode = LocoWeaponMode::Hold;
		return;
	}

	pNewMover->Link_To_Object(this->LinkedTo);
	this->Mover = pNewMover;
}

void LocomotorWeaponLocomotionClass::DetachMover()
{
	if (!this->Mover)
		return;

	this->Mover->Stop_Moving();
	this->Mover = nullptr;
}

void LocomotorWeaponLocomotionClass::InitializeFromConfig()
{
	this->RequestedCLSID = LocomotorWeapon::CurrentRequestedCLSID;
	this->Config = LocomotorWeapon::CurrentConfig;

	this->ResolveMode();

	// Jumpjet is handled entirely by the vanilla locomotor; the trigger layer
	// never routes it here. If it somehow happens, hold instead of misbehaving.
	if (this->Config.Mode == LocoWeaponMode::Jumpjet)
		this->Config.Mode = LocoWeaponMode::Hold;

	if (this->Config.Mode == LocoWeaponMode::Mover)
		this->CreateMover();

	if (this->Config.Duration >= 0)
		this->Timer.Start(this->Config.Duration);

	// Remember the altitude the victim started at; the own-movement modes use it
	// as "the ground" so that bridges and terrain need no extra handling.
	if (auto const pTarget = this->LinkedTo)
	{
		const CoordStruct coords = pTarget->GetCoords();
		this->GroundZ = coords.Z;

		const int height = this->Config.Height > 0 ? this->Config.Height : DefaultFlightHeight;
		this->FlightZ = this->GroundZ + height;
	}

	this->AppliedFrame = Unsorted::CurrentFrame;
	this->LastStuckLogFrame = -100000;
	this->DescentLogged = false;
	this->Armed = false;
	this->EndReason = EndReasonNone;

	// Diagnostics: start watching the victim (see LocomotorWeapon::TrackVictim).
	// Compiled out unless LocomotorWeapon::Diagnostics is on.
	if constexpr (LocomotorWeapon::Diagnostics)
		LocomotorWeapon::TrackVictim(this->LinkedTo);

	Debug::Log("[LocomotorWeapon] Begin: target=%s clsid=%08X mode=%d, mover=%d, height=%d, climb=%d, descend=%d, stopDistance=%d, duration=%d, endOnArrival=%d, ic=%d fs=%d warpedOut=%d\n",
		LocomotorWeapon::TargetLabel(this->LinkedTo),
		this->RequestedCLSID.Data1,
		static_cast<int>(this->Config.Mode), this->Mover != nullptr, this->Config.Height,
		this->Config.ClimbRate, this->Config.DescendRate, this->Config.StopDistance,
		this->Config.Duration, this->Config.EndOnArrival,
		this->LinkedTo ? this->LinkedTo->IsIronCurtained() : 0,
		this->LinkedTo ? this->LinkedTo->ForceShielded : 0,
		this->LinkedTo ? this->LinkedTo->BeingWarpedOut : 0);

	SyncLogger::AddLocomotorWeaponSyncLogEvent(this->LinkedTo,
		static_cast<int>(this->Config.Mode), true, GetSyncLogCaller());
}

void LocomotorWeaponLocomotionClass::UpdateMoverDestination()
{
	auto const pTarget = this->LinkedTo;

	if (!pTarget)
		return;

	auto const pFirer = pTarget->LocomotorSource;

	if (!pFirer)
		return;

	const CoordStruct firerCoords = pFirer->GetCenterCoords();

	if (this->Config.StopDistance > 0 && pTarget->DistanceFrom(pFirer) <= this->Config.StopDistance)
	{
		if (this->HasIssuedMoverDestination)
		{
			this->Mover->Stop_Moving();
			this->HasIssuedMoverDestination = false;
		}

		return;
	}

	// Only re-issue the order once the firer has moved a meaningful distance,
	// otherwise the Mover would recompute its path every single frame.
	if (this->HasIssuedMoverDestination
		&& std::abs(firerCoords.X - this->LastIssuedMoverDestination.X) < Unsorted::LeptonsPerCell
		&& std::abs(firerCoords.Y - this->LastIssuedMoverDestination.Y) < Unsorted::LeptonsPerCell)
	{
		return;
	}

	this->Mover->Move_To(firerCoords);
	this->LastIssuedMoverDestination = firerCoords;
	this->HasIssuedMoverDestination = true;
}

// ---------------------------------------------------------------------------
// ILocomotion - linking
// ---------------------------------------------------------------------------
// LocomotionClass::Link_To_Object only records which object this locomotor
// belongs to. This class owns two more locomotor instances - the piggybacked
// original and the Mover - and both have to point at that same object. They
// usually do not exist yet when the engine links a freshly created locomotor
// (Link_To_Object runs before Begin_Piggyback), but a locomotor that came back
// from a savegame can be linked again after its Mover and Piggybacker have
// been restored, and that case is what this override exists for (see the
// LocomotorWeapon handover, risk 1).
HRESULT LocomotorWeaponLocomotionClass::Link_To_Object(void* pointer)
{
	const HRESULT hr = LocomotionClass::Link_To_Object(pointer);

	if (SUCCEEDED(hr))
	{
		if (this->Piggybacker)
			this->Piggybacker->Link_To_Object(pointer);

		if (this->Mover)
			this->Mover->Link_To_Object(pointer);
	}

	return hr;
}

// ---------------------------------------------------------------------------
// ILocomotion - queries
// ---------------------------------------------------------------------------
bool LocomotorWeaponLocomotionClass::Is_Moving()
{
	if (this->IsOwnMovementMode())
		return this->Phase == LocoWeaponPhase::Lift
			|| this->Phase == LocoWeaponPhase::Cruise
			|| this->Phase == LocoWeaponPhase::Descend;

	if (auto const pSource = this->GetQuerySource())
		return pSource->Is_Moving();

	return false;
}

CoordStruct LocomotorWeaponLocomotionClass::Destination()
{
	if (this->IsOwnMovementMode())
		return this->GetGoalCoords();

	if (this->HasDestination)
		return this->DestinationCached;

	if (auto const pSource = this->GetQuerySource())
		return pSource->Destination();

	return CoordStruct::Empty;
}

CoordStruct LocomotorWeaponLocomotionClass::Head_To_Coord()
{
	if (this->IsOwnMovementMode())
		return this->GetGoalCoords();

	if (auto const pSource = this->GetQuerySource())
		return pSource->Head_To_Coord();

	if (this->LinkedTo)
		return this->LinkedTo->GetCenterCoords();

	return CoordStruct::Empty;
}

Move LocomotorWeaponLocomotionClass::Can_Enter_Cell(CellStruct cell)
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Can_Enter_Cell(cell);

	return LocomotionClass::Can_Enter_Cell(cell);
}

bool LocomotorWeaponLocomotionClass::Is_To_Have_Shadow()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Is_To_Have_Shadow();

	return LocomotionClass::Is_To_Have_Shadow();
}

Matrix3D LocomotorWeaponLocomotionClass::Draw_Matrix(VoxelIndexKey* pIndex)
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Draw_Matrix(pIndex);

	return LocomotionClass::Draw_Matrix(pIndex);
}

Matrix3D LocomotorWeaponLocomotionClass::Shadow_Matrix(VoxelIndexKey* pIndex)
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Shadow_Matrix(pIndex);

	return LocomotionClass::Shadow_Matrix(pIndex);
}

Point2D LocomotorWeaponLocomotionClass::Draw_Point()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Draw_Point();

	return LocomotionClass::Draw_Point();
}

Point2D LocomotorWeaponLocomotionClass::Shadow_Point()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Shadow_Point();

	return LocomotionClass::Shadow_Point();
}

VisualType LocomotorWeaponLocomotionClass::Visual_Character(bool raw)
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Visual_Character(raw);

	return LocomotionClass::Visual_Character(raw);
}

int LocomotorWeaponLocomotionClass::Z_Adjust()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Z_Adjust();

	return LocomotionClass::Z_Adjust();
}

ZGradient LocomotorWeaponLocomotionClass::Z_Gradient()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Z_Gradient();

	return LocomotionClass::Z_Gradient();
}

Layer LocomotorWeaponLocomotionClass::In_Which_Layer()
{
	// Modes that fly the victim themselves must report themselves as airborne,
	// otherwise the engine keeps treating them as grounded while they are up.
	if (this->IsOwnMovementMode())
		return this->IsOnGround() ? Layer::Ground : Layer::Air;

	if (auto const pSource = this->GetQuerySource())
		return pSource->In_Which_Layer();

	return Layer::Ground;
}

bool LocomotorWeaponLocomotionClass::Is_Moving_Now()
{
	if (this->IsOwnMovementMode())
		return this->Is_Moving();

	if (auto const pSource = this->GetQuerySource())
		return pSource->Is_Moving_Now();

	return false;
}

int LocomotorWeaponLocomotionClass::Apparent_Speed()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Apparent_Speed();

	return 0;
}

int LocomotorWeaponLocomotionClass::Drawing_Code()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Drawing_Code();

	return LocomotionClass::Drawing_Code();
}

FireError LocomotorWeaponLocomotionClass::Can_Fire()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Can_Fire();

	return LocomotionClass::Can_Fire();
}

int LocomotorWeaponLocomotionClass::Get_Status()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Get_Status();

	return LocomotionClass::Get_Status();
}

bool LocomotorWeaponLocomotionClass::Is_Surfacing()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Is_Surfacing();

	return LocomotionClass::Is_Surfacing();
}

bool LocomotorWeaponLocomotionClass::Is_Moving_Here(CoordStruct to)
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Is_Moving_Here(to);

	return false;
}

bool LocomotorWeaponLocomotionClass::Will_Jump_Tracks()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Will_Jump_Tracks();

	return LocomotionClass::Will_Jump_Tracks();
}

bool LocomotorWeaponLocomotionClass::Is_Really_Moving_Now()
{
	if (this->IsOwnMovementMode())
		return this->Is_Moving();

	if (auto const pSource = this->GetQuerySource())
		return pSource->Is_Really_Moving_Now();

	return false;
}

bool LocomotorWeaponLocomotionClass::Is_Powered()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Is_Powered();

	return LocomotionClass::Is_Powered();
}

bool LocomotorWeaponLocomotionClass::Is_Ion_Sensitive()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Is_Ion_Sensitive();

	return LocomotionClass::Is_Ion_Sensitive();
}

int LocomotorWeaponLocomotionClass::Get_Track_Number()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Get_Track_Number();

	return LocomotionClass::Get_Track_Number();
}

int LocomotorWeaponLocomotionClass::Get_Track_Index()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Get_Track_Index();

	return LocomotionClass::Get_Track_Index();
}

int LocomotorWeaponLocomotionClass::Get_Speed_Accum()
{
	if (auto const pSource = this->GetQuerySource())
		return pSource->Get_Speed_Accum();

	return LocomotionClass::Get_Speed_Accum();
}

// ---------------------------------------------------------------------------
// ILocomotion - commands
// ---------------------------------------------------------------------------
bool LocomotorWeaponLocomotionClass::Process()
{
	auto const pTarget = this->LinkedTo;

	if (!pTarget)
		return false;

	// The firer let go (or we timed out): stop the Mover so Is_Ok_To_End() can
	// actually fire on the next check of the vanilla auto-end machinery. Modes
	// that fly the victim first have to bring it back down.
	if (this->Is_Ok_To_End())
	{
		this->Stop_Moving();
		return false;
	}

	// Diagnostics: this locomotor is still installed and still refuses to end.
	// If a victim ever gets stuck again, these lines say why - and their absence
	// (a Begin with neither End nor Stuck) means the engine replaced
	// target->Locomotor behind our back instead of asking us to end.
	if (this->AppliedFrame >= 0
		&& Unsorted::CurrentFrame - this->AppliedFrame > StuckReportDelay
		&& Unsorted::CurrentFrame - this->LastStuckLogFrame >= StuckReportInterval)
	{
		this->LastStuckLogFrame = Unsorted::CurrentFrame;

		Debug::Log("[LocomotorWeapon] Stuck: target=%s frames=%d mode=%d mover=%d attacked=%d letgo=%d source=%s alive=%d limbo=%d attackedBy=%s z=%d ground=%d phase=%d arrived=%d height=%d crashing=%d manipulated=%d falling=%d\n",
			LocomotorWeapon::TargetLabel(pTarget),
			Unsorted::CurrentFrame - this->AppliedFrame,
			static_cast<int>(this->Config.Mode), this->Mover != nullptr,
			pTarget->IsAttackedByLocomotor, pTarget->IsLetGoByLocomotor,
			LocomotorWeapon::PointerLabel(pTarget->LocomotorSource),
			pTarget->IsAlive, pTarget->InLimbo,
			LocomotorWeapon::PointerLabel(pTarget->BeingManipulatedBy),
			pTarget->GetCoords().Z, this->GroundZ,
			static_cast<int>(this->Phase), this->Arrived,
			pTarget->GetHeight(), pTarget->IsCrashing, pTarget->IsBeingManipulated, pTarget->IsFallingDown);
	}

	// The effect is over, but the victim is still off the ground: vanilla leaves
	// it crashing in mid-air (see MustLand), so bring it down ourselves before
	// Is_Ok_To_End() may complete. Air/Meteor have their own descent code.
	if (this->WantsToEnd() && this->MustLand()
		&& this->Config.Mode != LocoWeaponMode::Air
		&& this->Config.Mode != LocoWeaponMode::Meteor)
	{
		return this->ProcessLanding();
	}

	if (auto const pMover = this->GetMover())
	{
		if (this->Config.Mode == LocoWeaponMode::Mover)
			this->UpdateMoverDestination();

		return pMover->Process();
	}

	// hold / warp / air / meteor: this class does the moving.
	return this->ProcessOwnMovement();
}

void LocomotorWeaponLocomotionClass::Move_To(CoordStruct to)
{
	this->DestinationCached = to;
	this->HasDestination = true;

	if (auto const pMover = this->GetMover())
	{
		pMover->Move_To(to);
		this->LastIssuedMoverDestination = to;
		this->HasIssuedMoverDestination = true;
	}
}

void LocomotorWeaponLocomotionClass::Stop_Moving()
{
	this->DestinationCached = CoordStruct::Empty;
	this->HasDestination = false;
	this->HasIssuedMoverDestination = false;

	if (auto const pMover = this->GetMover())
		pMover->Stop_Moving();
}

void LocomotorWeaponLocomotionClass::Do_Turn(DirStruct coord)
{
	if (auto const pMover = this->GetMover())
		pMover->Do_Turn(coord);
	else if (this->LinkedTo)
		this->LinkedTo->PrimaryFacing.SetCurrent(coord);
}

void LocomotorWeaponLocomotionClass::Unlimbo()
{
	if (auto const pMover = this->GetMover())
		pMover->Unlimbo();
}

void LocomotorWeaponLocomotionClass::Tilt_Pitch_AI()
{
	if (auto const pMover = this->GetMover())
		pMover->Tilt_Pitch_AI();
}

bool LocomotorWeaponLocomotionClass::Power_On()
{
	if (auto const pMover = this->GetMover())
		return pMover->Power_On();

	return LocomotionClass::Power_On();
}

bool LocomotorWeaponLocomotionClass::Power_Off()
{
	if (auto const pMover = this->GetMover())
		return pMover->Power_Off();

	return LocomotionClass::Power_Off();
}

bool LocomotorWeaponLocomotionClass::Push(DirStruct dir)
{
	if (auto const pMover = this->GetMover())
		return pMover->Push(dir);

	return false;
}

bool LocomotorWeaponLocomotionClass::Shove(DirStruct dir)
{
	if (auto const pMover = this->GetMover())
		return pMover->Shove(dir);

	return false;
}

void LocomotorWeaponLocomotionClass::Force_Track(int track, CoordStruct coord)
{
	if (auto const pMover = this->GetMover())
		pMover->Force_Track(track, coord);
}

void LocomotorWeaponLocomotionClass::Force_Immediate_Destination(CoordStruct coord)
{
	this->DestinationCached = coord;
	this->HasDestination = true;

	if (auto const pMover = this->GetMover())
		pMover->Force_Immediate_Destination(coord);
}

void LocomotorWeaponLocomotionClass::Force_New_Slope(int ramp)
{
	if (auto const pMover = this->GetMover())
		pMover->Force_New_Slope(ramp);
}

void LocomotorWeaponLocomotionClass::Acquire_Hunter_Seeker_Target()
{
	if (auto const pMover = this->GetMover())
		pMover->Acquire_Hunter_Seeker_Target();
}

void LocomotorWeaponLocomotionClass::Mark_All_Occupation_Bits(MarkType mark)
{
	if (auto const pSource = this->GetQuerySource())
	{
		pSource->Mark_All_Occupation_Bits(mark);
		return;
	}

	LocomotionClass::Mark_All_Occupation_Bits(mark);
}

void LocomotorWeaponLocomotionClass::Stop_Movement_Animation()
{
	if (auto const pMover = this->GetMover())
		pMover->Stop_Movement_Animation();
}

void LocomotorWeaponLocomotionClass::Limbo()
{
	this->DetachMover();
}

void LocomotorWeaponLocomotionClass::Lock()
{
	if (auto const pSource = this->GetQuerySource())
		pSource->Lock();
}

void LocomotorWeaponLocomotionClass::Unlock()
{
	if (auto const pSource = this->GetQuerySource())
		pSource->Unlock();
}

// ---------------------------------------------------------------------------
// IPiggyback
// ---------------------------------------------------------------------------
HRESULT LocomotorWeaponLocomotionClass::Begin_Piggyback(ILocomotion* pointer)
{
	if (!pointer)
		return E_POINTER;

	if (this->Piggybacker)
		return E_FAIL;

	this->Piggybacker = pointer;
	this->InitializeFromConfig();

	return S_OK;
}

HRESULT LocomotorWeaponLocomotionClass::End_Piggyback(ILocomotion** pointer)
{
	if (!pointer)
		return E_POINTER;

	if (!this->Piggybacker)
		return S_FALSE;

	this->DetachMover();

	auto const pTarget = this->LinkedTo;

	// Snapshot the release diagnostics before the cleanup below clears them.
	const FootClass* const pFirer = pTarget ? pTarget->LocomotorSource : nullptr;
	const bool wasAttacked = pTarget && pTarget->IsAttackedByLocomotor;
	const bool wasLetGo = pTarget && pTarget->IsLetGoByLocomotor;
	const int wasZ = pTarget ? pTarget->GetCoords().Z : 0;
	const int wasHeight = pTarget ? pTarget->GetHeight() : 0;
	const bool wasCrashing = pTarget && pTarget->IsCrashing;
	const bool wasManipulated = pTarget && pTarget->IsBeingManipulated;
	const bool wasFalling = pTarget && pTarget->IsFallingDown;

	if (pTarget)
	{
		// Without this the victim can never accept a destination again
		// (FootClass::SetDestination rejects everything at 0x4D94BE).
		pTarget->IsAttackedByLocomotor = false;
		pTarget->IsLetGoByLocomotor = false;

		// Vanilla's Jumpjet cleanup leaves FrozenStill set; the project decided
		// to clear it (see the LocomotorWeapon handover, decision B4).
		pTarget->FrozenStill = false;

		// Mirrors JumpjetLocomotionClass::End_Piggyback (0x54DAC1-0x54DACD):
		// forget the firer we were dropped by.
		pTarget->BeingManipulatedBy = nullptr;
		pTarget->ChronoWarpedByHouse = nullptr;

		// ReleaseLocomotor's airborne branch (0x70FF25) also sets
		// IsCrashing / IsBeingManipulated / FallingDown on a victim released
		// above the ground, and only Jumpjet's landing code ever clears them
		// (0x54CA08). Leaving them set keeps the victim frozen in the air with
		// a ground locomotor ("定在半空中"), so clear them here as well.
		ClearCrashState(pTarget);

		RestoreHarvestMission(pTarget);
	}

	// Keeps the restored locomotor powered the same way the vanilla hook
	// EndPiggyback_PowerOn (src/Misc/Hooks.BugFixes.cpp) does for Drive/Jumpjet.
	if (!pTarget || (!pTarget->Deactivated && !pTarget->IsUnderEMP()))
		this->Piggybacker->Power_On();
	else
		this->Piggybacker->Power_Off();

	// Hand the reference over without AddRef()ing: the caller stores it back
	// into FootClass::Locomotor.
	*pointer = this->Piggybacker.Detach();

	// Diagnostics: keep watching the victim after we are gone - the unresolved
	// "it cannot be damaged any more" report happens *after* this frame.
	// Compiled out unless LocomotorWeapon::Diagnostics is on.
	if constexpr (LocomotorWeapon::Diagnostics)
		LocomotorWeapon::TrackVictim(pTarget);

	Debug::Log("[LocomotorWeapon] End: target=%s reason=%s frames=%d mode=%d attacked=%d letgo=%d source=%s alive=%d limbo=%d arrived=%d z=%d height=%d ground=%d crashing=%d manipulated=%d falling=%d tracked=%s\n",
		LocomotorWeapon::TargetLabel(pTarget), EndReasonLabel(this->EndReason),
		this->AppliedFrame >= 0 ? Unsorted::CurrentFrame - this->AppliedFrame : -1,
		static_cast<int>(this->Config.Mode), wasAttacked, wasLetGo,
		LocomotorWeapon::PointerLabel(pFirer),
		pTarget ? pTarget->IsAlive : false, pTarget ? pTarget->InLimbo : false, this->Arrived,
		wasZ, wasHeight, this->GroundZ, wasCrashing, wasManipulated, wasFalling,
		LocomotorWeapon::Diagnostics
			? (LocomotorWeapon::IsTrackedVictim(pTarget) ? "1" : "0")
			: "off");

	SyncLogger::AddLocomotorWeaponSyncLogEvent(pTarget,
		static_cast<int>(this->Config.Mode), false, GetSyncLogCaller());

	// What we hand back to the engine. TechnoClass::CanFire refuses to fire when
	// a unit has both LocomotorSource (+0x2B0) and IsAttackedByLocomotor (+0x6AD)
	// set (0x6FBF57-0x6FBF7D), and FootClass::IsSelectable-ish (0x4DFA50) returns
	// false while +0x6AD is set - so a stale flag here would show up in play as
	// "it cannot be selected / it takes no damage from some units".
	if (pTarget)
	{
		Debug::Log("[LocomotorWeapon] Post: target=%s attacked=%d letgo=%d frozen=%d crashing=%d manipulated=%d falling=%d bomb=%d z=%d height=%d inAir=%d ic=%d fs=%d icLeft=%d warpedOut=%d warpingOut=%d temporalTargetingMe=%s temporalImUsing=%s bunker=%s health=%d estimated=%d\n",
			LocomotorWeapon::TargetLabel(pTarget),
			pTarget->IsAttackedByLocomotor, pTarget->IsLetGoByLocomotor, pTarget->FrozenStill,
			pTarget->IsCrashing, pTarget->IsBeingManipulated, pTarget->IsFallingDown,
			pTarget->IsABomb, pTarget->GetCoords().Z, pTarget->GetHeight(), pTarget->IsInAir(),
			pTarget->IsIronCurtained(), pTarget->ForceShielded, pTarget->IronCurtainTimer.TimeLeft,
			pTarget->BeingWarpedOut, pTarget->WarpingOut,
			LocomotorWeapon::PointerLabel(pTarget->TemporalTargetingMe),
			LocomotorWeapon::PointerLabel(pTarget->TemporalImUsing),
			LocomotorWeapon::PointerLabel(pTarget->BunkerLinkedItem),
			pTarget->Health, pTarget->EstimatedHealth);
	}

	return S_OK;
}

bool LocomotorWeaponLocomotionClass::Is_Ok_To_End()
{
	if (!this->WantsToEnd())
		return false;

	// Everything that lifted (or that the engine flagged as crashing) has to be
	// back on the ground before the original locomotor may be handed back.
	if (this->MustLand() && !this->IsOnGround())
		return false;

	return true;
}

// Every release/timeout/arrival condition, without the landing gate.
bool LocomotorWeaponLocomotionClass::WantsToEnd()
{
	if (!this->Piggybacker)
		return false;

	auto const pTarget = this->LinkedTo;

	if (!pTarget || !pTarget->IsAlive || pTarget->InLimbo)
	{
		this->EndReason = EndReasonTargetGone;
		return true;
	}

	// TechnoClass::ImbueLocomotor arms the victim in this order:
	//   0x7102D8  Begin_Piggyback (our "Begin:" log)
	//   0x7102DB  target->Locomotor = <us>
	//   0x710318  target->LocomotorSource = firer
	//   0x710326  target->SetDestination(firer)        (Phobos-hooked)
	//   0x710334  target->[vtbl+0x150]()
	//   0x71034E  target->IsAttackedByLocomotor = 1    <- the last step
	// SetDestination and that second virtual both run *before* the flag is set,
	// and for some victims they reach the engine's auto-end check
	// (0x4D8302 / 0x4D9309), which asks us Is_Ok_To_End(). Reading
	// "IsAttackedByLocomotor == 0" in that window as "the firer let go" ended the
	// brand-new effect while ImbueLocomotor was still running - and the rest of
	// ImbueLocomotor then set IsAttackedByLocomotor = 1 on a victim whose
	// locomotor had already been handed back, locking it forever with no "Stuck"
	// line to show for it (debug.log 19:07: Begin and End in the same frame,
	// frames=0, for exactly SAPC#1105624 and SHAD#1105623 - the hovercraft and
	// the jumpjet helicopter). So nothing may end until we have seen the armed
	// flag at least once.
	if (pTarget->IsAttackedByLocomotor)
		this->Armed = true;
	else if (!this->Armed
		&& Unsorted::CurrentFrame - this->AppliedFrame < ArmedGraceFrames)
		return false;

	// FootClass::SetDestination clears the firer link and sets
	// IsLetGoByLocomotor when the magnetron releases with a null destination
	// (0x4D9518-0x4D9538); JumpjetLocomotionClass::Is_Ok_To_End (0x54DB00)
	// treats exactly this combination as "done".
	//
	// TechnoClass::ReleaseLocomotor (0x70FEE0) is the firer's side of the
	// release. It clears the victim's LocomotorSource (+0x2B0) as its very
	// first action (0x70FEF1) in *every* branch, but it only sets
	// IsLetGoByLocomotor (+0x6AE) on its ground branch (0x70FFBB): victims that
	// take the airborne branch (0x70FF25) or the third entry point (0x70FFD4)
	// never got that flag, so they never looked released and stayed locked
	// forever (= the "报废" reports of the first in-game session). A cleared
	// source link is therefore the reliable "the firer let go" signal.
	//
	// Both firer-side paths clear the source link (0x70FEF1 and 0x4D9532), so
	// only those two signals may end the effect. IsLetGoByLocomotor on its own
	// does NOT mean the firer let go: Jumpjet's own landing code sets it
	// (0x54C9FE) without touching the source link, and for a Jumpjet victim
	// (SHAD = Stallion helicopter) that used to end the freshly applied effect
	// after a single frame - which is why the magnetron never dragged it
	// (debug.log 19:29: SHAD Begin/End with frames=1, letgo=1, source=<set>).
	const bool sourceCleared = pTarget->LocomotorSource == nullptr;
	const bool flagCleared = !pTarget->IsAttackedByLocomotor;
	const bool firerLetGo = sourceCleared || flagCleared;

	// ReleaseOnFirerStop=false ignores the release and keeps the lock until the
	// timer runs out. A finite Duration is required, otherwise the victim would
	// be locked forever - the exact state this whole feature exists to avoid.
	const bool released = firerLetGo
		&& (this->Config.ReleaseOnFirerStop || this->Config.Duration < 0);

	// Duration is the lock duration for every mode except Warp, where it is the
	// delay before the teleport instead (see ProcessOwnMovement).
	const bool timedOut = this->Config.Mode != LocoWeaponMode::Warp
		&& this->Config.Duration >= 0 && this->Timer.Completed();

	const bool arrived = this->Config.EndOnArrival && this->Arrived;

	if (released)
	{
		this->EndReason = sourceCleared ? EndReasonFirerReleased : EndReasonFlagCleared;
		return true;
	}

	if (timedOut)
	{
		this->EndReason = EndReasonTimeout;
		return true;
	}

	if (arrived)
	{
		this->EndReason = EndReasonArrival;
		return true;
	}

	return false;
}

bool LocomotorWeaponLocomotionClass::MustLand()
{
	auto const pTarget = this->LinkedTo;

	if (!pTarget)
		return false;

	// TechnoClass::ReleaseLocomotor's airborne branch (0x70FF25) flags a victim
	// it released above the ground with IsCrashing (+0x425) and
	// IsBeingManipulated (+0x427), then calls the object's FallingDown slot
	// (0x70FF5F) and SetDestination(null). The only code in the game that ever
	// clears those flags is Jumpjet's own landing sequence (0x54CA08/0x54CA12),
	// so for every other victim nothing brings it down and nothing clears the
	// crash state: it stays frozen in the air - the second in-game report
	// ("hover 打陆地载具：结束时离地有高度，定在半空中", "两栖/空军依然报废").
	// Therefore anything that is off the ground must be put back down by us.
	return pTarget->IsCrashing
		|| pTarget->IsBeingManipulated
		|| pTarget->GetHeight() > 0;
}

void LocomotorWeaponLocomotionClass::ClearCrashState(FootClass* pTarget)
{
	if (!pTarget)
		return;

	pTarget->IsCrashing = false;
	pTarget->IsBeingManipulated = false;
	pTarget->IsFallingDown = false;
	pTarget->IsABomb = false;
}

bool LocomotorWeaponLocomotionClass::ProcessLanding()
{
	auto const pTarget = this->LinkedTo;

	if (!pTarget)
		return false;

	// The Mover must not fight the descent.
	if (this->Mover)
		this->Mover->Stop_Moving();

	const CoordStruct current = pTarget->GetCoords();
	const int height = pTarget->GetHeight();
	const int descendRate = this->Config.DescendRate > 0 ? this->Config.DescendRate : DefaultDescendRate;

	// The ground directly underneath, as the engine sees it. Not the altitude
	// captured at Begin: the victim may have been dragged over higher or lower
	// terrain in the meantime, and landing on the wrong one is what left units
	// hovering.
	const int targetZ = current.Z - std::max(height, 0);

	if (!this->DescentLogged)
	{
		this->DescentLogged = true;

		Debug::Log("[LocomotorWeapon] Descent: target=%s z=%d height=%d targetZ=%d ground=%d crashing=%d manipulated=%d falling=%d\n",
			LocomotorWeapon::TargetLabel(pTarget), current.Z, height, targetZ, this->GroundZ,
			pTarget->IsCrashing, pTarget->IsBeingManipulated, pTarget->IsFallingDown);
	}

	CoordStruct next = current;
	next.Z = std::max(targetZ, current.Z - descendRate);

	if (next.Z <= targetZ)
	{
		next.Z = targetZ;
		this->ApplyLocation(next);

		// Touched down: undo the crash state the release left behind, otherwise
		// the victim keeps a "crashing" flag forever (and IsABomb would make it
		// explode on contact).
		ClearCrashState(pTarget);
		this->Phase = LocoWeaponPhase::Landed;

		Debug::Log("[LocomotorWeapon] Landed: target=%s z=%d ground=%d\n",
			LocomotorWeapon::TargetLabel(pTarget), next.Z, this->GroundZ);

		return false;
	}

	this->ApplyLocation(next);

	return true;
}

HRESULT LocomotorWeaponLocomotionClass::Piggyback_CLSID(GUID* classid)
{
	if (!classid)
		return E_POINTER;

	if (this->Piggybacker)
	{
		IPersistStreamPtr piggyAsPersist(this->Piggybacker);

		return piggyAsPersist->GetClassID(classid);
	}

	IPersistStreamPtr thisAsPersist(this);

	if (!thisAsPersist)
		return E_FAIL;

	return thisAsPersist->GetClassID(classid);
}

bool LocomotorWeaponLocomotionClass::Is_Piggybacking()
{
	return this->Piggybacker != nullptr;
}

// ---------------------------------------------------------------------------
// Modes this class moves itself: hold / warp / air / meteor
// ---------------------------------------------------------------------------
bool LocomotorWeaponLocomotionClass::IsOwnMovementMode() const
{
	return !this->Mover
		&& (this->Config.Mode == LocoWeaponMode::Hold
			|| this->Config.Mode == LocoWeaponMode::Warp
			|| this->Config.Mode == LocoWeaponMode::Air
			|| this->Config.Mode == LocoWeaponMode::Meteor);
}

bool LocomotorWeaponLocomotionClass::RequiresLanding() const
{
	return this->Config.Mode == LocoWeaponMode::Air
		|| this->Config.Mode == LocoWeaponMode::Meteor;
}

bool LocomotorWeaponLocomotionClass::IsOnGround()
{
	if (this->Phase == LocoWeaponPhase::Landed)
		return true;

	auto const pTarget = this->LinkedTo;

	if (!pTarget)
		return true;

	// The engine's own idea of "off the ground" - it is the very check
	// ReleaseLocomotor's branch uses (0x70FF13 reads the same virtual).
	// Deliberately no tolerance based on the altitude captured at Begin: a
	// victim dragged over lower terrain would then count as "landed" while
	// still high above the ground there - which is exactly how EUROC / FMCV /
	// CIVP still ended 43..67 leptons up (debug.log 19:29).
	return pTarget->GetHeight() <= 0;
}

CoordStruct LocomotorWeaponLocomotionClass::GetGoalCoords()
{
	auto const pTarget = this->LinkedTo;

	if (!pTarget)
		return CoordStruct::Empty;

	if (auto const pFirer = pTarget->LocomotorSource)
		return pFirer->GetCenterCoords();

	// After the firer is gone (release without a destination) keep the last
	// order so that the victim still lands where it was being dragged to.
	CoordStruct goal = pTarget->GetCoords();

	if (this->HasDestination)
	{
		goal.X = this->DestinationCached.X;
		goal.Y = this->DestinationCached.Y;
	}

	return goal;
}

void LocomotorWeaponLocomotionClass::ApplyLocation(const CoordStruct& coord)
{
	auto const pTarget = this->LinkedTo;

	if (!pTarget)
		return;

	// Same dance as the reference TestLocomotionClass: occupation bits have to
	// be released around a teleport or ghost blockers are left behind.
	pTarget->Mark(MarkType::Up);
	pTarget->SetLocation(coord);
	pTarget->Mark(MarkType::Down);
}

WarheadTypeClass* LocomotorWeaponLocomotionClass::GetWarhead() const
{
	const int index = this->Config.WarheadIndex;

	if (index >= 0 && index < WarheadTypeClass::Array.Count)
		return WarheadTypeClass::Array[index];

	return nullptr;
}

AnimTypeClass* LocomotorWeaponLocomotionClass::GetAnim() const
{
	const int index = this->Config.AnimIndex;

	if (index >= 0 && index < AnimTypeClass::Array.Count)
		return AnimTypeClass::Array[index];

	return nullptr;
}

AnimTypeClass* LocomotorWeaponLocomotionClass::GetMeteorAnim() const
{
	const int index = this->Config.MeteorAnimIndex;

	if (index >= 0 && index < AnimTypeClass::Array.Count)
		return AnimTypeClass::Array[index];

	return nullptr;
}

// Vanilla JumpjetLocomotionClass::End_Piggyback sends a released harvester back
// to Mission::Harvest (0x54DA95-0x54DAB9):
//   0x54DA95  mov  edx,[LinkedTo+0x21C]     ; TechnoTypeClass*
//   0x54DA9B  cmp  BYTE PTR [edx+0x1EC],0
//   0x54DAA1  jne  skip
//   0x54DAA5  call [vtbl+0x84]              ; GetTechnoType(), the same pointer
//   0x54DAAB  cmp  BYTE PTR [eax+0x5EC],0
//   0x54DAB1  je   skip
//   0x54DAB3  QueueMission(Mission::Harvest, false)
// Those two TechnoTypeClass flags have no YRpp names, so read them raw; this is
// a byte-for-byte copy of what the vanilla code tests - no more, no less.
void LocomotorWeaponLocomotionClass::RestoreHarvestMission(FootClass* pTarget)
{
	if (!pTarget)
		return;

	auto const pType = pTarget->GetTechnoType();

	if (!pType)
		return;

	auto const pBytes = reinterpret_cast<const unsigned char*>(pType);

	if (pBytes[0x1EC] == 0 && pBytes[0x5EC] != 0)
		pTarget->QueueMission(Mission::Harvest, false);
}

void LocomotorWeaponLocomotionClass::ApplyImpact()
{
	if (this->ImpactApplied)
		return;

	this->ImpactApplied = true;

	auto const pTarget = this->LinkedTo;

	if (!pTarget)
		return;

	if (auto const pAnimType = this->GetMeteorAnim())
		GameCreate<AnimClass>(pAnimType, pTarget->GetCoords());

	if (!this->Config.FallingDamage)
		return;

	int damage = this->Config.MeteorDamage;

	if (damage < 0)
	{
		// Mirror of the vanilla falling damage computation
		// (0x41BC78 / 0x74623E): F2I(height * [General]FallingDamageMultiplier),
		// clamped at zero.
		damage = Game::F2I(this->FallDistance * RulesClass::Instance->FallingDamageMultiplier);

		if (damage < 0)
			damage = 0;
	}

	if (damage <= 0)
		return;

	auto const pWarhead = this->GetWarhead();

	if (!pWarhead)
		return;

	auto const pAttacker = pTarget->LocomotorSource;

	pTarget->ReceiveDamage(&damage, 0, pWarhead, pAttacker, false, false,
		pAttacker ? pAttacker->Owner : nullptr);
}

void LocomotorWeaponLocomotionClass::WarpToFirer()
{
	auto const pTarget = this->LinkedTo;

	if (!pTarget)
		return;

	const CoordStruct current = pTarget->GetCoords();
	CoordStruct dest = this->GetGoalCoords();
	dest.Z = current.Z;

	// StopDistance keeps the victim that far short of the firer.
	const int stop = this->Config.StopDistance;

	if (stop > 0)
	{
		const double dx = static_cast<double>(dest.X - current.X);
		const double dy = static_cast<double>(dest.Y - current.Y);
		const double len = std::sqrt(dx * dx + dy * dy);

		if (len > stop)
		{
			dest.X -= static_cast<int>(dx / len * stop);
			dest.Y -= static_cast<int>(dy / len * stop);
		}
	}

	this->ApplyLocation(dest);
	this->Arrived = true;

	if (auto const pAnimType = this->GetAnim())
		GameCreate<AnimClass>(pAnimType, dest);
}

bool LocomotorWeaponLocomotionClass::ProcessOwnMovement()
{
	switch (this->Config.Mode)
	{
	case LocoWeaponMode::Warp:
	{
		if (this->WarpDone)
			return false;

		const int delay = this->Config.Duration >= 0 ? this->Config.Duration : DefaultWarpDelay;

		if (delay > 0 && (!this->Timer.IsTicking() || !this->Timer.Completed()))
			return false;

		this->WarpToFirer();
		this->WarpDone = true;

		return false;
	}

	case LocoWeaponMode::Air:
	case LocoWeaponMode::Meteor:
		return this->ProcessFlight();

	case LocoWeaponMode::Hold:
	default:
		// Deliberately no movement: the victim is pinned in place until the
		// effect ends.
		return false;
	}
}

bool LocomotorWeaponLocomotionClass::ProcessFlight()
{
	auto const pTarget = this->LinkedTo;

	if (!pTarget)
		return false;

	const bool meteor = this->Config.Mode == LocoWeaponMode::Meteor;
	const CoordStruct current = pTarget->GetCoords();
	const CoordStruct goal = this->GetGoalCoords();

	const int speed = this->Config.Speed > 0
		? this->Config.Speed
		: std::max(pTarget->GetTechnoType()->Speed, 1);

	const int climbRate = this->Config.ClimbRate > 0 ? this->Config.ClimbRate : DefaultClimbRate;
	const int descendRate = this->Config.DescendRate > 0 ? this->Config.DescendRate : DefaultDescendRate;
	const int stop = std::max(this->Config.StopDistance, 0);

	CoordStruct next = current;

	// First frame: place a meteor in the sky above the firer, everything else
	// starts by climbing.
	if (this->Phase == LocoWeaponPhase::Idle)
	{
		if (meteor)
		{
			const int height = this->Config.Height > 0 ? this->Config.Height : DefaultFlightHeight;

			CoordStruct spawn = goal;
			spawn.Z += height;

			this->GroundZ = goal.Z;
			this->FallDistance = height;
			this->ApplyLocation(spawn);
			this->Phase = LocoWeaponPhase::Descend;

			return true;
		}

		this->Phase = LocoWeaponPhase::Lift;
	}

	// The effect is over: bring the victim back down (only air can still be up).
	const bool firerLetGo = !pTarget->IsAttackedByLocomotor || pTarget->IsLetGoByLocomotor;
	const bool timedOut = this->Config.Duration >= 0 && this->Timer.Completed();

	if (!meteor && (firerLetGo || timedOut || (this->Config.EndOnArrival && this->Arrived)))
		this->Phase = LocoWeaponPhase::Descend;

	switch (this->Phase)
	{
	case LocoWeaponPhase::Lift:
	{
		next.Z = std::min(this->FlightZ, current.Z + climbRate);
		StepTowards(next.X, goal.X, speed);
		StepTowards(next.Y, goal.Y, speed);

		if (next.Z >= this->FlightZ)
			this->Phase = LocoWeaponPhase::Cruise;

		break;
	}

	case LocoWeaponPhase::Cruise:
	{
		next.Z = this->FlightZ;
		StepTowards(next.X, goal.X, speed);
		StepTowards(next.Y, goal.Y, speed);

		const int threshold = stop > 0 ? stop : Unsorted::LeptonsPerCell;
		const int dx = next.X - goal.X;
		const int dy = next.Y - goal.Y;

		if (dx * dx + dy * dy <= threshold * threshold)
		{
			this->Arrived = true;
			this->Phase = LocoWeaponPhase::Descend;
		}

		break;
	}

	case LocoWeaponPhase::Descend:
	{
		next.Z = std::max(this->GroundZ, current.Z - descendRate);

		// A meteor drops straight down; air still tracks the firer while it
		// comes down so that it does not end up far away from it.
		if (!meteor)
		{
			StepTowards(next.X, goal.X, speed);
			StepTowards(next.Y, goal.Y, speed);
		}

		if (next.Z <= this->GroundZ)
		{
			next.Z = this->GroundZ;
			this->Phase = LocoWeaponPhase::Landed;
			this->ApplyLocation(next);
			this->ApplyImpact();

			return false;
		}

		break;
	}

	case LocoWeaponPhase::Landed:
	case LocoWeaponPhase::Idle:
	default:
		return false;
	}

	this->ApplyLocation(next);

	return true;
}

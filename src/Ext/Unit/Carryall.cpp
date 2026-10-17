#include "Carryall.h"

#include <Ext/Unit/Body.h>
#include <Ext/UnitType/Body.h>

#include <CellClass.h>
#include <HouseClass.h>
#include <JumpjetLocomotionClass.h>
#include <LocomotionClass.h>
#include <MapClass.h>
#include <RulesClass.h>
#include <UnitClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>

namespace
{
	// How far the search for a free drop cell reaches, in cells.
	constexpr int MaxDropSearchRadius = 8;

	// Used when the locomotor is unexpectedly missing, in leptons per frame. The
	// jumpjet's own Climb is what a real carrier uses; this only keeps a broken state
	// from freezing the descent.
	constexpr int FallbackClimbStep = 20;

	// How close, horizontally, the carrier has to get before it dives. A jumpjet hovers
	// with a wobble and never sits on an exact cell, so this is a distance rather than a
	// cell comparison; 384 leptons is 1.5 cells, which is what the version that worked in
	// game used.
	constexpr int ApproachRange = 384;

	// Squared distance to the vehicle's own cell centre (one cell is 256 leptons across,
	// so its diagonal is about 362). When the carrier is in a neighbouring cell it still
	// has to be at least this close, otherwise it has stopped alongside the target
	// rather than over it - which is how it ended up picking vehicles up from an offset.
	constexpr int CellDiagonalSq = 256 * 256;

	// A quarter of a cell. Below this the unit counts as being on the ground for the
	// "close enough to grab" test.
	constexpr int NearGroundHeight = 0x68;

	// Altitude at which the payload is let go. Low enough that the aircraft visibly came
	// down onto the cell, high enough that the jumpjet does not make the ground layer of
	// that cell "occupied" for the engine's Unlimbo.
	constexpr int DropReleaseHeight = 320;

	// ObjectClass::SetPositionAnimated, 0x4DB810 (vtable +0x1B4). This is the engine's own
	// animated position setter: the vanilla aircraft carryall moves its cargo with exactly
	// this call every frame (it passes the carrier's own Location, gamemd 0x4151AA), and
	// Ares only adds the unload-time UpdatePosition(2) / Power_Off on top of that
	// (Ares hooks 416C94 / 416C4D). Writing Location directly instead is not the same
	// thing - it skips the engine's own position bookkeeping.
	//
	// Declared here by address on purpose: adding it to YRpp earlier added vtable slots to
	// ObjectClass/FootClass, i.e. to every class in the project, which crashed on load.
	void SetPositionAnimatedRaw(void* pObject, const CoordStruct& coords)
	{
		using SetPositionAnimatedFn = int(__thiscall*)(void*, CoordStruct*);
		static SetPositionAnimatedFn const fn = reinterpret_cast<SetPositionAnimatedFn>(0x4DB810);

		auto crd = coords;
		fn(pObject, &crd);
	}

	// The jumpjet's own climb rate, i.e. the number of leptons the engine would move
	// this unit per frame while it ascends or descends. Reading it keeps our forced
	// descent at the engine's own pace.
	int GetClimbStep(UnitClass* pThis)
	{
		if (auto const pLoco = locomotion_cast<JumpjetLocomotionClass*>(pThis->Locomotor))
			return std::max(1, static_cast<int>(pLoco->Climb));

		return FallbackClimbStep;
	}

	// The descent lever, exactly the combination the version that worked in game used:
	//
	//   1. the jumpjet's own Height field (+0x2C) is what the engine drives the unit's
	//      altitude towards - its process reloads CurrentHeight from it every frame.
	//      Setting it is what actually moves the carrier; writing Z alone loses a tug of
	//      war against that reload (measured in game: Z was stepped -44 and put straight
	//      back, then froze at ground + 600).
	//   2. SetPositionAnimated then lands the frame's position on the new altitude, which
	//      is the "UpdateReveal(0) -> SetPositionAnimated -> UpdateReveal(1)" triple the
	//      reference carryall runs (0x1005861C / 0x1005862A / 0x10058636).
	//
	// It only works with NO destination set: a jumpjet holding a destination it cannot
	// reach raises itself by one Climb a frame instead (measured: Z 1621 -> 5977, +22 a
	// frame). Every caller therefore stops the locomotion first.
	//
	// Neither State nor IsMoving nor CurrentHeight is written: those belong to the
	// engine's movement AI.
	void WritePosition(UnitClass* pThis, const CoordStruct& coords);
	int GetFlyHeight(UnitClass* pThis);

	// Sets the carrier's altitude (above the ground) and moves it there.
	void SetAltitude(UnitClass* pThis, int height)
	{
		height = std::max(0, height);

		if (auto const pLoco = locomotion_cast<JumpjetLocomotionClass*>(pThis->Locomotor))
			pLoco->Height = height;

		auto coords = pThis->GetCoords();
		coords.Z = pThis->GetCell()->GetCoords().Z + height;
		WritePosition(pThis, coords);
	}

	// Writes the carrier's position, keeping the engine's reveal bookkeeping in sync.
	void WritePosition(UnitClass* pThis, const CoordStruct& coords)
	{
		using UpdateRevealFn = char(__thiscall*)(void*, int);
		static UpdateRevealFn const updateReveal = reinterpret_cast<UpdateRevealFn>(0x4D3780);

		updateReveal(pThis, 0);
		SetPositionAnimatedRaw(pThis, coords);
		updateReveal(pThis, 1);
	}

	// Moves the carrier one step towards the target altitude. The step is limited by the
	// jumpjet's own Climb value - the same ceiling the engine uses for a jumpjet's vertical
	// movement - and it accelerates/decelerates instead of jumping straight to full speed,
	// so taking off and landing look like the aircraft's own climb instead of a teleport.
	void MoveAltitudeTo(UnitClass* pThis, int targetHeight)
	{
		auto const pExt = UnitExt::TryFetch(pThis);

		if (!pExt)
			return;

		const int maxStep = GetClimbStep(pThis);
		const int accel = std::max(1, maxStep / 4);
		const int height = GetFlyHeight(pThis);
		const int remaining = targetHeight - height;
		int& speed = pExt->CarryallVerticalSpeed;

		if (remaining == 0)
		{
			speed = 0;
			return;
		}

		if (remaining > 0)
			speed = std::min(maxStep, std::max(0, speed) + accel);
		else
			speed = std::max(-maxStep, std::min(0, speed) - accel);

		int step = speed;

		if (std::abs(step) > std::abs(remaining))
			step = remaining;

		SetAltitude(pThis, height + step);
	}

	// Snaps the carrier horizontally onto the cell centre, keeping its current altitude.
	// A jumpjet whose altitude is being driven drifts sideways (its locomotion keeps a
	// destination of its own); pinning it over the cell is what keeps the grab/release in
	// the same cell the aircraft is in.
	void SnapOverCell(UnitClass* pThis, CellClass* pCell)
	{
		auto const center = pCell->GetCoordsWithBridge();
		auto coords = pThis->GetCoords();
		coords.X = center.X;
		coords.Y = center.Y;

		WritePosition(pThis, coords);
	}

	// Parks the jumpjet on the ground: Height 0 is "on the ground" for a jumpjet, so the
	// engine's own hover logic keeps it there instead of springing back to JumpjetHeight.
	// The carrier is also pinned onto the cell centre, so the grab happens in the cell the
	// aircraft is in (not in the neighbouring one it had drifted into).
	void ParkOnGround(UnitClass* pThis, CellClass* pCell)
	{
		if (auto const pLoco = locomotion_cast<JumpjetLocomotionClass*>(pThis->Locomotor))
			pLoco->Height = 0;

		if (auto const pExt = UnitExt::TryFetch(pThis))
			pExt->CarryallVerticalSpeed = 0;

		auto const center = pCell->GetCoordsWithBridge();
		WritePosition(pThis, center);
	}

	// The altitude the unit is flying at, i.e. what the jumpjet is being driven towards.
	int GetFlyHeight(UnitClass* pThis)
	{
		if (auto const pLoco = locomotion_cast<JumpjetLocomotionClass*>(pThis->Locomotor))
			return pLoco->Height;

		return 0;
	}

	// Stops the carrier from being flown anywhere by its own locomotion, which is the
	// state a descent has to be in. A jumpjet that still has a destination it cannot
	// reach raises itself by one Climb a frame.
	void HaltLocomotion(UnitClass* pThis)
	{
		pThis->SetDestination(nullptr, true);

		if (auto const pLoco = locomotion_cast<JumpjetLocomotionClass*>(pThis->Locomotor))
		{
			if (pLoco->IsMoving)
				pLoco->Stop_Moving();
		}
	}

	// The engine's own "this object is cargo now / is cargo no more" bookkeeping, which
	// is what the vanilla aircraft carryall does when it picks a vehicle up:
	//
	//   * the cargo leaves the cell's object list, so the world no longer sees it as
	//     standing anywhere (no cell-content pointer to it, no occupation bits),
	//   * it is linked into the carrier's passenger list instead.
	//
	// Both lists use ObjectClass::NextObject (+0x30) as their link, which is why the
	// cargo can only ever be in one of them - and why the hand-rolled "write Location
	// every frame while it is still registered in a cell" approach corrupted the object.
	//
	//   CellClass::AddContent(ObjectClass*, bool alt)    0x47E8A0  (links, then runs the
	//                                                              object's MarkAllOccupationBits)
	//   CellClass::RemoveContent(ObjectClass*, bool alt) 0x47EA90  (unlinks, then runs the
	//                                                              object's UnmarkAllOccupationBits)
	//   PassengersClass::AddPassenger / RemovePassenger  0x4733A0 / 0x4734B0 (YRpp wrappers)
	// TechnoClass::Limbo, 0x6F6AC0. A limboed object is not part of the world at all: it is
	// not drawn, not selectable, not targetable and not updated. That is exactly the state
	// a carried vehicle has to be in - the carrier draws its model itself (the reference
	// implementation does the same: the payload is limboed and redrawn by the carrier).
	void LimboTechno(TechnoClass* pTechno)
	{
		using LimboFn = bool(__thiscall*)(void*);
		static LimboFn const limbo = reinterpret_cast<LimboFn>(0x6F6AC0);

		if (!pTechno->InLimbo)
			limbo(pTechno);
	}

	// ObjectClass::Unlimbo, 0x5F4EC0. Puts a limboed object back into the world at the given
	// coordinates (cell content, layer list, occupation bits, and InLimbo = false).
	bool UnlimboTechno(TechnoClass* pTechno, const CoordStruct& coords, DirType facing)
	{
		using UnlimboFn = bool(__thiscall*)(void*, CoordStruct*, DirType);
		static UnlimboFn const unlimbo = reinterpret_cast<UnlimboFn>(0x5F4EC0);

		auto crd = coords;
		return unlimbo(pTechno, &crd, facing);
	}

	// Enters the cargo state: the payload leaves the world entirely (Limbo). It is not put
	// on the carrier's passenger list and no Transporter is written - a passenger is limboed
	// by the engine as well and an Unload order would try to unload it.
	void EnterCargoState(UnitClass* pPayload)
	{
		LimboTechno(pPayload);

		pPayload->OnBridge = false;
		pPayload->IsOnCarryall = true;
	}

	// Leaves the cargo state: the payload is put back into the world at the drop cell.
	bool LeaveCargoState(UnitClass* pPayload, CellClass* pDropCell)
	{
		auto const coords = pDropCell->GetCoordsWithBridge();

		if (!UnlimboTechno(pPayload, coords, DirType::North))
			return false;

		pPayload->IsOnCarryall = false;
		pPayload->SetDestination(nullptr, true);

		// The once-per-cell-entry processing the engine runs for a normally moving object
		// (Ares runs the same UpdatePosition(2) for the carryall cargo on unload).
		pPayload->UpdatePosition(PCPType::End);

		return true;
	}

	// The carrier of a payload we hooked on. Raw pointer first for the same reason as
	// ResolveHeldPayload, then the engine reference.
	UnitClass* ResolveHeldCarrier(UnitExt* pExt)
	{
		if (auto const pRaw = pExt->CarryallCarrierRaw)
		{
			if (pRaw->IsAlive && pRaw->Fetch_ID() == pExt->CarryallCarrier.m_ID)
				return pRaw;

			pExt->CarryallCarrierRaw = nullptr;
		}

		auto const pTechno = Carryall::ResolveLiveTechno(pExt->CarryallCarrier);

		return specific_cast<UnitClass*>(pTechno);
	}

	// Whether a techno pointer is still one of the engine's live technos.
	//
	// The array is the engine's own logic list, and an object is taken out of it before
	// its memory is released, so this answers "may I dereference this" without touching
	// the pointer itself. That check is needed because the id based TargetClass lookup
	// does NOT do it: TargetClass::As_Unit (0x6E79B0) only compares the stored RTTI and
	// then returns whatever the index has, which for a vehicle destroyed a moment ago is
	// the address of memory that has already been handed to something else.
	bool IsLiveTechno(const TechnoClass* pTechno)
	{
		if (!pTechno)
			return false;

		for (auto const pItem : TechnoClass::Array)
		{
			if (pItem == pTechno)
				return true;
		}

		return false;
	}

	// Resolves the carrier's stored payload link to a pointer that is safe to
	// dereference, or null when the vehicle it names is gone.
	// Same as ResolvePayload, but for a payload the carrier has already hooked on: a
	// carried object is allowed to be InLimbo (that is what "inside a transport" can look
	// like on the engine side), so only liveness matters here.
	UnitClass* ResolveHeldPayload(UnitExt* pExt)
	{
		// A limboed object is not part of the world and the engine's TargetClass lookup
		// (id index) stops resolving it, so the live pointer is what we use - validated by
		// the id it was stored with so a recycled allocation cannot be mistaken for it.
		if (auto const pRaw = pExt->CarryallPayloadRaw)
		{
			if (pRaw->IsAlive && pRaw->Fetch_ID() == pExt->CarryallPayload.m_ID)
				return pRaw;

			pExt->CarryallPayloadRaw = nullptr;
		}

		auto const pTechno = Carryall::ResolveLiveTechno(pExt->CarryallPayload);
		auto const pUnit = specific_cast<UnitClass*>(pTechno);

		if (!pUnit || !pUnit->IsAlive)
			return nullptr;

		return pUnit;
	}

	UnitClass* ResolvePayload(UnitExt* pExt)
	{
		auto const pTechno = Carryall::ResolveLiveTechno(pExt->CarryallPayload);
		auto const pUnit = specific_cast<UnitClass*>(pTechno);

		if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo)
			return nullptr;

		return pUnit;
	}


	// The pick-up order travels through the engine's MegaMission pipeline, and applying it
	// makes the engine do `SetTarget(the vehicle being lifted)`. That Target is a loaded
	// gun: the firing code happily fires at an explicitly assigned target even when it is
	// friendly (auto-acquisition filters by house, an assigned target does not), so a
	// pick-up order that never turns into a sequence would leave the carrier shooting at
	// its own cargo forever. The pending order is therefore remembered separately and its
	// Target is taken away again as soon as the sequence owns it - or when the order never
	// arrives at all.
	struct PendingPickup
	{
		int CarrierId;
		int TargetId;
		int Frame;
		int PreviousMission;
		TargetClass TargetLink;
		TargetClass PreviousDestination;

		PendingPickup() noexcept
			: CarrierId { 0 }, TargetId { 0 }, Frame { -1000 },
			PreviousMission { -1 }, TargetLink(), PreviousDestination()
		{ }

		void Reset()
		{
			CarrierId = 0;
			TargetId = 0;
			Frame = -1000;
			PreviousMission = -1;
			TargetLink = TargetClass();
			PreviousDestination = TargetClass();
		}
	};

	PendingPickup PendingPickupOrder;

	void NotePendingPickup(TechnoClass* pCarrier, TechnoClass* pTarget)
	{
		PendingPickupOrder.CarrierId = pCarrier->Fetch_ID();
		PendingPickupOrder.TargetId = pTarget->Fetch_ID();
		PendingPickupOrder.Frame = Unsorted::CurrentFrame;
		PendingPickupOrder.PreviousMission = static_cast<int>(pCarrier->CurrentMission);
		PendingPickupOrder.TargetLink = TargetClass(pTarget);

		// FootClass::Destination is what the order application may clear, so remember it.
		auto const pFoot = generic_cast<FootClass*, true>(pCarrier);
		PendingPickupOrder.PreviousDestination = pFoot && pFoot->Destination
			? TargetClass(pFoot->Destination) : TargetClass();
	}

	// Around half a second is far more than a MegaMission event needs to arrive; if it has
	// not by then the order is lost and only its Target is still lying around.
	constexpr int PendingPickupLifetime = 20;

	bool HasFreshPendingPickup(TechnoClass* pCarrier)
	{
		return PendingPickupOrder.CarrierId != 0
			&& PendingPickupOrder.CarrierId == pCarrier->Fetch_ID()
			&& Unsorted::CurrentFrame - PendingPickupOrder.Frame <= PendingPickupLifetime;
	}

	void ClearPendingPickup(TechnoClass* pCarrier)
	{
		if (pCarrier && PendingPickupOrder.CarrierId == pCarrier->Fetch_ID())
			PendingPickupOrder.Reset();
	}

	// Takes the order's Target away from the engine. Runs every frame for the carrier the
	// order was issued on, whether or not the sequence has started yet.
	void ConsumePendingPickupTarget(TechnoClass* pCarrier)
	{
		if (!HasFreshPendingPickup(pCarrier))
			return;

		// The order itself is still on the unit (queued or current): the intake needs that
		// Target to start the sequence, so leave it alone.
		if (pCarrier->CurrentMission == Mission::Unload || pCarrier->QueuedMission == Mission::Unload)
			return;

		auto const pTarget = abstract_cast<TechnoClass*>(pCarrier->Target);

		if (!pTarget || pTarget->Fetch_ID() != PendingPickupOrder.TargetId)
			return;

		// This is the vehicle being lifted, not something to shoot. Dropping it here also
		// means the sequence's own state (not the engine) is the only thing that knows it.
		pCarrier->SetTarget(nullptr);
	}

	// Whether an engine order of ours is waiting: the engine pipeline applies the
	// mission as Mission::Unload, with a target for a pick-up and without one for a drop.
	bool HasUnloadOrder(TechnoClass* pThis)
	{
		return pThis->CurrentMission == Mission::Unload
			|| pThis->QueuedMission == Mission::Unload;
	}

	// Instance-level half of "may this vehicle still be picked up": the type checks live
	// in UnitTypeExt::CanLift, this adds the state of the vehicle itself.
	bool IsAvailablePayload(TechnoClass* pTarget)
	{
		const auto pUnit = specific_cast<UnitClass*>(pTarget);

		if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo || pUnit->IsOnCarryall)
			return false;

		const auto pExt = UnitExt::TryFetch(pUnit);

		// A vehicle that is itself driving a carryall sequence cannot be cargo on top.
		return pExt && pExt->CarryallStatus == CarryallState::None;
	}

	bool IsStillLiftable(UnitClass* pCarrier, UnitClass* pPayload)
	{
		return pPayload && pPayload->IsAlive && !pPayload->InLimbo
			&& !pPayload->IsOnCarryall
			&& IsAvailablePayload(pPayload)
			&& UnitTypeExt::CanLift(pCarrier, pPayload);
	}

	// While the carrier is fetching, holding or putting down a vehicle it is doing cargo
	// duty: it must not engage anything. Two things are needed for that:
	//
	//   * the engine order itself sets TechnoClass::Target to the vehicle being lifted
	//     (the MegaMission application does SetTarget(event.Target)), so the carrier would
	//     otherwise treat a friendly vehicle as its target of interest and keep aiming /
	//     firing at whatever comes into range en route,
	//   * and with MobileFire / OpportunityFire on an armed carrier the engine keeps
	//     acquiring new targets on its own, which is what made an armed carrier fight
	//     while it was supposed to be lifting.
	//
	// Target acquisition is switched off for the whole sequence and put back afterwards.
	// Drops an engine Unload order that arrived while a maneuver was already running
	// (a player pressing the drop key during a pick-up, for instance). The order is not
	// acted on, but it must not stay queued either: it would otherwise fire the moment
	// the maneuver finishes.
	void ClearStrayOrder(TechnoClass* pThis, UnitExt* pExt)
	{
		// The order will not be carried out, so its Target (the vehicle it was meant to
		// lift) must not stay behind as an attack target.
		pThis->SetTarget(nullptr);

		// A jumpjet put on a destination-less Move "settles" where it stands, so an order
		// that never became a sequence puts the unit back into whatever it was doing when
		// the player clicked, not into a fresh Move.
		const bool isOurPending = PendingPickupOrder.CarrierId == pThis->Fetch_ID();
		const int previousMission = isOurPending ? PendingPickupOrder.PreviousMission : -1;
		auto const previousDestination = isOurPending
			? PendingPickupOrder.PreviousDestination.As_Abstract() : nullptr;

		ClearPendingPickup(pThis);

		if (pThis->QueuedMission == Mission::Unload)
			pThis->QueuedMission = Mission::None;

		if (pThis->CurrentMission != Mission::Unload)
			return;

		// The engine already switched to it: take the mission back.
		if (pExt->CarryallStatus == CarryallState::Pursuing)
		{
			pThis->ForceMission(Mission::Move);

			if (auto const pPayload = ResolveHeldPayload(pExt))
				pThis->SetDestination(pPayload->GetCell(), true);

			return;
		}

		if (pExt->CarryallStatus == CarryallState::Dropping && !pExt->CarryallDescending)
		{
			pThis->ForceMission(Mission::Move);

			if (auto const pCell = MapClass::Instance.TryGetCellAt(pExt->CarryallDropCell))
				pThis->SetDestination(pCell, true);

			return;
		}

		if (pExt->CarryallStatus != CarryallState::None)
		{
			// Landing / descending / ascending: the sequence owns the carrier. Do not put
			// back any old mission or destination (a destination here makes the jumpjet fly
			// and hold its own altitude, and the descent stalls).
			pThis->ForceMission(Mission::Guard);
			pThis->SetDestination(nullptr, true);
			return;
		}

		// Put the unit back exactly where it was: same mission, same destination. A jumpjet
		// left on a destination-less Move settles/lands where it stands, and that is what the
		// player sees as "the aircraft sits down as soon as I click".
		pThis->ForceMission(previousMission >= 0
			? static_cast<Mission>(previousMission) : Mission::Guard);

		// Only cell destinations are put back: an object destination can be gone by now and
		// a stale pointer must never be handed to the engine.
		if (previousDestination && previousDestination->WhatAmI() == AbstractType::Cell)
			pThis->SetDestination(previousDestination, true);
	}

	void ResetCarrier(UnitExt* pExt, const char* reason = "?")
	{

		if (auto const pOwner = pExt->OwnerObject())
			ClearPendingPickup(pOwner);

			pExt->CarryallStatus = CarryallState::None;
		pExt->CarryallPayload = TargetClass();
		pExt->CarryallPayloadRaw = nullptr;
		pExt->CarryallDropCell = CellStruct::Empty;
		pExt->CarryallApproachCell = CellStruct::Empty;
		pExt->CarryallStartHeight = 0;
		pExt->CarryallTargetCoords = CoordStruct::Empty;
		pExt->CarryallDescending = false;
	}

	// Takes the vehicle on board. The payload stays a live object and is never limboed;
	// the engine's IsOnCarryall flag is what makes TechnoClass::Draw hang it below the
	// carrier (it shifts the drawn Y down 14 pixels), so no drawing of our own is needed.
	bool AttachPayload(UnitClass* pThis, UnitExt* pExt, UnitClass* pPayload)
	{
		if (!IsStillLiftable(pThis, pPayload))
			return false;

		const auto pPayloadExt = UnitExt::TryFetch(pPayload);

		if (!pPayloadExt)
			return false;

		// If the vehicle was itself the carrier of a sequence, that sequence ends here:
		// it is cargo now.
		ResetCarrier(pPayloadExt, "payload-was-carrier");

		pPayload->SetDestination(nullptr, true);
		pPayload->QueueMission(Mission::Sleep, true);
		pPayload->SetTarget(nullptr);

		// Cargo state: out of the cell's object list. The payload is deliberately NOT put
		// on the carrier's passenger list - the engine limboes passengers, and a limboed
		// object is neither drawn nor updated (see EnterCargoState).
		EnterCargoState(pPayload);

		// The link is written in both directions; the payload side is the marker that
		// says "this one is ours" and is what the P0 fix keys on. The raw pointers are the
		// ones that keep working while the payload is limboed.
		pPayloadExt->CarryallCarrier = TargetClass(pThis);
		pPayloadExt->CarryallCarrierRaw = pThis;
		pExt->CarryallPayloadRaw = pPayload;

		// Hover where we are: a leftover Move order (from the approach) would otherwise keep
		// the carrier flying around with the vehicle still hanging under it.
		HaltLocomotion(pThis);
		pThis->ForceMission(Mission::Guard);
		pThis->SetTarget(nullptr);

		pExt->CarryallPayload = TargetClass(pPayload);
		pExt->CarryallDescending = false;
		pExt->CarryallVerticalSpeed = 0;
		pExt->CarryallStatus = CarryallState::Ascending;
		pExt->CarryallStartHeight = std::max(pExt->CarryallStartHeight, GetFlyHeight(pThis));


		return true;
	}

	// ObjectClass::Unlimbo refuses a cell it considers occupied - and the hovering carrier
	// itself makes its own cell "occupied" no matter what we clear. When that happens the
	// payload is put back into the world directly: cell content list + position update, the
	// same two steps the engine does for a normal ground object. The payload becomes a
	// normal vehicle again (it may be selected/moved), which is exactly what has to happen
	// after a release.
	bool ForcePlacePayload(UnitClass* pPayload, CellClass* pCell)
	{
		auto const coords = pCell->GetCoordsWithBridge();

		pPayload->Location = coords;
		pPayload->InLimbo = false;
		pPayload->OnBridge = false;
		pPayload->IsOnCarryall = false;

		// ObjectClass::Mark(Up) is the engine's own "put this object into the map at its
		// current position" step: it adds the object to the cell's content chain AND to the
		// drawing layer lists. Just appending to the cell content list (the previous
		// attempt) made the vehicle selectable but invisible and unable to take orders.
		if (!pPayload->Mark(MarkType::Up))
		{

			return false;
		}

		pPayload->UpdatePosition(PCPType::End);
		pPayload->SetDestination(nullptr, true);
		pPayload->SetTarget(nullptr);
		pPayload->QueueMission(Mission::Guard, true);


		return true;
	}

	// Puts the payload down on the given cell and lets go of it. Returns false when the
	// engine refused to put it back into the world (e.g. the cell got occupied); the caller
	// then keeps carrying it.
	bool ReleasePayload(UnitClass* pPayload, CellClass* pCell, UnitClass* pCarrier = nullptr)
	{
		if (auto const pExt = UnitExt::TryFetch(pPayload))
		{
			pExt->CarryallCarrier = TargetClass();
			pExt->CarryallCarrierRaw = nullptr;
			pExt->CarryallStatus = CarryallState::None;
		}

		// The engine's Unlimbo refuses to put a vehicle into the cell the carrier is over: a
		// jumpjet counts as an occupant of that cell. Take its registration out of the cell
		// for exactly this call, then put it back - the cell may legally contain the ground
		// vehicle and a jumpjet hovering above it.
		auto const pRegistrar = pCell->Jumpjet;
		const bool tookRegistrar = pCarrier && pRegistrar == pCarrier;

		if (tookRegistrar)
			pCell->Jumpjet = nullptr;

		// The carrier also holds the cell's ground occupation bits: clear them for the
		// release (the engine's own UnmarkAllOccupationBits virtual), otherwise Unlimbo
		// still refuses the cell. The carrier is about to fly away, so nothing has to be
		// put back afterwards.
		if (pCarrier)
			pCarrier->UnmarkAllOccupationBits(pCell->GetCoordsWithBridge());

		const bool unlimboed = LeaveCargoState(pPayload, pCell);

		if (tookRegistrar)
			pCell->Jumpjet = pCarrier;

		if (!unlimboed)
		{

			// The engine refuses to put a vehicle into the cell its (hovering) carrier is
			// over, whatever we clear first. Put it back into the world directly instead of
			// shoving it into a neighbouring cell - the vehicle has to end up in the cell
			// under the aircraft.
			if (!ForcePlacePayload(pPayload, pCell))
				return false;
		}

		pPayload->QueueMission(Mission::Guard, true);


		return true;
	}

	// Keeps the payload glued below the carrier.
	//
	// While carried the payload is in no cell's object list at all (see EnterCargoState),
	// so there is no cell bookkeeping to redo here: writing the position is all the
	// engine needs to draw it hanging below the carrier. Calling UpdatePosition() every
	// frame - the earlier attempt - is exactly what corrupted the object.
	void FollowPayload(UnitClass* pThis, UnitClass* pPayload)
	{
		// The payload is limboed (no entity, no AI). Keep its Location and facing in sync
		// with the carrier: the carrier's draw hook draws the payload's model at exactly
		// this position (mode + the 14px IsOnCarryall hang offset the engine applies).
		pPayload->Location = pThis->GetCoords();

		// The cargo hangs where the carrier is: it copies the body facing of the carrier
		// (the engine copies one facing structure into both of the cargo's facing
		// structures, so it is done the same way here).
		pPayload->PrimaryFacing = pThis->PrimaryFacing;
		pPayload->SecondaryFacing = pThis->PrimaryFacing;

		// A vehicle hanging here is not falling (its Z is held by us, not by gravity).
		pPayload->IsFallingDown = false;

		if (pPayload->Deactivated)
		{
			auto const pLoco = pPayload->Locomotor.GetInterfacePtr();

			if (pLoco && pLoco->Is_Powered())
				pLoco->Power_Off();
		}
	}

	// ---- state machine steps ----

	void TryStartPickup(UnitClass* pThis, UnitExt* pExt, TechnoClass* pOrderTarget = nullptr)
	{
		auto pTarget = pOrderTarget ? pOrderTarget : abstract_cast<TechnoClass*>(pThis->Target);

		// Belt and braces: if the engine Target of this order is already gone (it is only a
		// convenience carrier for "which vehicle"), the pending order still remembers it.
		if (!pTarget && HasFreshPendingPickup(pThis))
			pTarget = Carryall::ResolveLiveTechno(PendingPickupOrder.TargetLink);

		if (!pTarget || !Carryall::CanLiftNow(pThis, pTarget))
		{

			// The order cannot be honoured (the vehicle died or was taken in the
			// meantime): drop it instead of leaving it queued on a carrier type, whose
			// vanilla unload has nothing to do anyway.
			ClearStrayOrder(pThis, pExt);
			return;
		}

		pExt->CarryallPayload = TargetClass(pTarget);
		pExt->CarryallStatus = CarryallState::Pursuing;
		ClearPendingPickup(pThis);
		pExt->CarryallApproachCell = CellStruct::Empty;
		pExt->CarryallDescending = false;
		pExt->CarryallStartHeight = 0;

		// Take the order over: the mission stops being Unload right away, so this exact
		// order can never be consumed twice, and the engine's Move handler flies the
		// carrier towards the vehicle (the same shape the vanilla aircraft carryall's
		// tote click turns into).
		pThis->ForceMission(Mission::Move);
		pThis->SetDestination(pTarget->GetCell(), true);
		pExt->CarryallApproachCell = pTarget->GetCell()->MapCoords;

	}

	void TryStartDrop(UnitClass* pThis, UnitExt* pExt)
	{
		// Any Unload order that reaches a carrier which is already holding a payload is
		// the drop order - a second pick-up cannot be ordered while the carrier is busy,
		// and the engine may well have re-acquired an ordinary attack target in the
		// meantime, so Target is not used to tell the two apart.
		// ResolveHeldPayload, NOT ResolvePayload: the held payload is limboed, and the
		// strict resolver rejects limboed objects. Using the strict one here made every
		// drop think there was no payload - the previous version then even re-entered
		// TryStartDrop from its failure path without changing anything, an unbounded
		// recursion that overflowed the stack and silently killed the game.
		auto const pPayload = ResolveHeldPayload(pExt);

		if (!pPayload)
		{
			// The payload is really gone. ResetCarrier clears the links (nothing can be
			// orphaned when the object no longer exists) and the stray Unload mission is
			// cleared so the vanilla unload body cannot run on the carrier.
			ResetCarrier(pExt, "drop-order:payload-gone");
			ClearStrayOrder(pThis, pExt);
			return;
		}

		// Never drop our own link while the payload is still held: it is limboed and would
		// be orphaned outside the world forever. The marker can only have been cleared by
		// something else reading it, so put it back and keep carrying.
		pPayload->IsOnCarryall = true;

		// Prefer straight down, at the cell the carrier is over (that is the cell the player
		// sees under the aircraft). If that cell cannot hold the vehicle - a building, water,
		// a bridge, an occupied cell - release onto the nearest usable GROUND cell instead
		// and fly over there first, rather than dropping the vehicle into the building or
		// losing it.
		CellStruct cell = pThis->GetCell()->MapCoords;
		bool overCell = true;

		if (!Carryall::IsDropCellUsable(cell, pPayload, pThis))
		{
			overCell = false;

			if (!Carryall::FindDropCell(pThis, pPayload, cell))
			{
				// Nowhere to put it down: keep carrying and keep the order for later.
				pThis->ForceMission(Mission::Guard);
				return;
			}
		}

		pExt->CarryallDropCell = cell;

		// Commit to the descent immediately when already over the cell; otherwise fly over
		// it first (UpdateDropping drives that approach).
		pExt->CarryallDescending = overCell;
		pExt->CarryallStartHeight = std::max({ GetClimbStep(pThis), GetFlyHeight(pThis),
			pThis->Type->JumpjetHeight });
		pExt->CarryallVerticalSpeed = 0;
		pExt->CarryallStatus = CarryallState::Dropping;
		HaltLocomotion(pThis);
		pThis->ForceMission(Mission::Guard);

	}

	void UpdatePursuing(UnitClass* pThis, UnitExt* pExt)
	{
		auto const pTarget = ResolvePayload(pExt);

		if (!IsStillLiftable(pThis, pTarget))
		{
			ResetCarrier(pExt, "pursuit:not-liftable");
			return;
		}

		// A carrier that was left at ground level (an interrupted sequence, a save, ...) must
		// get back to its cruise altitude instead of flying along the ground.
		const int cruise = std::max(GetClimbStep(pThis), pThis->Type->JumpjetHeight);
		const int currentFly = GetFlyHeight(pThis);

		if (currentFly > 0 && currentFly < cruise)
			MoveAltitudeTo(pThis, cruise);

		// A vehicle that is itself on the move changes cells while the carrier flies at it;
		// that must never abort the approach. A move destination that is clearly somewhere
		// else is the player taking over, and the carrier is simply left to finish that
		// move - once it has no destination any more the approach resumes below.
		const auto targetCell = pTarget->GetCell()->MapCoords;
		auto const pDestCell = abstract_cast<CellClass*>(pThis->Destination);

		if (pDestCell
			&& (std::abs(pDestCell->MapCoords.X - targetCell.X) > 2
				|| std::abs(pDestCell->MapCoords.Y - targetCell.Y) > 2))
		{
			return;
		}

		// The carrier has to be DIRECTLY above the vehicle to start the descent, exactly
		// like the reference: it flies onto the vehicle's own cell first and only then
		// comes down. Being "near enough" is not enough - a grab from a neighbouring cell
		// was one of the things that made the pick-up look wrong.
		const auto carrierCoords = pThis->GetCoords();
		const auto targetCoords = pTarget->GetCoords();
		const int dx = carrierCoords.X - targetCoords.X;
		const int dy = carrierCoords.Y - targetCoords.Y;
		const auto carrierCell = pThis->GetCell()->MapCoords;
		const int cellDx = carrierCell.X - targetCell.X;
		const int cellDy = carrierCell.Y - targetCell.Y;
		const int distanceSq = dx * dx + dy * dy;
		const bool overTarget = cellDx == 0 && cellDy == 0
			&& distanceSq <= CellDiagonalSq;

		if (!overTarget)
		{
			// Re-issue the approach order when there is none any more (the player's own
			// move was just finished, the path was consumed, ...) or when the vehicle has
			// moved to another cell. Re-issuing it every frame would reset the path every
			// frame, so a destination that already is the vehicle's cell is left alone.
			const bool destinationMissing = pThis->Destination == nullptr;
			const bool targetMoved = pExt->CarryallApproachCell.X != targetCell.X
				|| pExt->CarryallApproachCell.Y != targetCell.Y;
			const bool destinationIsTargetCell = pDestCell
				&& pDestCell->MapCoords.X == targetCell.X
				&& pDestCell->MapCoords.Y == targetCell.Y;

			if ((destinationMissing || targetMoved) && !destinationIsTargetCell)
			{
				pExt->CarryallApproachCell = targetCell;
				pThis->ForceMission(Mission::Move);
				pThis->SetDestination(pTarget->GetCell(), true);
			}

			return;
		}

		// Close enough: remember where the vehicle stands and the altitude to come back
		// up to, then come down on that spot.
		pExt->CarryallTargetCoords = targetCoords;

		if (!pExt->CarryallDescending)
		{
			pExt->CarryallDescending = true;

			// Baseline for the climb back: the type's own cruise altitude. Recording the
			// current height is wrong when the carrier is already low (ground level, e.g.
			// after an interrupted sequence), because the climb then has nowhere to go.
			pExt->CarryallStartHeight = std::max(GetFlyHeight(pThis), pThis->Type->JumpjetHeight);
		}

		// Down we go. Clear the movement order and the destination first: a jumpjet still
		// holding a destination keeps being flown by its movement AI (it circles the target
		// and even climbs a step per frame), which is the "it flies around wildly" report.
		// The cell centre is pinned here so the descent is a straight vertical line over the
		// vehicle (the jumpjet kinematics are frozen for the duration - see the H4 hook).
		HaltLocomotion(pThis);
		pThis->ForceMission(Mission::Guard);
		SnapOverCell(pThis, pTarget->GetCell());
		pExt->CarryallVerticalSpeed = 0;
		pExt->CarryallStatus = CarryallState::Landing;
	}

	void UpdateLanding(UnitClass* pThis, UnitExt* pExt)
	{
		auto const pTarget = ResolvePayload(pExt);

		// The descent stays committed: drifting out of the cell on the way down is normal
		// for a jumpjet and must not cancel it. Only a vehicle that is gone or no longer
		// acceptable ends the attempt.
		if (!pTarget || !pTarget->IsAlive || pTarget->InLimbo || pTarget->IsOnCarryall
			|| !UnitTypeExt::CanLift(pThis, pTarget))
		{
			ResetCarrier(pExt, "landing:target-gone");
			return;
		}


		// The vehicle is picked up from the ground: the carrier comes all the way down onto
		// the vehicle's cell, parks there and only then grabs it. A grab from mid-air was
		// the "it works but that is not how it should look" version.
		if (pThis->GetHeight() > NearGroundHeight)
		{
			HaltLocomotion(pThis);

			if (pThis->CurrentMission != Mission::Guard)
				pThis->ForceMission(Mission::Guard);

			MoveAltitudeTo(pThis, 0);
			return;
		}

		// On the ground: pin it onto the vehicle's own cell, park it (Height 0) and take the
		// vehicle. The jumpjet drifts sideways while its altitude is driven, so the cell the
		// grab happens in is forced here rather than assumed.
		ParkOnGround(pThis, pTarget->GetCell());

		if (!AttachPayload(pThis, pExt, pTarget))
			ResetCarrier(pExt, "landing:attach-failed");
	}

	void UpdateCarrying(UnitClass* pThis, UnitExt* pExt)
	{
		// The payload follow itself runs in Update(), before the state switch, so that it
		// also covers the climb after a pick-up and the flight to a drop cell.
		auto const pPayload = ResolveHeldPayload(pExt);

		if (!pPayload || !pPayload->IsAlive || !pPayload->IsOnCarryall)
		{

			ResetCarrier(pExt, "carrying:payload-lost");
		}
	}

	void UpdateDropping(UnitClass* pThis, UnitExt* pExt)
	{
		auto const pPayload = ResolveHeldPayload(pExt);

		if (!pPayload || !pPayload->IsAlive || !pPayload->IsOnCarryall)
		{

			ResetCarrier(pExt, "dropping:payload-lost");
			ClearStrayOrder(pThis, pExt);
			return;
		}

		auto pCell = MapClass::Instance.TryGetCellAt(pExt->CarryallDropCell);

		if (!pCell)
		{
			ResetCarrier(pExt, "dropping:no-cell");
			ClearStrayOrder(pThis, pExt);
			return;
		}

		if (!pExt->CarryallDescending)
		{
			// Fly over the chosen cell first (used when the cell under the aircraft cannot
			// hold the vehicle and the nearest usable ground cell is somewhere else).
			if (pThis->GetCell()->MapCoords != pExt->CarryallDropCell)
			{
				// The destination has to BE the chosen cell. Only setting it when there is
				// no destination at all left an old move order in place, so the carrier flew
				// off somewhere else ("it looks like it picks a release point, but it just
				// wanders away and only releases if it happens to fly over that point").
				auto const pDest = abstract_cast<CellClass*>(pThis->Destination);
				const bool destinationIsDropCell = pDest
					&& pDest->MapCoords == pExt->CarryallDropCell;

				if (!destinationIsDropCell)
				{
					pThis->ForceMission(Mission::Move);
					pThis->SetDestination(pCell, true);
				}
				else if (pThis->CurrentMission != Mission::Move)
				{
					// The mission can be overwritten by a leftover order; the approach owns
					// the carrier until it is over the cell.
					pThis->ForceMission(Mission::Move);
				}

				return;
			}

			pExt->CarryallDescending = true;
			pExt->CarryallStartHeight = std::max({ GetClimbStep(pThis), GetFlyHeight(pThis),
				pThis->Type->JumpjetHeight });
			pExt->CarryallVerticalSpeed = 0;
			HaltLocomotion(pThis);
			pThis->ForceMission(Mission::Guard);
		}

		// The drop ends on the ground as well: the carrier comes down onto the chosen cell,
		// parks there and only then lets go of the vehicle (the reference's "ground unload"
		// behaviour). The jumpjet's Location.Z sits one climb step above its Height, so
		// subtract the climb step twice - otherwise the step is a fixed point and the
		// carrier hangs in the air forever.
		if (pThis->GetHeight() > DropReleaseHeight)
		{
			// Stop the locomotion EVERY frame, exactly like the pick-up descent does. The
			// jumpjet's own movement AI otherwise keeps a destination (its guard point) and
			// flies the carrier around while holding an altitude of its own, which fights
			// StepAltitude - observed as "it descends a bit and then hangs in the air".
			HaltLocomotion(pThis);

			if (pThis->CurrentMission != Mission::Guard)
				pThis->ForceMission(Mission::Guard);

			MoveAltitudeTo(pThis, DropReleaseHeight);
			return;
		}

		// Low enough: let go while still hovering, NOT parked. A parked carrier occupies the
		// cell it stands on, the engine then refuses to Unlimbo the vehicle there and the
		// payload ends up in a neighbouring cell - exactly the "it is not in the cell the
		// aircraft is in" report. Hovering close above the cell keeps it free, and pinning
		// the carrier onto the cell centre makes the release happen in that cell.
		SnapOverCell(pThis, pCell);

		// Last check: the cell is the one the carrier is over, but the world can differ from
		// what the movement checks thought when the order was given (a building, a newly
		// occupied cell). Release onto the nearest usable GROUND cell rather than forcing the
		// vehicle into a building - forcing it there made it vanish.
		if (!Carryall::IsDropCellUsable(pExt->CarryallDropCell, pPayload, pThis))
		{
			CellStruct alternativeCell;

			if (Carryall::FindDropCell(pThis, pPayload, alternativeCell))
			{
				if (auto const pAlternative = MapClass::Instance.TryGetCellAt(alternativeCell))
				{
					pExt->CarryallDropCell = alternativeCell;
					pExt->CarryallDescending = false;
					pCell = pAlternative;


					return;
				}
			}

			// No free ground at all: keep holding it.
			pExt->CarryallDescending = false;
			pExt->CarryallStatus = CarryallState::Carrying;
			return;
		}


		bool released = ReleasePayload(pPayload, pCell, pThis);

		if (!released)
		{
			// The engine refused (the parked carrier itself can count as an occupant of its
			// own cell). Look for a DIFFERENT, free neighbouring cell once instead of
			// retrying the same spot forever.
			for (int radius = 1; radius <= 2 && !released; ++radius)
			{
				for (int dy = -radius; dy <= radius && !released; ++dy)
				{
					for (int dx = -radius; dx <= radius && !released; ++dx)
					{
						if (std::max(std::abs(dx), std::abs(dy)) != radius)
							continue;

						const CellStruct candidate
						{
							static_cast<short>(pCell->MapCoords.X + dx),
							static_cast<short>(pCell->MapCoords.Y + dy)
						};

						if (!Carryall::IsDropCellUsable(candidate, pPayload, nullptr))
							continue;

						if (auto const pCandidate = MapClass::Instance.TryGetCellAt(candidate))
						{
							if (pCandidate == pCell)
								continue;

							if (ReleasePayload(pPayload, pCandidate, pThis))
							{
								pExt->CarryallDropCell = candidate;
								pCell = pCandidate;
								released = true;
							}
						}
					}
				}
			}
		}

		if (!released)
			return;

		// The carrier is on the ground / low, so the climb back to cruise height is the next
		// thing UpdateAscending does. Missing that transition is why the aircraft used to
		// keep flying at ground level after a drop.


		pExt->CarryallPayload = TargetClass();
		pExt->CarryallDropCell = CellStruct::Empty;
		pExt->CarryallDescending = false;
		pThis->SetDestination(nullptr, true);
		pExt->CarryallStatus = CarryallState::Ascending;
	}

	void UpdateAscending(UnitClass* pThis, UnitExt* pExt)
	{
		// Climb back to the altitude the descent started from. The locomotion has no
		// destination, so the engine leaves the carrier alone and the altitude step moves it.
		const int height = GetFlyHeight(pThis);

		if (height >= pExt->CarryallStartHeight)
		{
			HaltLocomotion(pThis);
			pExt->CarryallDescending = false;

			auto const pPayload = ResolveHeldPayload(pExt);

			if (pPayload && pPayload->IsAlive && pPayload->IsOnCarryall)
			{
				pExt->CarryallStatus = CarryallState::Carrying;

			}
			else if (!pPayload)
			{
				// A drop: the payload was released on purpose, so this is the normal end of
				// the sequence (the carrier is back at cruise altitude).

				ResetCarrier(pExt, "ascend:done");
			}
			else
			{
				ResetCarrier(pExt, "ascend:payload-lost");
			}

			return;
		}

		MoveAltitudeTo(pThis, pExt->CarryallStartHeight);
	}
}

TechnoClass* Carryall::ResolveLiveTechno(TargetClass& link)
{
	auto const pTechno = link.As_Techno();

	if (!pTechno || !IsLiveTechno(pTechno))
		return nullptr;

	return pTechno;
}

bool Carryall::IsCarrierType(TechnoClass* pCarrier)
{
	const auto pUnit = specific_cast<UnitClass*>(pCarrier);

	if (!pUnit)
		return false;

	const auto pTypeExt = UnitTypeExt::TryFetch(pUnit->Type);

	return pTypeExt && pTypeExt->IsCarryall();
}

bool Carryall::IsFreeCarrier(TechnoClass* pCarrier)
{
	const auto pUnit = specific_cast<UnitClass*>(pCarrier);

	if (!pUnit || pUnit->IsOnCarryall || !IsCarrierType(pUnit))
		return false;

	const auto pExt = UnitExt::TryFetch(pUnit);

	return pExt && pExt->CarryallStatus == CarryallState::None;
}

bool Carryall::IsCarrying(TechnoClass* pCarrier)
{
	const auto pUnit = specific_cast<UnitClass*>(pCarrier);

	if (!pUnit || !IsCarrierType(pUnit))
		return false;

	const auto pExt = UnitExt::TryFetch(pUnit);

	return pExt && pExt->CarryallStatus == CarryallState::Carrying;
}

bool Carryall::CanLiftNow(TechnoClass* pCarrier, TechnoClass* pTarget)
{
	return pTarget
		&& IsFreeCarrier(pCarrier)
		&& IsAvailablePayload(pTarget)
		&& UnitTypeExt::CanLift(pCarrier, pTarget);
}

bool Carryall::IssuePickupOrder(UnitClass* pCarrier, TechnoClass* pTarget)
{
	if (!pCarrier || !pTarget || !CanLiftNow(pCarrier, pTarget))
		return false;

	// No engine order. ClickedMission(Mission::Unload, target) executes locally right away
	// and the engine turns it into its own "move to an unload spot" order, which overwrites
	// the destination we set below - in game the aircraft then idled next to the target
	// instead of flying to it ("the lift cursor click does nothing, only a normal click
	// works"). The sequence is therefore started here directly, exactly like the drop is.
	NotePendingPickup(pCarrier, pTarget);

	if (auto const pExt = UnitExt::TryFetch(pCarrier))
	{
		if (pExt->CarryallStatus == CarryallState::None)
			TryStartPickup(pCarrier, pExt, pTarget);
	}


	return true;
}

bool Carryall::IssueDropOrder(UnitClass* pCarrier)
{
	if (!pCarrier || !IsCarrying(pCarrier))
		return false;

	// The drop is started directly, not through an engine Unload order: executing that
	// order locally is what consumed/killed the carried payload and left the carrier with a
	// live Unload mission (which then ran the vanilla unload body on a deployable carrier -
	// the aircraft "sat down" and never flew again).
	auto const pExt = UnitExt::TryFetch(pCarrier);

	if (!pExt)
		return false;

	TryStartDrop(pCarrier, pExt);


	return pExt->CarryallStatus == CarryallState::Dropping;
}

void Carryall::Update(UnitClass* pThis)
{
	const auto pExt = UnitExt::TryFetch(pThis);

	if (!pExt)
		return;

	// The held vehicle can be destroyed by something else while the carrier still links to
	// it. Once the object is really dead the links can be dropped safely - nothing can be
	// orphaned outside the world any more - and the carrier returns to normal duty.
	if (pExt->CarryallStatus != CarryallState::None && pExt->CarryallPayloadRaw
		&& !pExt->CarryallPayloadRaw->IsAlive)
	{
		ResetCarrier(pExt, "payload-died");
		ClearStrayOrder(pThis, pExt);
		return;
	}

	// A unit that is itself being carried is driven by UpdatePayload instead.
	if (pThis->IsOnCarryall)
		return;

	const bool isCarrier = IsCarrierType(pThis);

	// A unit that is not a carrier and is not in the middle of anything has nothing to
	// do here; that is the common case for every unit in the game.
	if (!isCarrier && pExt->CarryallStatus == CarryallState::None)
		return;


	// Consume engine orders before the state switch. Pick-up arrives as Unload + a
	// target, drop as Unload while a payload is held; from here on the state field
	// decides what happens, and the mission is taken over so an order cannot be consumed
	// twice.
	if (isCarrier && HasUnloadOrder(pThis))
	{

		if (pExt->CarryallStatus == CarryallState::None)
			TryStartPickup(pThis, pExt);
		else if (pExt->CarryallStatus == CarryallState::Carrying)
			TryStartDrop(pThis, pExt);
		else
			ClearStrayOrder(pThis, pExt);
	}

	// The engine Target the order sets must never survive as an attack target: clear it as
	// soon as the order was turned into a sequence (or never arrived at all). This must run
	// AFTER the order intake above - the intake is what turns that Target into the sequence.
	if (isCarrier)
		ConsumePendingPickupTarget(pThis);


	// Cargo duty: the carrier keeps normal target acquisition and firing. The only rule is
	// that the vehicle it is carrying (or is on its way to collect) must never be its
	// target - the player Alt-clicked that vehicle to lift it, and with an assigned target
	// the engine would happily shoot a friendly unit.
	if (pExt->CarryallStatus != CarryallState::None && isCarrier)
	{
		auto const pCurrentTarget = abstract_cast<TechnoClass*>(pThis->Target);

		if (pCurrentTarget)
		{
			auto const pHeldPayload = ResolveHeldPayload(pExt);

			if (pHeldPayload && pCurrentTarget == pHeldPayload)
			{
				pThis->SetTarget(nullptr);
			}
			else if (HasFreshPendingPickup(pThis)
				&& pCurrentTarget == Carryall::ResolveLiveTechno(PendingPickupOrder.TargetLink))
			{
				pThis->SetTarget(nullptr);
			}
		}

		// The engine may have applied a leftover order (or its echo) and switched the
		// carrier to Attack/Guard in the middle of the approach. The sequence owns the
		// carrier's movement while it is pursuing, so put the mission back.
		if (pExt->CarryallStatus == CarryallState::Pursuing
			&& pThis->CurrentMission != Mission::Move)
		{
			pThis->ForceMission(Mission::Move);
		}
	}

	// Keep the hooked-on payload glued below the carrier while it is attached, whatever
	// the carrier is doing (climbing back up, flying to the drop cell, carrying).
	switch (pExt->CarryallStatus)
	{
	case CarryallState::Ascending:
	case CarryallState::Dropping:
	case CarryallState::Carrying:
	{
		auto const pPayload = ResolveHeldPayload(pExt);

		if (pPayload && pPayload->IsAlive && pPayload->IsOnCarryall)
		{
			FollowPayload(pThis, pPayload);
		}

		break;
	}

	default:
		break;
	}

	switch (pExt->CarryallStatus)
	{
	case CarryallState::None:
		break;

	case CarryallState::Pursuing:
		UpdatePursuing(pThis, pExt);
		break;

	case CarryallState::Landing:
		UpdateLanding(pThis, pExt);
		break;

	case CarryallState::Carrying:
		UpdateCarrying(pThis, pExt);
		break;

	case CarryallState::Dropping:
		UpdateDropping(pThis, pExt);
		break;

	case CarryallState::Ascending:
		UpdateAscending(pThis, pExt);
		break;
	}
}

void Carryall::UpdatePayload(UnitClass* pThis)
{
	const auto pExt = UnitExt::TryFetch(pThis);

	if (!pExt)
		return;

	// Only a payload that OUR attach step hooked on is dealt with here. A vanilla
	// aircraft-carryall cargo has an empty link and is left completely alone - reading
	// that empty link as "the carrier died" is exactly the regression that killed
	// vanilla cargo.
	if (pExt->CarryallCarrier.m_ID == 0)
		return;

	auto const pCarrier = ResolveHeldCarrier(pExt);

	const bool carrierAlive = pCarrier && pCarrier->IsAlive;

	if (carrierAlive && !pThis->IsOnCarryall)
	{
		// Still carried-looking state is gone but the carrier lives: the engine let go of
		// us by some route of its own. Just forget the mark.
		pExt->CarryallCarrier = TargetClass();
		return;
	}

	if (carrierAlive)
		return;

	// The carrier was destroyed underneath us: the payload dies with it, the same way the
	// engine destroys carried cargo (health as damage, C4 warhead). This runs even when
	// the engine already dropped the carried flag/passenger link on the way.
	pExt->CarryallCarrier = TargetClass();
	pThis->IsOnCarryall = false;

	int damage = pThis->Health;
	pThis->ReceiveDamage(&damage, 0, RulesClass::Instance->C4Warhead, nullptr, true, true, nullptr);

}

bool Carryall::BlocksDeploy(TechnoClass* pTechno)
{
	if (!pTechno)
		return false;

	// A vehicle hanging under a carrier must not deploy itself.
	if (pTechno->IsOnCarryall)
		return true;

	auto const pUnit = specific_cast<UnitClass*>(pTechno);

	if (!pUnit || !Carryall::IsCarrierType(pUnit))
		return false;

	auto const pExt = UnitExt::TryFetch(pUnit);

	return pExt && pExt->CarryallStatus != CarryallState::None;
}

UnitClass* Carryall::GetHeldPayload(TechnoClass* pCarrier)
{
	auto const pUnit = specific_cast<UnitClass*>(pCarrier);

	if (!pUnit || !Carryall::IsCarrierType(pUnit))
		return nullptr;

	auto const pExt = UnitExt::TryFetch(pUnit);

	if (!pExt || pExt->CarryallStatus == CarryallState::None)
		return nullptr;

	auto const pPayload = ResolveHeldPayload(pExt);

	if (!pPayload || !pPayload->IsAlive || !pPayload->IsOnCarryall)
		return nullptr;

	return pPayload;
}

void Carryall::OnCarrierDestroyed(TechnoClass* pCarrier)
{
	auto const pUnit = specific_cast<UnitClass*>(pCarrier);

	if (!pUnit)
		return;

	auto const pExt = UnitExt::TryFetch(pUnit);

	if (!pExt || pExt->CarryallStatus == CarryallState::None)
		return;

	auto const pPayload = ResolveHeldPayload(pExt);

	if (!pPayload || !pPayload->IsAlive)
	{
		ResetCarrier(pExt, "carrier-destroyed:no-payload");
		return;
	}


	ResetCarrier(pExt, "carrier-destroyed");

	// Ares does exactly this for the vanilla carryall cargo: both sides get one hit with the
	// C4 warhead, which is the engine's own "killed by its transport" damage.
	int damage = pPayload->Health;
	pPayload->ReceiveDamage(&damage, 0, RulesClass::Instance->C4Warhead,
		pCarrier, true, true, nullptr);
}

bool Carryall::IsVerticallyControlled(TechnoClass* pTechno)
{
	auto const pUnit = specific_cast<UnitClass*>(pTechno);

	if (!pUnit || !Carryall::IsCarrierType(pUnit))
		return false;

	auto const pExt = UnitExt::TryFetch(pUnit);

	if (!pExt)
		return false;

	switch (pExt->CarryallStatus)
	{
	case CarryallState::Landing:
	case CarryallState::Ascending:
		return true;

	case CarryallState::Dropping:
		// Only the vertical part is frozen. While the carrier is still flying to the chosen
		// drop cell (approach phase) its locomotion must run, or it stops dead in the air and
		// ignores every order.
		return pExt->CarryallDescending;

	default:
		return false;
	}
}


bool Carryall::BlocksVanillaUnload(TechnoClass* pCarrier)
{
	const auto pUnit = specific_cast<UnitClass*>(pCarrier);

	if (!pUnit)
		return false;

	const auto pExt = UnitExt::TryFetch(pUnit);

	return pExt && pExt->CarryallStatus != CarryallState::None;
}

bool Carryall::IsOurCarriedPayload(TechnoClass* pPayload)
{
	const auto pUnit = specific_cast<UnitClass*>(pPayload);

	if (!pUnit || !pUnit->IsOnCarryall)
		return false;

	const auto pExt = UnitExt::TryFetch(pUnit);

	return pExt && pExt->CarryallCarrier.m_ID != 0;
}

bool Carryall::IsDropCellUsable(const CellStruct& cell, UnitClass* pPayload, UnitClass* pCarrier)
{
	const auto pCell = MapClass::Instance.TryGetCellAt(cell);

	if (!pCell)
		return false;

	if (!MapClass::Instance.IsWithinUsableArea(pCell, true))
		return false;

	if (pCell->GetBuilding() != nullptr)
		return false;

	// Bridge cells are not supported as drop targets for now: dropping onto the ground
	// layer of a cell with a bridge above it needs layer-aware placement that is not
	// worth the risk here.
	if (pCell->ContainsBridge())
		return false;

	// Another jumpjet parks in this cell: two jumpjets in one cell are not allowed.
	// The carrier's own hover cell is fine - that is exactly where a drop in place
	// lands, and the carrier lifts off right after the release.
	if (pCell->Jumpjet != nullptr && pCell->Jumpjet != pPayload && pCell->Jumpjet != pCarrier)
		return false;

	const auto pType = pPayload->Type;

	// Terrain passability for the payload's own movement class. Units are ignored here,
	// they are checked one by one below.
	if (!pCell->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone,
		pCell->GetLevel(), false))
	{
		return false;
	}

	if (auto const pUnit = pCell->GetUnit(false); pUnit && pUnit != pPayload && pUnit != pCarrier)
		return false;

	if (pCell->GetAircraft(false) != nullptr)
		return false;

	if (pCell->GetTerrain(false) != nullptr)
		return false;

	return true;
}

bool Carryall::FindDropCell(UnitClass* pCarrier, UnitClass* pPayload, CellStruct& outCell)
{
	const auto origin = pCarrier->GetCell()->MapCoords;

	if (IsDropCellUsable(origin, pPayload, pCarrier))
	{
		outCell = origin;
		return true;
	}

	for (int radius = 1; radius <= MaxDropSearchRadius; ++radius)
	{
		for (int dy = -radius; dy <= radius; ++dy)
		{
			for (int dx = -radius; dx <= radius; ++dx)
			{
				// Only the ring at this distance; the inner cells were checked by the
				// previous rounds.
				if (std::max(std::abs(dx), std::abs(dy)) != radius)
					continue;

				const CellStruct cell
				{
					static_cast<short>(origin.X + dx),
					static_cast<short>(origin.Y + dy)
				};

				if (IsDropCellUsable(cell, pPayload, pCarrier))
				{
					outCell = cell;
					return true;
				}
			}
		}
	}

	return false;
}

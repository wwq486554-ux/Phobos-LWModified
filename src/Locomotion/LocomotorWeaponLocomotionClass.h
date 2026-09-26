#pragma once

#include <LocomotionClass.h>
#include <Interfaces.h>
#include <Utilities/Enum.h>

/*
	LocomotorWeapon - a self-managing, generic version of the "magnetron"
	(IsLocomotor=yes warhead) locomotor.

	Vanilla only implements the *ending* of the piggyback for Jumpjet. Every
	other Locomotor= CLSID leaves FootClass::IsAttackedByLocomotor set forever,
	and since FootClass::SetDestination (0x4D94BE) hard-rejects any non-null
	destination while that flag is set, the victim can never move again.

	This class is installed as the victim's locomotor (the trigger layer swaps
	the warhead's CLSID for ours for the duration of ImbueLocomotor). It keeps
	the victim's previous locomotor as its Piggybacker and, depending on the
	configured mode, moves the victim through a second, vanilla locomotor (the
	"Mover") or through its own logic. On release it restores the previous
	locomotor and clears every flag the vanilla auto-end machinery relies on.
*/

struct LocomotorWeaponConfig
{
	LocoWeaponMode Mode { LocoWeaponMode::Auto };
	LocoWeaponEndAction EndAction { LocoWeaponEndAction::Restore };
	int Speed { -1 };
	int Height { -1 };
	int ClimbRate { -1 };
	int DescendRate { -1 };
	int StopDistance { -1 };
	int Duration { -1 };
	bool EndOnArrival { false };
	bool ReleaseOnFirerStop { true };
	bool FallingDamage { true };
	int MeteorDamage { -1 };
	// Indices instead of pointers so that the whole config stays savegame-safe.
	int WarheadIndex { -1 };
	int AnimIndex { -1 };
	int MeteorAnimIndex { -1 };
};

// Phases of the movement this class performs itself (air / meteor).
enum class LocoWeaponPhase
{
	Idle = 0,
	Lift,
	Cruise,
	Descend,
	Landed
};

namespace LocomotorWeapon
{
	// Filled by the trigger layer (the 0x4696CE hook) for the duration of the
	// ImbueLocomotor call and cleared immediately afterwards. Begin_Piggyback
	// runs synchronously inside that window, so it can copy everything it
	// needs into its own (saved) state and never look at these again.
	extern CLSID CurrentRequestedCLSID;
	extern LocomotorWeaponConfig CurrentConfig;
	extern bool Applying;

	bool IsJumpjetCLSID(const CLSID& clsid);
	bool IsDropPodCLSID(const CLSID& clsid);
	bool IsNullCLSID(const CLSID& clsid);

	// Diagnostics: compact labels for the debug log. Both return a pointer into
	// a small rotating static buffer, so they can be passed straight to printf
	// but must not be stored.
	const char* TargetLabel(FootClass* pTarget);
	const char* TechnoLabel(const TechnoClass* pTechno);
	const char* PointerLabel(const void* pPointer);

	// --- Diagnostics: post-release victim watch -----------------------------
	// This locomotor and the bug it may leave behind ("the victim cannot be
	// damaged any more") are on opposite sides of a frame boundary: we are gone
	// by the time the player shoots at the victim. So every victim can be
	// remembered here for a while, and two hooks that have nothing to do with
	// this feature report what happens to it afterwards:
	//   * TechnoClass::ReceiveDamage (Ext/Techno/Hooks.ReceiveDamage.cpp)
	//   * TechnoClass::AI            (Ext/Techno/Hooks.cpp, the periodic probe)
	// Purely diagnostic: nothing here is serialized, and no logic reads it.
	//
	// The whole harness is compiled OUT of the shipped build (2026-09-24: the
	// "cannot be damaged" report stopped reproducing, and the multiplayer
	// validation has never been run, so the chase was parked). Flip
	// Diagnostics to true, rebuild and redeploy to arm it again; the handover
	// manual has a section on what it prints, how to read it and what to do
	// when the report comes back.
	//
	// With Diagnostics == false:
	//   * TrackVictim() is never called and the watch list stays empty;
	//   * "Hit:" / "Hit-final:" / "Probe:" are discarded by if constexpr,
	//     string literals included - they are not in the DLL at all;
	//   * End_Piggyback prints "tracked=off" instead of "tracked=0/1";
	//   * the extra fields on Begin:/Post: keep printing - one line per
	//     application, no measurable cost, and always valid readings.
	constexpr bool Diagnostics = false;

	constexpr int TrackedVictimCount = 8;
	constexpr int TrackedVictimFrames = 900;
	constexpr int TrackedVictimProbeInterval = 30;

	// Starts (or refreshes) the watch window for a victim. Called when the
	// effect is applied and again when it ends, so the window always covers the
	// interesting "after" part. Only called when Diagnostics is true.
	void TrackVictim(const AbstractClass* pVictim);
	// True while pVictim is inside the watch window. When pProbeDue is given it
	// is set to true at most once every TrackedVictimProbeInterval frames.
	bool IsTrackedVictim(const AbstractClass* pVictim, bool* pProbeDue = nullptr);
}

class __declspec(uuid("{55680C96-8DE1-4E6D-8756-FCCB1720B3E1}"))
	LocomotorWeaponLocomotionClass : public LocomotionClass, public IPiggyback
{
public:
	// IUnknown
	virtual HRESULT __stdcall QueryInterface(REFIID iid, LPVOID* ppvObject) override;
	virtual ULONG __stdcall AddRef() override { return LocomotionClass::AddRef(); }
	virtual ULONG __stdcall Release() override { return LocomotionClass::Release(); }

	// IPersist
	virtual HRESULT __stdcall GetClassID(CLSID* pClassID) override;

	// IPersistStream
	virtual HRESULT __stdcall Load(IStream* pStm) override;
	virtual HRESULT __stdcall Save(IStream* pStm, BOOL fClearDirty) override;

	// ILocomotion
	virtual HRESULT __stdcall Link_To_Object(void* pointer) override;
	virtual bool __stdcall Is_Moving() override;
	virtual CoordStruct __stdcall Destination() override;
	virtual CoordStruct __stdcall Head_To_Coord() override;
	virtual Move __stdcall Can_Enter_Cell(CellStruct cell) override;
	virtual bool __stdcall Is_To_Have_Shadow() override;
	virtual Matrix3D __stdcall Draw_Matrix(VoxelIndexKey* pIndex) override;
	virtual Matrix3D __stdcall Shadow_Matrix(VoxelIndexKey* pIndex) override;
	virtual Point2D __stdcall Draw_Point() override;
	virtual Point2D __stdcall Shadow_Point() override;
	virtual VisualType __stdcall Visual_Character(bool raw) override;
	virtual int __stdcall Z_Adjust() override;
	virtual ZGradient __stdcall Z_Gradient() override;
	virtual bool __stdcall Process() override;
	virtual void __stdcall Move_To(CoordStruct to) override;
	virtual void __stdcall Stop_Moving() override;
	virtual void __stdcall Do_Turn(DirStruct coord) override;
	virtual void __stdcall Unlimbo() override;
	virtual void __stdcall Tilt_Pitch_AI() override;
	virtual bool __stdcall Power_On() override;
	virtual bool __stdcall Power_Off() override;
	virtual bool __stdcall Is_Powered() override;
	virtual bool __stdcall Is_Ion_Sensitive() override;
	virtual bool __stdcall Push(DirStruct dir) override;
	virtual bool __stdcall Shove(DirStruct dir) override;
	virtual void __stdcall Force_Track(int track, CoordStruct coord) override;
	virtual Layer __stdcall In_Which_Layer() override;
	virtual void __stdcall Force_Immediate_Destination(CoordStruct coord) override;
	virtual void __stdcall Force_New_Slope(int ramp) override;
	virtual bool __stdcall Is_Moving_Now() override;
	virtual int __stdcall Apparent_Speed() override;
	virtual int __stdcall Drawing_Code() override;
	virtual FireError __stdcall Can_Fire() override;
	virtual int __stdcall Get_Status() override;
	virtual void __stdcall Acquire_Hunter_Seeker_Target() override;
	virtual bool __stdcall Is_Surfacing() override;
	virtual void __stdcall Mark_All_Occupation_Bits(MarkType mark) override;
	virtual bool __stdcall Is_Moving_Here(CoordStruct to) override;
	virtual bool __stdcall Will_Jump_Tracks() override;
	virtual bool __stdcall Is_Really_Moving_Now() override;
	virtual void __stdcall Stop_Movement_Animation() override;
	virtual void __stdcall Limbo() override;
	virtual void __stdcall Lock() override;
	virtual void __stdcall Unlock() override;
	virtual int __stdcall Get_Track_Number() override;
	virtual int __stdcall Get_Track_Index() override;
	virtual int __stdcall Get_Speed_Accum() override;

	// IPiggyback
	virtual HRESULT __stdcall Begin_Piggyback(ILocomotion* pointer) override;
	virtual HRESULT __stdcall End_Piggyback(ILocomotion** pointer) override;
	virtual bool __stdcall Is_Ok_To_End() override;
	virtual HRESULT __stdcall Piggyback_CLSID(GUID* classid) override;
	virtual bool __stdcall Is_Piggybacking() override;

public:
	LocomotorWeaponLocomotionClass()
		: LocomotionClass { }
		, RequestedCLSID { }
		, Config { }
		, Timer { }
		, DestinationCached { CoordStruct::Empty }
		, LastIssuedMoverDestination { CoordStruct::Empty }
		, HasDestination { false }
		, HasIssuedMoverDestination { false }
	{ }

	explicit LocomotorWeaponLocomotionClass(noinit_t)
		: LocomotionClass { noinit_t() }
	{ }

	virtual ~LocomotorWeaponLocomotionClass() override = default;
	virtual int Size() override { return sizeof(*this); }

public:
	// The locomotor that was active before this one got applied; handed back
	// verbatim from End_Piggyback.
	ILocomotionPtr Piggybacker;

	// The vanilla locomotor that actually moves the victim in Mover mode.
	// Never used for anything that requires IPiggyback (our class owns that).
	ILocomotionPtr Mover;

	// The CLSID the warhead originally asked for. This is internal state (not
	// an INI key): it drives Mover creation and is needed to rebuild the Mover
	// after loading a savegame.
	CLSID RequestedCLSID;

	LocomotorWeaponConfig Config;

	// Duration countdown for LocomotorWeapon.Duration (in frames). A timer that
	// was never started reports -1 / not ticking.
	CDTimerClass Timer;

	CoordStruct DestinationCached;
	CoordStruct LastIssuedMoverDestination;
	bool HasDestination;
	bool HasIssuedMoverDestination;

	// State of the movement this class performs itself (air / meteor).
	LocoWeaponPhase Phase { LocoWeaponPhase::Idle };
	int GroundZ { 0 };
	int FlightZ { 0 };
	int FallDistance { 0 };
	bool Arrived { false };
	bool ImpactApplied { false };
	bool WarpDone { false };

	// Diagnostics only. The base class' raw object dump carries them across a
	// savegame, so no explicit serialization is needed.
	int AppliedFrame { 0 };
	int LastStuckLogFrame { -100000 };
	bool DescentLogged { false };

	// Latches once the engine has finished arming this locomotor
	// (TechnoClass::ImbueLocomotor sets IsAttackedByLocomotor as its last step).
	// Before that, "IsAttackedByLocomotor == 0" must not be read as a release -
	// see the comment in WantsToEnd().
	bool Armed { false };

	// Diagnostic: which condition made WantsToEnd() return true. Only used by the
	// "End:" log line.
	int EndReason { 0 };

public:
	// The locomotor that should actually move the victim, or null.
	ILocomotion* GetMover();
	// The locomotor that should answer queries (drawing, layer, ...).
	ILocomotion* GetQuerySource();

	// Applies Mode=auto, creates the Mover for Mover mode.
	void InitializeFromConfig();
	void CreateMover();
	void ResolveMode();
	// Stops and drops the Mover.
	void DetachMover();
	// Keeps a Mover-based victim chasing the firer.
	void UpdateMoverDestination();

	// Modes that move the victim without a Mover.
	bool IsOwnMovementMode() const;
	bool RequiresLanding() const;
	bool ProcessOwnMovement();
	bool ProcessFlight();
	bool IsOnGround();
	// True when every release/timeout/arrival condition for ending is met; the
	// caller still has to check the landing gate (Is_Ok_To_End does both).
	bool WantsToEnd();
	// True when the victim must be back on the ground before the effect may end
	// (our own lifting modes, and any victim the engine flagged as crashing).
	bool MustLand();
	// Controlled descent used when the effect is over but the victim is still
	// off the ground; returns true while it is still coming down.
	bool ProcessLanding();
	// Undoes the crash/manipulation state TechnoClass::ReleaseLocomotor leaves
	// behind on a victim that was released above the ground.
	static void ClearCrashState(FootClass* pTarget);
	CoordStruct GetGoalCoords();
	// Moves the victim in a way the engine notices (occupation bits included).
	void ApplyLocation(const CoordStruct& coord);
	void WarpToFirer();
	void ApplyImpact();
	// Mirrors the vanilla "released harvester goes back to work" branch.
	static void RestoreHarvestMission(FootClass* pTarget);
	// Resolves the type arrays stored as indices in the config.
	WarheadTypeClass* GetWarhead() const;
	AnimTypeClass* GetAnim() const;
	AnimTypeClass* GetMeteorAnim() const;
};

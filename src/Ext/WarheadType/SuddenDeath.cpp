#include "SuddenDeath.h"

#include <Ext/Techno/Body.h>
#include <Ext/TerrainType/Body.h>
#include <Ext/WarheadType/Body.h>
#include <Utilities/Debug.h>

#include <Helpers/Iterators.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace
{
	// Every [SuddenDeath] line goes through here, so the whole trace is one switch
	// (SuddenDeath::Diagnostics, off by default).
	template <typename... Args>
	void Log(const char* pFormat, Args... args)
	{
		if constexpr (SuddenDeath::Diagnostics)
			Debug::Log(pFormat, args...);
	}
	struct TechnoRecord
	{
		TechnoClass* Target;
		TechnoClass* Attacker;
		HouseClass* AttackingHouse;
		SuddenDeathMode Mode;
	};

	struct TerrainRecord
	{
		CoordStruct Coords;
		double CellSpread;
	};

	// Raw pointers, but they never outlive the logic frame they were recorded in: ExecutePending
	// runs at the head of every one of them, and loading a scenario drops whatever is left.
	std::vector<TechnoRecord> TechnoQueue;
	std::vector<TerrainRecord> TerrainQueue;

	// Terrain this feature has already taken out of the world. Removal does not unlink a terrain
	// from its cell, so without this a second detonation over the same cell - the same weapon
	// fired twice at the same tree - would find the same object again and hand it to the removal
	// a second time. Entries are dropped only when a scenario is loaded: an address that a later
	// terrain happens to reuse costs that one terrain a SuddenDeath hit, which is the safe
	// direction to err in.
	std::vector<TerrainClass*> RemovedTerrain;
	constexpr size_t RemovedTerrainLimit = 256;

	// Bridge cells this feature has already collapsed, for the same reason: two detonations over
	// the same cell in one frame must not collapse the same bridge twice.
	std::vector<CellClass*> BlownBridges;

	// ReduceTiberium's argument is a number of tiberium density steps (the cell keeps its density
	// in a byte capped at 11 - the engine compares against 0xB), and the engine only removes the
	// overlay outright when the argument reaches density + 1. Anything comfortably above the cap
	// therefore means "take all of it", which is what the ore half of this feature promises.
	// Cells without tiberium return early inside ReduceTiberium itself.
	constexpr int TiberiumClearance = 100;

	// Cursor layer: whose cursor is being computed right now (see SuddenDeath.h).
	TechnoClass* CursorTechno = nullptr;


	bool HoldsExecutionerWeapon(TechnoClass* pTechno)
	{
		if (!pTechno || !pTechno->IsAlive)
			return false;

		auto const pType = pTechno->GetTechnoType();

		if (!pType)
			return false;

		// Read the type's weapon slots directly rather than through the engine's GetWeapon: this
		// runs on every cursor update, and a lookup that can return something unexpected is not
		// worth the elegance. Slots 0 and 1 are always present.
		const bool elite = pTechno->Veterancy.IsElite();

		for (size_t i = 0; i < 2; ++i)
		{
			auto const pWeapon = pType->GetWeapon(i, elite).WeaponType;

			if (pWeapon && pWeapon->Warhead && WarheadTypeExt::Fetch(pWeapon->Warhead)->SuddenDeath)
				return true;
		}

		return false;
	}
}

void SuddenDeath::RegisterTechno(TechnoClass* pTarget, TechnoClass* pAttacker, HouseClass* pAttackingHouse, SuddenDeathMode mode)
{
	if (!pTarget)
		return;

	const auto it = std::find_if(TechnoQueue.begin(), TechnoQueue.end(),
		[pTarget](const TechnoRecord& record) { return record.Target == pTarget; });

	if (it == TechnoQueue.end())
	{
		TechnoQueue.push_back({ pTarget, pAttacker, pAttackingHouse, mode });

		Log("[SuddenDeath] queue target=%s(%d) hp=%d mode=%d atk=%s\n",
			pTarget->GetTechnoType() && pTarget->GetTechnoType()->ID ? pTarget->GetTechnoType()->ID : "?",
			static_cast<int>(pTarget->WhatAmI()), pTarget->Health, static_cast<int>(mode),
			pAttacker && pAttacker->GetTechnoType() && pAttacker->GetTechnoType()->ID ? pAttacker->GetTechnoType()->ID : "<none>");
	}
}

void SuddenDeath::RegisterTerrain(const CoordStruct& coords, double cellSpread)
{
	TerrainQueue.push_back({ coords, cellSpread });
}

void SuddenDeath::ClearPending()
{
	TechnoQueue.clear();
	TerrainQueue.clear();
	RemovedTerrain.clear();
	BlownBridges.clear();
	CursorTechno = nullptr;
}

void SuddenDeath::NoteCursorTechno(TechnoClass* pTechno)
{
	CursorTechno = pTechno;
}

bool SuddenDeath::CursorHoldsExecutionerWeapon()
{
	return HoldsExecutionerWeapon(CursorTechno);
}

void SuddenDeath::ExecutePending()
{
	if (!TechnoQueue.empty())
	{
		const auto queue = std::move(TechnoQueue);
		TechnoQueue.clear();

		for (const auto& record : queue)
		{
			auto const pTarget = record.Target;

			// A frame of damage has passed since the hit was recorded, and the warhead's own
			// damage was settled in it - the target may already be dead, in limbo or gone.
			if (!pTarget || !pTarget->IsAlive || pTarget->InLimbo || pTarget->Health <= 0)
			{
				Log("[SuddenDeath] execute skip %p (dead, in limbo or gone)\n", pTarget);
				continue;
			}

			if (record.Mode == SuddenDeathMode::Erase)
			{
				// Phobos' own vanish: no wreck, no DeathWeapon, no revenge weapon, nothing that
				// hangs off the ordinary death path. And no animation either - erase is meant to
				// leave nothing behind at all.
				TechnoExt::KillSelf(pTarget, AutoDeathBehavior::Vanish, {});
				continue;
			}

			auto const pC4Warhead = RulesClass::Instance->C4Warhead;

			if (!pC4Warhead)
				continue;

			// The ordinary exit, in the shape the engine already knows: a warhead that carries no
			// behaviour of its own (the one the engine uses itself when it has no weapon to settle
			// damage with), through the ordinary ReceiveDamage entry. IgnoreDefenses stays on - a
			// SuddenDeath hit is supposed to be past armour, immunity, Iron Curtain and shields -
			// and the attacker is passed along so the kill is still credited to whoever fired.
			pTarget->ReceiveDamage(&pTarget->Health, 0, pC4Warhead, record.Attacker, true, false, record.AttackingHouse);

			Log("[SuddenDeath] execute target=%s(%d) hp=%d\n",
				pTarget->GetTechnoType() && pTarget->GetTechnoType()->ID ? pTarget->GetTechnoType()->ID : "?",
				static_cast<int>(pTarget->WhatAmI()), pTarget->Health);
		}
	}

	if (!TerrainQueue.empty())
	{
		const auto queue = std::move(TerrainQueue);
		TerrainQueue.clear();

		for (const auto& record : queue)
		{
			CellRangeIterator<CellClass>{}(CellClass::Coord2Cell(record.Coords), record.CellSpread + 0.5, [](CellClass* const pCell)
			{
				// Ore. Vanilla clears tiberium through the warhead's Tiberium flag, inside
				// MapClass::DamageArea (0x48962A-0x489665, ReduceTiberium(damage/10)); an
				// executioner warhead does not need that flag or the damage behind it, it takes
				// the whole cell. ReduceTiberium returns on its own for a cell with no tiberium.
				if (pCell->GetContainedTiberiumIndex() >= 0)
				{
					pCell->ReduceTiberium(TiberiumClearance);

					Log("[SuddenDeath] execute ore cell=[%d,%d]\n",
						pCell->MapCoords.X, pCell->MapCoords.Y);
				}

				// Bridges. Vanilla destroys a bridge from MapClass::DamageArea, and only from the
				// branch that a warhead with Wall=yes reaches (0x489FC2); that branch then rolls
				// RandomRanged(1, RulesClass::BridgeStrength) against the damage and gives up if
				// the roll wins (0x489FE0-0x489FFE). There is exactly one way past the roll: the
				// warhead being [CombatDamage] IonCannonWarhead (0x489FD8 -> 0x48A004). Rather
				// than imitating the collapse - this file's own attempt (BlowUpBridge alone) only
				// produced the aftermath and left the span standing - the engine is asked to do it
				// with that very warhead, at damage 1 so nothing else in the cell is hurt:
				// WallAbsoluteDestroyer/wall overlays see 1 point, units see 1 point, and the
				// bridge branch sees the warhead it never rolls against.
				if ((pCell->ContainsBridge() || pCell->ContainsBridgeEx())
					&& std::find(BlownBridges.begin(), BlownBridges.end(), pCell) == BlownBridges.end())
				{
					if (auto const pIonWarhead = RulesClass::Instance->IonCannonWarhead)
					{
						// The explosion's Z decides whether the engine even considers the bridge:
						// the branch at 0x489FC2 is reached only when
						//   (Level - 2) * LevelHeight + base < Z <= (Level + 1) * LevelHeight + base
						// (0x489FA0-0x489FBC; measured at run time base = [0x89E864] = 416 and
						// LevelHeight = [0x89E870] = 104, so a Level-0 bridge cell wants Z inside
						// (208, 520]). Two earlier attempts passed Z values outside that window -
						// Cell2Coord(MapCoords) has Z = 0 and GetCellCoords() alone returns the
						// ground-relative height (also 0 on flat ground) - and were silently skipped
						// right there. Adding the bridge height is YRpp's own CellClass::FixHeight,
						// and lands the point on the deck.
						CoordStruct bridgeCoords = pCell->GetCellCoords();

						if (pCell->ContainsBridge())
							bridgeCoords.Z += CellClass::BridgeHeight;

						MapClass::DamageArea(bridgeCoords, 1, nullptr,
							pIonWarhead, false, nullptr);
					}

					// The aftermath the engine runs when a bridge's flags flip: the units standing
					// on the cell are killed and the craters are placed (BlowUpBridge). Calling it
					// here as well is what keeps this feature independent of when the map's own
					// cell update gets around to it.
					pCell->BlowUpBridge();
					BlownBridges.push_back(pCell);

					Log("[SuddenDeath] execute bridge cell=[%d,%d]\n",
						pCell->MapCoords.X, pCell->MapCoords.Y);
				}

				// Both content chains: GetTerrain walks the cell's object list looking for a
				// TerrainClass, and the alternate one holds what a bridge cell keeps aside.
				for (const bool alt : { false, true })
				{
					auto const pTerrain = pCell->GetTerrain(alt);

					// Health is deliberately not part of this test: a TerrainType that does not
					// write Strength= enters the world with Health 0 (the constructor copies
					// Type->Strength straight into it), and the tiberium pillars of this mod are
					// exactly that kind of terrain - testing Health here would skip the very
					// objects a SuddenDeath warhead is most wanted against. What is tested instead
					// is "still in the world" and "not already on its way out".
					if (!pTerrain || pTerrain->InLimbo || pTerrain->IsBurning || pTerrain->IsCrumbling)
						continue;

					if (std::find(RemovedTerrain.begin(), RemovedTerrain.end(), pTerrain) != RemovedTerrain.end())
						continue;

					auto const pType = pTerrain->Type;

					if (!pType)
						continue;

					Log("[SuddenDeath] execute terrain=%s cell=[%d,%d]\n",
						pType->ID ? pType->ID : "?", pCell->MapCoords.X, pCell->MapCoords.Y);

					// The removal Phobos uses once its crumbling frames are done: destroy effects,
					// redraw the area, Disappear, UnInit. Terrain has no death path to borrow - the
					// damage pipeline never reaches a TerrainClass - so this is the engine's own way
					// out of the world, minus the burning that would normally lead up to it.
					TerrainTypeExt::Fetch(pType)->PlayDestroyEffects(pTerrain->GetCoords());
					TerrainTypeExt::Remove(pTerrain);

					if (RemovedTerrain.size() >= RemovedTerrainLimit)
						RemovedTerrain.erase(RemovedTerrain.begin());

					RemovedTerrain.push_back(pTerrain);
				}

				return true;
			});
		}
	}
}

// Cursor layer
//
// The engine only offers an attack cursor on something it believes it can hurt: in each of the
// three per-class cursor functions, an `Attack` action gets downgraded to 2 when the target's type
// is Immune (0x417F63 / 0x51F179 / 0x74049A - `mov <action>,0x2`). A SuddenDeath warhead kills what
// it reaches, immunity included, so while one is in hand that downgrade is simply wrong - without
// this the tiberium pillars of this mod (Immune=yes) cannot be aimed at at all, even with Ctrl.
//
// All three cursor functions compute the action they offer through the same helper, 0x4DDED0
// (the whole engine calls it from those three places and nowhere else), so its entry is the one
// point where the unit whose cursor is being computed is known. The stolen bytes there are
// `sub esp,0x18` + `mov eax,[esp+0x20]` (3 + 4 bytes, both whole instructions).
DEFINE_HOOK(0x4DDED0, TechnoClass_GetCursorOverObject_SuddenDeathContext, 0x7)
{
	GET(TechnoClass*, pThis, ECX);
	SuddenDeath::NoteCursorTechno(pThis);
	return 0;
}

namespace
{
	// Shared body of the three downgrade hooks; the site name only exists for the log.
	bool BypassImmuneDowngrade(const char* pSite)
	{
		if (!SuddenDeath::CursorHoldsExecutionerWeapon())
			return false;

		Log("[SuddenDeath] cursor immune bypass at %s (unit=%s)\n", pSite,
			CursorTechno && CursorTechno->GetTechnoType() && CursorTechno->GetTechnoType()->ID
				? CursorTechno->GetTechnoType()->ID : "?");

		return true;
	}
}

DEFINE_HOOK(0x417F63, AircraftClass_GetCursorOverObject_SuddenDeathImmune, 0x5)
{
	enum { SkipDowngrade = 0x417F68 };
	return BypassImmuneDowngrade("aircraft") ? SkipDowngrade : 0;
}

DEFINE_HOOK(0x51F179, InfantryClass_GetCursorOverObject_SuddenDeathImmune, 0x5)
{
	enum { SkipDowngrade = 0x51F17E };
	return BypassImmuneDowngrade("infantry") ? SkipDowngrade : 0;
}

DEFINE_HOOK(0x74049A, UnitClass_GetCursorOverObject_SuddenDeathImmune, 0x5)
{
	enum { SkipDowngrade = 0x74049F };
	return BypassImmuneDowngrade("unit") ? SkipDowngrade : 0;
}

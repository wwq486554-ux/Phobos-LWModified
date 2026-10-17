// ExtraWeapon: additional independent weapons.
//
// Each mount of an ExtraWeapon list fires its own weapon at the techno's current target,
// paced by that weapon's own ROF/burst/range. The extra weapons do not acquire targets and
// do not touch the engine's weapon state (no ammo use, no rearm timer, no Fire_At): they are
// a second, fully independent firing layer. Which mount list is active is decided per frame
// by the weapon slot the engine currently has selected for the techno's target, which is
// where the AA/secondary/gattling/IFV firing context comes from.

#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/WeaponType/Body.h>
#include <Ext/Bullet/Body.h>

#include <BuildingClass.h>
#include <HouseClass.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <WeaponTypeClass.h>

#include <Utilities/Debug.h>

#include <algorithm>

namespace
{
	// Fallback burst delay: the same 3..5 frame random the engine uses when a weapon has no
	// usable burst delay. An extra weapon has no engine weapon slot, so the vanilla art
	// BurstDelayN keys cannot apply to it; WeaponTypeExt::GetBurstDelay is the only source.
	constexpr int DefaultBurstDelayMin = 3;
	constexpr int DefaultBurstDelayMax = 5;

	// Shared line budget for the "this weapon ID did not resolve to a usable weapon" warning:
	// a typo on a type that is deployed hundreds of times must not flood debug.log.
	int InvalidWeaponWarningsLeft = 8;

	// Frames between two shots of a burst. The first shot is immediate (index 0); every later
	// shot waits at least one frame, even when the configured delay is 0.
	int GetBurstDelayForShot(WeaponTypeClass* pWeapon, int burstIndex)
	{
		if (burstIndex <= 0)
			return 0;

		int delay = WeaponTypeExt::Fetch(pWeapon)->GetBurstDelay(burstIndex);

		if (delay < 0)
			delay = ScenarioClass::Instance->Random.RandomRanged(DefaultBurstDelayMin, DefaultBurstDelayMax);

		return std::max(delay, 1);
	}

	CoordStruct GetMountFLH(const std::vector<CoordStruct>& FLHs, size_t mountIndex)
	{
		return mountIndex < FLHs.size() ? FLHs[mountIndex] : CoordStruct::Empty;
	}

	// Vanilla Fire_At special-cases these weapon kinds in ways a self-built bullet cannot
	// reproduce. Configuring one of them as an extra weapon is skipped safely (one warning)
	// instead of producing a wrong or broken shot.
	bool IsUnsupportedExtraWeapon(WeaponTypeClass* pWeapon)
	{
		return pWeapon->Spawner || pWeapon->DiskLaser || pWeapon->IsSonic
			|| pWeapon->LimboLaunch || pWeapon->Suicide || pWeapon->IsMagBeam;
	}
}

bool ExtraWeaponRuntime::Load(PhobosStreamReader& Stm, bool RegisterForChange)
{
	return this->Serialize(Stm);
}

bool ExtraWeaponRuntime::Save(PhobosStreamWriter& Stm) const
{
	return const_cast<ExtraWeaponRuntime*>(this)->Serialize(Stm);
}

template <typename T>
bool ExtraWeaponRuntime::Serialize(T& Stm)
{
	return Stm
		.Process(this->Weapon)
		.Process(this->Cooldown)
		.Process(this->BurstTimer)
		.Process(this->BurstShotsLeft)
		.Process(this->NextShotIndex)
		.Success();
}

bool TechnoExt::CanExtraWeaponsFire() const
{
	const auto pThis = this->OwnerObject();

	if (!pThis->IsAlive || pThis->InLimbo || pThis->IsSinking || pThis->Health <= 0)
		return false;

	// Unit-level gates. A failed gate only pauses the extra weapons: their cooldown and
	// burst state are left alone, exactly like a vanilla weapon that cannot fire right now.
	if (pThis->Deactivated || pThis->IsUnderEMP())
		return false;

	// Attach-effect DisableWeapons. Ares' own disable timer is intentionally not consulted:
	// it belongs to the engine weapon slots, and the extra weapons are independent of them.
	if (this->AE.DisableWeapons)
		return false;

	// Fixed v1 limitation: passengers of an open-topped transport do not use extra weapons.
	if (pThis->InOpenToppedTransport)
		return false;

	if (pThis->WhatAmI() == AbstractType::Building)
	{
		const auto mission = pThis->GetCurrentMission();

		if (mission == Mission::Selling || mission == Mission::Construction)
			return false;
	}

	return true;
}

bool TechnoExt::IsInExtraWeaponRange(WeaponTypeClass* pWeapon, AbstractClass* pTarget) const
{
	const auto pThis = this->OwnerObject();
	const int range = WeaponTypeExt::GetRangeWithModifiers(pWeapon, pThis);

	// The engine's "infinite range" marker: such a weapon never fails the range test.
	if (range == -512)
		return true;

	const bool targetInAir = pTarget->IsInAir();
	int effectiveRange = range;

	if (targetInAir)
		effectiveRange += pThis->GetTechnoType()->AirRangeBonus;

	if (effectiveRange < 0)
		return false;

	// Distance basis of the engine's own range check: horizontal distance whenever either
	// side is airborne (the AirRangeBonus above compensates for the altitude), full 3D
	// distance for a ground-to-ground attack. ObjectClass::DistanceFrom* subtract a building
	// target's foundation size exactly like the engine does, so no extra correction here.
	const bool airInvolved = targetInAir || pThis->IsInAir();
	const int distance = airInvolved ? pThis->DistanceFrom(pTarget) : pThis->DistanceFrom3D(pTarget);

	if (distance > effectiveRange)
		return false;

	// MinimumRange is checked against the true 3D distance, as the engine does.
	if (pWeapon->MinimumRange > 0 && pThis->DistanceFrom3D(pTarget) < pWeapon->MinimumRange)
		return false;

	return true;
}

int TechnoExt::GetExtraWeaponROF(WeaponTypeClass* pWeapon) const
{
	const auto pThis = this->OwnerObject();
	double ROF = pWeapon->ROF;

	if (pThis->HasAbility(Ability::ROF))
		ROF *= RulesClass::Instance->VeteranROF;

	if (const auto pOwner = pThis->Owner)
		ROF *= pOwner->Type->ROFMult;

	// The same attach-effect multiplier the engine path applies to its own reload.
	ROF *= this->AE.ROFMultiplier;

	// Deliberately no vanilla 0..2 frame random here: the extra weapons stay predictable.
	return std::max(1, static_cast<int>(ROF));
}

void TechnoExt::SyncExtraWeaponRuntimes(const std::vector<WeaponTypeClass*>& mounts, std::vector<ExtraWeaponRuntime>& runtimes)
{
	if (runtimes.size() != mounts.size())
		runtimes.resize(mounts.size());

	for (size_t i = 0; i < mounts.size(); ++i)
	{
		auto& runtime = runtimes[i];

		if (runtime.Weapon == mounts[i])
			continue;

		// The mount is a different weapon now (elite upgrade, type conversion, or a
		// different list became active): start it from scratch.
		runtime.Weapon = mounts[i];
		runtime.Cooldown.Stop();
		runtime.BurstTimer.Stop();
		runtime.BurstShotsLeft = 0;
		runtime.NextShotIndex = 0;
		runtime.SkippedLogged = false;
	}
}

bool TechnoExt::FireExtraWeaponShot(WeaponTypeClass* pWeapon, AbstractClass* pTarget, const CoordStruct& flh, int burstIndex, int burstCount)
{
	const auto pThis = this->OwnerObject();
	const auto pOwner = pThis->Owner;

	if (!pWeapon->Projectile)
		return false;

	// The mount point follows the techno's turret exactly like a vanilla weapon does:
	// HasTurret() picks the turret facing for Turret=yes units and the body facing for the
	// rest, and BodyWeapon's turret lock is already reflected in the turret facing.
	const CoordStruct muzzle = TechnoExt::GetFLHAbsoluteCoords(pThis, flh, pThis->HasTurret());
	const double firepower = TechnoExt::GetCurrentFirepowerMultiplier(pThis);
	const int damage = static_cast<int>(pWeapon->Damage * firepower);
	const auto pBullet = pWeapon->Projectile->CreateBullet(pTarget, pThis, damage, pWeapon->Warhead, pWeapon->Speed, pWeapon->Bright);

	if (!pBullet)
		return false;

	const auto pBulletExt = BulletExt::Fetch(pBullet);

	pBulletExt->FirepowerMult = firepower;
	pBulletExt->NotMainWeapon = false;
	pBulletExt->FLHCoord = flh;

	// Suppress the trajectory's own OpenFire until the launch is set up, then let it start
	// from this exact muzzle - the same order the disperse-weapon path uses.
	pBulletExt->DispersedTrajectory = true;
	BulletExt::SimulatedFiringUnlimbo(pBullet, pOwner, pWeapon, muzzle, true);
	pBulletExt->DispersedTrajectory = false;

	if (const auto pTrajectory = pBulletExt->Trajectory.get())
	{
		pTrajectory->CurrentBurst = burstIndex;
		pTrajectory->CountOfBurst = burstCount;
		pTrajectory->OpenFire();
	}

	// Every shot is a real shot: muzzle animation, report sound, laser/bolt/beam particles.
	BulletExt::SimulatedFiringEffects(pBullet, pOwner, nullptr, true, true);

	return true;
}

void TechnoExt::UpdateExtraWeapons()
{
	const auto pTypeExt = this->TypeExtData;

	if (!pTypeExt || !pTypeExt->HasExtraWeapons())
		return;

	const auto pThis = this->OwnerObject();

	// Target context. No target (or a target that is already gone) only pauses the extra
	// weapons - the burst/cooldown state is kept, as vanilla burst behaviour does.
	const auto pTarget = pThis->Target;

	if (!pTarget)
		return;

	if (const auto pTargetTechno = abstract_cast<TechnoClass*>(pTarget))
	{
		if (BulletExt::CheckTechnoIsInvalid(pTargetTechno))
			return;
	}

	// The engine's currently selected weapon slot is the firing context: a slot with its
	// own group list selects that group, every other slot falls back to the suffix-less list.
	const int slot = pThis->SelectWeapon(pTarget);

	if (slot < 0 || static_cast<size_t>(slot) >= TechnoTypeExt::ExtraWeapon_MaxMounts)
		return;

	const bool elite = pThis->Veterancy.IsElite();
	const std::vector<WeaponTypeClass*>* pMounts = nullptr;
	const std::vector<CoordStruct>* pFLHs = nullptr;
	std::vector<ExtraWeaponRuntime>* pRuntimes = nullptr;

	if (pTypeExt->HasExtraWeaponGroupForSlot(slot, elite))
	{
		pMounts = &pTypeExt->GetExtraWeaponGroup(slot, elite);
		pFLHs = &pTypeExt->GetExtraWeaponGroupFLHs(slot);

		if (this->ExtraWeapon_GroupRuntimes.size() < TechnoTypeExt::ExtraWeapon_MaxMounts)
			this->ExtraWeapon_GroupRuntimes.resize(TechnoTypeExt::ExtraWeapon_MaxMounts);

		pRuntimes = &this->ExtraWeapon_GroupRuntimes[slot];
	}
	else
	{
		pMounts = &pTypeExt->GetFlatExtraWeaponList(elite);
		pFLHs = &pTypeExt->ExtraWeapon_FLHs;

		if (this->ExtraWeapon_FlatRuntimes.empty())
			this->ExtraWeapon_FlatRuntimes.resize(1);

		pRuntimes = &this->ExtraWeapon_FlatRuntimes[0];
	}

	if (pMounts->empty())
		return;

	this->SyncExtraWeaponRuntimes(*pMounts, *pRuntimes);

	if (!this->CanExtraWeaponsFire())
		return;

	for (size_t mountIndex = 0; mountIndex < pMounts->size(); ++mountIndex)
	{
		const auto pWeapon = (*pMounts)[mountIndex];
		auto& runtime = (*pRuntimes)[mountIndex];

		if (!pWeapon || !pWeapon->Projectile || IsUnsupportedExtraWeapon(pWeapon))
		{
			// An unresolved WeaponTypes ID still allocates an empty shell, and the
			// engine-special weapon kinds are outside this feature's scope. Skip the mount
			// (the other mounts keep firing) and warn once, not once per frame.
			if (!runtime.SkippedLogged)
			{
				runtime.SkippedLogged = true;

				if (InvalidWeaponWarningsLeft > 0)
				{
					--InvalidWeaponWarningsLeft;
					Debug::Log("[ExtraWeapon] [%s] mount %d: weapon '%s' is not usable here (missing Projectile or an engine-special weapon kind), skipping it\n",
						pThis->GetTechnoType()->ID, static_cast<int>(mountIndex) + 1, pWeapon ? pWeapon->ID : "<none>");
				}
			}

			continue;
		}

		// Out of this weapon's own range: wait. The cooldown/burst state is kept, so the
		// mount fires the moment the target comes into range instead of restarting.
		if (!this->IsInExtraWeaponRange(pWeapon, pTarget))
			continue;

		// Cloak handling follows the weapon's own DecloakToFire: declare first (the engine
		// does the same before CanFire), fire on the next frame while uncloaked.
		if (pThis->CloakState == CloakState::Cloaked && pWeapon->DecloakToFire)
		{
			pThis->Uncloak(true);
			continue;
		}

		const auto pWeaponExt = WeaponTypeExt::Fetch(pWeapon);
		const int burstCount = std::max(pWeapon->Burst, 1);

		if (runtime.BurstShotsLeft > 0)
		{
			// Burst in progress: continue where it stopped, no reset.
			if (runtime.BurstTimer.InProgress())
				continue;

			CoordStruct flh = GetMountFLH(*pFLHs, mountIndex);

			if (runtime.NextShotIndex % 2 == 1)
				flh.Y = -flh.Y;

			this->FireExtraWeaponShot(pWeapon, pTarget, flh, runtime.NextShotIndex, burstCount);

			if (--runtime.BurstShotsLeft <= 0)
			{
				runtime.Cooldown.Start(this->GetExtraWeaponROF(pWeapon));
				runtime.NextShotIndex = 0;
			}
			else
			{
				++runtime.NextShotIndex;
				runtime.BurstTimer.Start(GetBurstDelayForShot(pWeapon, runtime.NextShotIndex));
			}
		}
		else if (runtime.Cooldown.Expired())
		{
			// A new round. Burst.NoDelay puts the whole burst into this frame.
			runtime.NextShotIndex = 0;

			if (pWeaponExt->Burst_NoDelay && burstCount > 1)
			{
				for (int shot = 0; shot < burstCount; ++shot)
				{
					CoordStruct flh = GetMountFLH(*pFLHs, mountIndex);

					if (shot % 2 == 1)
						flh.Y = -flh.Y;

					this->FireExtraWeaponShot(pWeapon, pTarget, flh, shot, burstCount);
				}

				runtime.Cooldown.Start(this->GetExtraWeaponROF(pWeapon));
			}
			else
			{
				CoordStruct flh = GetMountFLH(*pFLHs, mountIndex);
				this->FireExtraWeaponShot(pWeapon, pTarget, flh, 0, burstCount);
				runtime.BurstShotsLeft = burstCount - 1;

				if (runtime.BurstShotsLeft <= 0)
				{
					runtime.Cooldown.Start(this->GetExtraWeaponROF(pWeapon));
				}
				else
				{
					runtime.NextShotIndex = 1;
					runtime.BurstTimer.Start(GetBurstDelayForShot(pWeapon, 1));
				}
			}
		}
	}
}

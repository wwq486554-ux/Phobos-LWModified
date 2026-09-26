#include "EngraveTrajectory.h"

#include <Ext/Bullet/Trajectories/EngraveLine.h>

#include <AnimClass.h>
#include <BuildingClass.h>
#include <ScenarioClass.h>

#include <Ext/Anim/Body.h>
#include <Ext/WeaponType/Body.h>
#include <Ext/Bullet/Body.h>
#include <Ext/Techno/Body.h>

std::unique_ptr<PhobosTrajectory> EngraveTrajectoryType::CreateInstance(BulletClass* pBullet) const
{
	return std::make_unique<EngraveTrajectory>(this, pBullet);
}

template<typename T>
void EngraveTrajectoryType::Serialize(T& Stm)
{
	Stm
		.Process(this->AttachToTarget)
		.Process(this->UpdateDirection)
		.Process(this->FiringAnim)
		.Process(this->FiringAnimInterval)
		;
}

bool EngraveTrajectoryType::Load(PhobosStreamReader& Stm, bool RegisterForChange)
{
	this->VirtualTrajectoryType::Load(Stm, false);
	this->Serialize(Stm);
	return true;
}

bool EngraveTrajectoryType::Save(PhobosStreamWriter& Stm) const
{
	this->VirtualTrajectoryType::Save(Stm);
	const_cast<EngraveTrajectoryType*>(this)->Serialize(Stm);
	return true;
}

void EngraveTrajectoryType::Read(CCINIClass* const pINI, const char* pSection)
{
	this->PhobosTrajectoryType::Read(pINI, pSection);
	INI_EX exINI(pINI);

	// Limitation
	this->Speed = Math::min(128.0, this->Speed);

	// Virtual
	this->VirtualSourceCoord.Read(exINI, pSection, "Trajectory.Engrave.SourceCoord");
	this->VirtualTargetCoord.Read(exINI, pSection, "Trajectory.Engrave.TargetCoord");
	this->AllowFirerTurning.Read(exINI, pSection, "Trajectory.AllowFirerTurning");

	// Engrave
	this->AttachToTarget.Read(exINI, pSection, "Trajectory.Engrave.AttachToTarget");
	this->UpdateDirection.Read(exINI, pSection, "Trajectory.Engrave.UpdateDirection");
	this->FiringAnim.Read(exINI, pSection, "Trajectory.Engrave.FiringAnim");
	this->FiringAnimInterval.Read(exINI, pSection, "Trajectory.Engrave.FiringAnimInterval");
}

template<typename T>
void EngraveTrajectory::Serialize(T& Stm)
{
	Stm
		.Process(this->Type)
		.Process(this->RotateRadian)
		.Process(this->FiringAnimTimer)
		.Process(this->FiringAnimDuration)
		;
}

bool EngraveTrajectory::Load(PhobosStreamReader& Stm, bool RegisterForChange)
{
	this->VirtualTrajectory::Load(Stm, false);
	this->Serialize(Stm);
	return true;
}

bool EngraveTrajectory::Save(PhobosStreamWriter& Stm) const
{
	this->VirtualTrajectory::Save(Stm);
	const_cast<EngraveTrajectory*>(this)->Serialize(Stm);
	return true;
}

bool EngraveTrajectory::OnVelocityCheck()
{
	const auto pType = this->Type;

	if (pType->AttachToTarget || pType->UpdateDirection)
		this->ChangeVelocity();

	if (!BulletExt::Fetch(this->Bullet)->TargetIsInAir && this->PlaceOnCorrectHeight())
		return true;

	// Fork addition: replay the weapon's firing animation at the muzzle on a fixed
	// interval for the whole engrave process (the vanilla engine only plays it once
	// when the shot fires, which looks odd for a beam that lasts many frames).
	if (pType->FiringAnim && this->FiringAnimTimer.Completed())
	{
		this->PlayFiringAnim();
		this->FiringAnimTimer.Start(this->FiringAnimDuration);
	}

	return this->PhobosTrajectory::OnVelocityCheck();
}

void EngraveTrajectory::OpenFire()
{
	const auto pBullet = this->Bullet;
	const auto pBulletExt = BulletExt::Fetch(pBullet);
	const auto pType = this->Type;
	const auto pFirer = pBullet->Owner;
	auto virtualSource = BulletExt::Coord2Point(pType->VirtualSourceCoord.Get());
	auto virtualTarget = BulletExt::Coord2Point(pType->VirtualTargetCoord.Get());

	// Mirror trajectory
	if (!pBulletExt->NotMainWeapon && pType->MirrorCoord && this->CurrentBurst < 0)
		EngraveLine::MirrorVirtualCoord(virtualSource, virtualTarget);

	// To be used later, no reference
	auto source = pBullet->SourceCoords;
	auto target = pBullet->TargetCoords;
	this->RotateRadian = EngraveLine::GetRotateRadian((pFirer ? pFirer->GetCoords() : source), target);

	// Special case: Starting from the launch position
	if (EngraveLine::HasOffset(virtualSource))
		source = EngraveLine::AddVirtualOffset(target, virtualSource, this->RotateRadian);

	// If the target is in the air, there is no need to attach it to the ground
	if (!pBulletExt->TargetIsInAir)
		source.Z = this->GetFloorCoordHeight(source);

	// set initial status
	pBullet->SetLocation(source);
	target = EngraveLine::AddVirtualOffset(target, virtualTarget, this->RotateRadian);

	this->MovingVelocity.X = target.X - source.X;
	this->MovingVelocity.Y = target.Y - source.Y;
	this->MovingVelocity.Z = 0;

	if (this->CalculateBulletVelocity(pType->Speed))
		pBulletExt->Status |= TrajectoryStatus::Detonate;

	this->PrepareFiringAnim();

	this->PhobosTrajectory::OpenFire();
}

void EngraveTrajectory::PrepareFiringAnim()
{
	// Fork addition (see OnVelocityCheck): compute the interval between muzzle-anim
	// replays up front. With an interval of 0 the animation's own play duration
	// ((End - Start + 1) * Rate) is used so it replays seamlessly one after another.
	const auto pType = this->Type;

	if (!pType->FiringAnim)
	{
		this->FiringAnimDuration = 0;
		this->FiringAnimTimer.Stop();
		return;
	}

	const auto pBullet = this->Bullet;
	const auto pWeapon = pBullet->WeaponType;

	if (!pWeapon || pWeapon->Anim.Count <= 0)
	{
		this->FiringAnimDuration = 0;
		this->FiringAnimTimer.Stop();
		return;
	}

	const auto pAnimType = pWeapon->Anim[0];

	this->FiringAnimDuration = pType->FiringAnimInterval > 0
		? pType->FiringAnimInterval
		: Math::max(1, (pAnimType->End - pAnimType->Start + 1) * Math::max(1, pAnimType->Rate));

	this->FiringAnimTimer.Start(0);
}

void EngraveTrajectory::PlayFiringAnim()
{
	const auto pBullet = this->Bullet;
	const auto pWeapon = pBullet->WeaponType;

	if (!pWeapon)
		return;

	const auto animCounts = pWeapon->Anim.Count;

	if (animCounts <= 0)
		return;

	// Pick the anim the same way the game does when the weapon fires. Use the
	// engraving (moving) direction, like BulletExt::SimulatedFiringAnim does.
	const auto pBulletExt = BulletExt::Fetch(pBullet);
	const auto pTraj = pBulletExt->Trajectory.get();
	const auto velocityRadian = pTraj ? Math::atan2(pTraj->MovingVelocity.Y, pTraj->MovingVelocity.X) : Math::atan2(pBullet->Velocity.Y, pBullet->Velocity.X);
	const auto pAnimType = pWeapon->Anim[(animCounts % 8 == 0) // Have direction
		? (static_cast<int>((velocityRadian / Math::TwoPi + 1.5) * animCounts - (animCounts / 8) + 0.5) % animCounts) // Calculate direction
		: ScenarioClass::Instance->Random.RandomRanged(0, animCounts - 1)]; // Simple random;

	if (!pAnimType)
		return;

	// Place the anim at the same muzzle position the engrave laser is drawn from.
	auto fireCoord = pBullet->SourceCoords;
	auto pFirer = pBullet->Owner;
	const auto pOwner = pFirer ? pFirer->Owner : pBulletExt->FirerHouse;

	// Find the outermost transporter
	pFirer = BulletExt::GetSurfaceFirer(pFirer);

	if (!pBulletExt->NotMainWeapon && pFirer && !pFirer->InLimbo)
		fireCoord = TechnoExt::GetFLHAbsoluteCoords(pFirer, pBulletExt->FLHCoord, pFirer->HasTurret());

	const auto pAnim = GameCreate<AnimClass>(pAnimType, fireCoord);

	AnimExt::SetAnimOwnerHouseKind(pAnim, pOwner, nullptr, false, true);
	AnimExt::Fetch(pAnim)->SetInvoker(pBullet->Owner, pOwner);

	if (pFirer && !pFirer->InLimbo)
	{
		if (const auto pBuilding = abstract_cast<BuildingClass*, true>(pFirer))
		{
			pAnim->ZAdjust = (pBuilding->GetOccupantCount() > 0) ? -200 : 0;
		}
		else
		{
			pAnim->SetOwnerObject(pFirer);
		}
	}
}

bool EngraveTrajectory::CalculateBulletVelocity(const double speed)
{
	// Only call once
	// Substitute the speed to calculate velocity
	const double velocityLengthSq = this->MovingVelocity.MagnitudeSquared();

	if (velocityLengthSq < BulletExt::EpsilonSquared)
		return true;

	double velocityLength = sqrt(velocityLengthSq);
	const auto pBullet = this->Bullet;
	const auto pBulletExt = BulletExt::Fetch(pBullet);
	const auto pBulletTypeExt = pBulletExt->TypeExtData;
	const auto pType = this->Type;
	const auto pFirer = pBullet->Owner;

	// Calculate additional range
	if (pBulletTypeExt->ApplyRangeModifiers && pFirer)
	{
		if (const auto pWeapon = pBullet->WeaponType)
			velocityLength = static_cast<double>(WeaponTypeExt::GetRangeWithModifiers(pWeapon, pFirer, static_cast<int>(velocityLength)));

		if (velocityLength < BulletExt::Epsilon)
			return true;
	}

	// Automatically calculate duration
	if (pBulletTypeExt->LifeDuration <= 0)
		pBulletExt->LifeDurationTimer.Start(static_cast<int>(velocityLength / pType->Speed) + 1);
	else
		pBulletExt->LifeDurationTimer.Start(pBulletTypeExt->LifeDuration);

	this->MovingVelocity *= speed / velocityLength;
	this->MovingSpeed = speed;

	return false;
}

int EngraveTrajectory::GetFloorCoordHeight(const CoordStruct& coord)
{
	const auto pBullet = this->Bullet;

	return EngraveLine::GetFloorCoordHeight(coord, pBullet->SourceCoords.Z, pBullet->TargetCoords.Z);
}

void EngraveTrajectory::ChangeVelocity()
{
	const auto pBullet = this->Bullet;
	const auto pBulletExt = BulletExt::Fetch(pBullet);
	const auto pType = this->Type;

	// The center is located on the target
	if (pType->AttachToTarget)
	{
		// No need to synchronize the target again
		if (!pBulletExt->TypeExtData->Synchronize)
		{
			if (const auto pTarget = pBullet->Target)
				pBullet->TargetCoords = pTarget->GetCoords();
		}
	}

	// The angle will be updated according to the orientation
	if (pType->UpdateDirection)
	{
		const auto pFirer = pBullet->Owner;
		this->RotateRadian = BulletExt::Get2DOpRadian((pFirer ? pFirer->GetCoords() : pBullet->SourceCoords), pBullet->TargetCoords);
	}

	// Recalculate speed and position
	auto virtualSource = BulletExt::Coord2Point(pType->VirtualSourceCoord.Get());
	auto virtualTarget = BulletExt::Coord2Point(pType->VirtualTargetCoord.Get());

	if (!pBulletExt->NotMainWeapon && pType->MirrorCoord && this->CurrentBurst < 0)
		EngraveLine::MirrorVirtualCoord(virtualSource, virtualTarget);

	const double path = (pBulletExt->LifeDurationTimer.CurrentTime - pBulletExt->LifeDurationTimer.StartTime + 1) * pType->Speed;
	auto source = BulletExt::Coord2Point(pBullet->SourceCoords);
	auto target = BulletExt::Coord2Point(pBullet->TargetCoords);

	// Special case: Starting from the launch position
	if (EngraveLine::HasOffset(virtualSource))
		source = EngraveLine::AddVirtualOffset(target, virtualSource, this->RotateRadian);

	target = EngraveLine::AddVirtualOffset(target, virtualTarget, this->RotateRadian);

	CoordStruct newLocation {};

	if (!EngraveLine::GetCoordAtDistance(source, target, path, pBullet->TargetCoords.Z, newLocation))
	{
		pBulletExt->Status |= TrajectoryStatus::Detonate;
		return;
	}

	this->MovingVelocity = BulletExt::Coord2Vector(newLocation - pBullet->Location);
	this->MovingSpeed = this->MovingVelocity.Magnitude();
}

bool EngraveTrajectory::PlaceOnCorrectHeight()
{
	const auto pBullet = this->Bullet;
	auto bulletCoords = pBullet->Location;
	const auto futureCoords = bulletCoords + BulletExt::Vector2Coord(this->MovingVelocity);

	// Calculate where will be located in the next frame
	const auto checkDifference = this->GetFloorCoordHeight(futureCoords) - futureCoords.Z;

	// When crossing the cliff, directly move the position of the bullet, otherwise change the vertical velocity (384 -> (4 * Unsorted::LevelHeight - 32(error range)))
	if (std::abs(checkDifference) >= 384)
	{
		if (pBullet->Type->SubjectToCliffs)
			return true;

		// Move from low altitude to high altitude
		if (checkDifference > 0)
		{
			bulletCoords.Z += checkDifference;
			pBullet->SetLocation(bulletCoords);
		}
		else
		{
			const int nowDifference = bulletCoords.Z - this->GetFloorCoordHeight(bulletCoords);

			// Less than 384 and greater than the maximum difference that can be achieved between two non cliffs
			if (nowDifference >= 256)
			{
				bulletCoords.Z -= nowDifference;
				pBullet->SetLocation(bulletCoords);
			}
		}
	}
	else
	{
		this->MovingVelocity.Z += checkDifference;
		this->MovingSpeed = this->MovingVelocity.Magnitude();
	}

	return false;
}

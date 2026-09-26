#pragma once

#include "../PhobosVirtualTrajectory.h"

class EngraveTrajectoryType final : public VirtualTrajectoryType
{
public:
	EngraveTrajectoryType() : VirtualTrajectoryType()
		, AttachToTarget { false }
		, UpdateDirection { false }
		, FiringAnim { true }
		, FiringAnimInterval { 0 }
	{ }

	Valueable<bool> AttachToTarget;
	Valueable<bool> UpdateDirection;
	Valueable<bool> FiringAnim; // Keep replaying the weapon's firing animation at the muzzle for the whole engrave process
	Valueable<int> FiringAnimInterval; // Frames between two replays, 0 = use the animation's own play duration

	virtual bool Load(PhobosStreamReader& Stm, bool RegisterForChange) override;
	virtual bool Save(PhobosStreamWriter& Stm) const override;
	virtual std::unique_ptr<PhobosTrajectory> CreateInstance(BulletClass* pBullet) const override;
	virtual void Read(CCINIClass* const pINI, const char* pSection) override;
	virtual TrajectoryFlag Flag() const override { return TrajectoryFlag::Engrave; }

private:
	template <typename T>
	void Serialize(T& Stm);
};

class EngraveTrajectory final : public VirtualTrajectory
{
public:
	EngraveTrajectory(noinit_t) { }
	EngraveTrajectory(EngraveTrajectoryType const* pTrajType, BulletClass* pBullet)
		: VirtualTrajectory(pTrajType, pBullet)
		, Type { pTrajType }
		, RotateRadian { 0 }
		, FiringAnimTimer {}
		, FiringAnimDuration { 0 }
	{ }

	const EngraveTrajectoryType* Type;
	double RotateRadian;
	CDTimerClass FiringAnimTimer;
	int FiringAnimDuration;

	virtual bool Load(PhobosStreamReader& Stm, bool RegisterForChange) override;
	virtual bool Save(PhobosStreamWriter& Stm) const override;
	virtual TrajectoryFlag Flag() const override { return TrajectoryFlag::Engrave; }
	virtual bool OnVelocityCheck() override;
	virtual const PhobosTrajectoryType* GetType() const override { return this->Type; }
	virtual void OpenFire() override;
	virtual bool GetCanHitGround() const override { return false; }
	virtual bool CalculateBulletVelocity(const double speed) override;

private:
	int GetFloorCoordHeight(const CoordStruct& coord);
	void ChangeVelocity();
	bool PlaceOnCorrectHeight();
	void PrepareFiringAnim();
	void PlayFiringAnim();

	template <typename T>
	void Serialize(T& Stm);
};

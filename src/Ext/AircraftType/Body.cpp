#include "Body.h"

#include <WarheadTypeClass.h>
#include <cctype>
#include <cstring>

#include <Utilities/Debug.h>

AircraftTypeExt::ExtContainer AircraftTypeExt::ExtMap;

namespace
{
	// 引擎对"非 Ares 自定义导弹"的原版载荷键分组:
	// 匹配 [General]->V3RocketType 用 V3Rocket* 组, 匹配 CMislType 用 CMisl* 组,
	// 其余火箭类 Aircraft 一律用 DMisl* 组 (与引擎内部判断一致)。
	// [General] 存 *Damage/*EliteDamage; [CombatDamage] 存 *Warhead/*EliteWarhead。
	struct MissilePayloadKeys
	{
		const char* Damage;
		const char* EliteDamage;
		const char* Warhead;
		const char* EliteWarhead;
	};

	bool SameININame(const char* lhs, const char* rhs)
	{
		if (!lhs || !rhs)
			return false;

		for (; *lhs && *rhs; ++lhs, ++rhs)
		{
			const char l = static_cast<char>(tolower(static_cast<unsigned char>(*lhs)));
			const char r = static_cast<char>(tolower(static_cast<unsigned char>(*rhs)));
			if (l != r)
				return false;
		}

		return *lhs == *rhs;
	}

	MissilePayloadKeys GetVanillaMissilePayloadKeys(CCINIClass* pINI, const char* pSection)
	{
		if (pINI && pSection)
		{
			char buffer[0x40];

			if (pINI->ReadString("General", "V3RocketType", "", buffer) > 0 && SameININame(buffer, pSection))
				return { "V3RocketDamage", "V3RocketEliteDamage", "V3Warhead", "V3EliteWarhead" };

			if (pINI->ReadString("General", "CMislType", "", buffer) > 0 && SameININame(buffer, pSection))
				return { "CMislDamage", "CMislEliteDamage", "CMislWarhead", "CMislEliteWarhead" };
		}

		return { "DMislDamage", "DMislEliteDamage", "DMislWarhead", "DMislEliteWarhead" };
	}
}

// =============================
// load / save

void AircraftTypeExt::Initialize()
{
	TechnoTypeExt::Initialize();

	this->Missile_TakeOffAnim = AnimTypeClass::Find("V3TAKOFF");
}

void AircraftTypeExt::LoadFromINIFile(CCINIClass* const pINI)
{
	TechnoTypeExt::LoadFromINIFile(pINI);

	auto pThis = this->OwnerObject();
	const char* pSection = pThis->ID;
	INI_EX exINI(pINI);

	this->VoicePickup.Read(exINI, pSection, "VoicePickup");
	this->SpawnFromEdge.Read(exINI, pSection, "SpawnFromEdge");
	this->RetreatToEdge.Read(exINI, pSection, "RetreatToEdge");
	this->SpawnDistanceFromTarget.Read(exINI, pSection, "SpawnDistanceFromTarget");
	this->SpawnHeight.Read(exINI, pSection, "SpawnHeight");
	this->LandingDir.Read(exINI, pSection, "LandingDir");
	this->CurleyShuffle.Read(exINI, pSection, "CurleyShuffle");
	this->ExtendedAircraftMissions.Read(exINI, pSection, "ExtendedAircraftMissions");
	this->ExtendedAircraftMissions_SmoothMoving.Read(exINI, pSection, "ExtendedAircraftMissions.SmoothMoving");
	this->ExtendedAircraftMissions_EarlyDescend.Read(exINI, pSection, "ExtendedAircraftMissions.EarlyDescend");
	this->ExtendedAircraftMissions_RearApproach.Read(exINI, pSection, "ExtendedAircraftMissions.RearApproach");
	this->ExtendedAircraftMissions_FastScramble.Read(exINI, pSection, "ExtendedAircraftMissions.FastScramble");
	this->ExtendedAircraftMissions_UnlandDamage.Read(exINI, pSection, "ExtendedAircraftMissions.UnlandDamage");

	// AdvancedAircraftMissions —— 常驻盘旋 / 手动返航提速（详见 docs 详解）
	this->AdvancedAircraftMissions.Read(exINI, pSection, "AdvancedAircraftMissions");
	this->AdvancedAircraftMissions_LoiterRadius.Read(exINI, pSection, "AdvancedAircraftMissions.LoiterRadius");
	this->AdvancedAircraftMissions_LoiterAutoTarget.Read(exINI, pSection, "AdvancedAircraftMissions.LoiterAutoTarget");
	this->AdvancedAircraftMissions_HoverBrakeRange.Read(exINI, pSection, "AdvancedAircraftMissions.HoverBrakeRange");
	this->AdvancedAircraftMissions_ReturnSpeedMultiplier.Read(exINI, pSection, "AdvancedAircraftMissions.ReturnSpeedMultiplier");

	// 字符串枚举键：deny（默认，不作为）/ loiter（回家待命盘旋）
	if (exINI.ReadString(pSection, "AdvancedAircraftMissions.ReturnWithoutDock"))
	{
		if (_strcmpi(exINI.value(), "deny") == 0)
			this->AdvancedAircraftMissions_ReturnWithoutDock = false;
		else if (_strcmpi(exINI.value(), "loiter") == 0)
			this->AdvancedAircraftMissions_ReturnWithoutDock = true;
		else
			Debug::INIParseFailed(pSection, "AdvancedAircraftMissions.ReturnWithoutDock", exINI.value(), "Expected deny or loiter");
	}

	// 字符串枚举键：circle（默认，绕圈）/ hover（像直升机一样定在原地）
	if (exINI.ReadString(pSection, "AdvancedAircraftMissions.LoiterMode"))
	{
		if (_strcmpi(exINI.value(), "circle") == 0)
			this->AdvancedAircraftMissions_LoiterMode = false;
		else if (_strcmpi(exINI.value(), "hover") == 0)
			this->AdvancedAircraftMissions_LoiterMode = true;
		else
			Debug::INIParseFailed(pSection, "AdvancedAircraftMissions.LoiterMode", exINI.value(), "Expected circle or hover");
	}
	this->FiringForceScatter.Read(exINI, pSection, "FiringForceScatter");
	this->ParadropDelay.Read(exINI, pSection, "ParadropDelay");
	this->ParadropEndDelay.Read(exINI, pSection, "ParadropEndDelay");
	this->FlyNoWobbles.Read(exINI, pSection, "FlyNoWobbles");
	this->IsALoaner.Read(exINI, pSection, "IsALoaner");
	this->LandingAnim.Read(exINI, pSection, "LandingAnim");
	this->Missile_Cruise.Read(exINI, pSection, "Missile.Cruise");
	this->Missile_TakeOffSeparation.Read(exINI, pSection, "Missile.TakeOffSeparation");
	this->Missile_TakeOffAnim.Read(exINI, pSection, "Missile.TakeOffAnim");

	this->Missile_Homing.Read(exINI, pSection, "Missile.Homing");
	this->Homing_AirBurstRangeXY.Read(exINI, pSection, "Missile.Homing.AirBurstRangeXY");
	this->Homing_AirBurstRangeZ.Read(exINI, pSection, "Missile.Homing.AirBurstRangeZ");
	this->Homing_CruiseSkipRange.Read(exINI, pSection, "Missile.Homing.CruiseSkipRange");

	if (this->Missile_Homing)
	{
		// 对空引爆用的载荷: 同节 Ares 键 (Missile.Damage/EliteDamage/Warhead/EliteWarhead) 优先;
		// 某键缺失时, 按引擎规则从 [General]/[CombatDamage] 的 V3Rocket*/DMisl*/CMisl* 组回退。
		// (不修改也不覆盖 Ares 的读取, 仅本扩展自用。)
		const bool customDamageKey = exINI.ReadString(pSection, "Missile.Damage") > 0;
		const bool customEliteDamageKey = exINI.ReadString(pSection, "Missile.EliteDamage") > 0;
		const bool customWarheadKey = exINI.ReadString(pSection, "Missile.Warhead") > 0;
		const bool customEliteWarheadKey = exINI.ReadString(pSection, "Missile.EliteWarhead") > 0;

		if (customDamageKey)
			this->Homing_Damage.Read(exINI, pSection, "Missile.Damage");
		if (customEliteDamageKey)
			this->Homing_EliteDamage.Read(exINI, pSection, "Missile.EliteDamage");
		if (customWarheadKey)
			this->Homing_Warhead.Read<true>(exINI, pSection, "Missile.Warhead");
		if (customEliteWarheadKey)
			this->Homing_EliteWarhead.Read<true>(exINI, pSection, "Missile.EliteWarhead");

		const auto keys = GetVanillaMissilePayloadKeys(pINI, pSection);

		if (!customDamageKey)
		{
			int damage = 0;
			if (exINI.ReadInteger("General", keys.Damage, &damage))
				this->Homing_Damage = damage;
		}

		if (!customEliteDamageKey)
		{
			int damage = 0;
			if (exINI.ReadInteger("General", keys.EliteDamage, &damage))
				this->Homing_EliteDamage = damage;
		}

		if (!customWarheadKey)
		{
			if (exINI.ReadString("CombatDamage", keys.Warhead))
			{
				if (const auto pWarhead = WarheadTypeClass::FindOrAllocate(exINI.value()))
					this->Homing_Warhead = pWarhead;
			}
		}

		if (!customEliteWarheadKey)
		{
			if (exINI.ReadString("CombatDamage", keys.EliteWarhead))
			{
				if (const auto pWarhead = WarheadTypeClass::FindOrAllocate(exINI.value()))
					this->Homing_EliteWarhead = pWarhead;
			}
		}
	}
}

template <typename T>
void AircraftTypeExt::Serialize(T& Stm)
{
	Stm
		.Process(this->VoicePickup)
		.Process(this->SpawnFromEdge)
		.Process(this->RetreatToEdge)
		.Process(this->SpawnDistanceFromTarget)
		.Process(this->SpawnHeight)
		.Process(this->LandingDir)
		.Process(this->CurleyShuffle)
		.Process(this->ExtendedAircraftMissions)
		.Process(this->ExtendedAircraftMissions_SmoothMoving)
		.Process(this->ExtendedAircraftMissions_EarlyDescend)
		.Process(this->ExtendedAircraftMissions_RearApproach)
		.Process(this->ExtendedAircraftMissions_FastScramble)
		.Process(this->ExtendedAircraftMissions_UnlandDamage)
		.Process(this->AdvancedAircraftMissions)
		.Process(this->AdvancedAircraftMissions_LoiterRadius)
		.Process(this->AdvancedAircraftMissions_LoiterAutoTarget)
		.Process(this->AdvancedAircraftMissions_ReturnSpeedMultiplier)
		.Process(this->AdvancedAircraftMissions_ReturnWithoutDock)
		.Process(this->FiringForceScatter)
		.Process(this->ParadropDelay)
		.Process(this->ParadropEndDelay)
		.Process(this->FlyNoWobbles)
		.Process(this->IsALoaner)
		.Process(this->LandingAnim)
		.Process(this->Missile_Cruise)
		.Process(this->Missile_TakeOffAnim)
		.Process(this->Missile_TakeOffSeparation)
		.Process(this->Missile_Homing)
		.Process(this->Homing_Damage)
		.Process(this->Homing_EliteDamage)
		.Process(this->Homing_Warhead)
		.Process(this->Homing_EliteWarhead)
		.Process(this->Homing_AirBurstRangeXY)
		.Process(this->Homing_AirBurstRangeZ)
		.Process(this->Homing_CruiseSkipRange)
		// 新增字段一律追加在链尾：插在中间会让旧存档的整体布局错位一格，
		// 旧存档读进来时新字段会拿到下一个旧字段的值（LoiterMode 曾因此恒为 false）。
		.Process(this->AdvancedAircraftMissions_LoiterMode)
		.Process(this->AdvancedAircraftMissions_HoverBrakeRange)
		;
}

void AircraftTypeExt::LoadFromStream(PhobosStreamReader& Stm)
{
	TechnoTypeExt::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void AircraftTypeExt::SaveToStream(PhobosStreamWriter& Stm)
{
	TechnoTypeExt::SaveToStream(Stm);
	this->Serialize(Stm);
}

// =============================
// container

AircraftTypeExt::ExtContainer::ExtContainer() : Container("AircraftTypeClass") { }
AircraftTypeExt::ExtContainer::~ExtContainer() = default;

// =============================
// container hooks

DEFINE_HOOK(0x41C8C0, AircraftTypeClass_CTOR, 0x5)
{
	GET(AircraftTypeClass*, pItem, ESI);

	AircraftTypeExt::ExtMap.Allocate(pItem);

	return 0;
}

// The extension chain is read at the end of each concrete type class's LoadFromINI,
// once every native field - inherited and own alike - has been parsed.
DEFINE_HOOK(0x41CD82, AircraftTypeClass_LoadFromINI, 0x7)
{
	GET(AircraftTypeClass*, pItem, ESI);
	GET_STACK(CCINIClass*, pINI, 0x98);

	if (auto const pExt = AircraftTypeExt::TryFetch(pItem))
		pExt->LoadFromINI(pINI);

	return 0;
}

// Hooked after the base destructor call in both destructor bodies; the second site
// is the tail of the standalone body (pop/pop/retn, safe to steal - the bytes after
// it are alignment padding that is never executed).
DEFINE_HOOK_AGAIN(0x41CA96, AircraftTypeClass_DTOR, 0x3)
DEFINE_HOOK(0x41D056, AircraftTypeClass_DTOR, 0x5)
{
	GET(AircraftTypeClass*, pItem, ESI);

	AircraftTypeExt::ExtMap.Remove(pItem);

	return 0;
}

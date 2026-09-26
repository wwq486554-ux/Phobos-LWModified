#include "SpecialAction.h"

#include <Ext/Techno/SpecialAction.h>
#include <Utilities/GeneralUtils.h>

const char* SpecialActionCommandClass::GetName() const
{
	return "Special Action";
}

const wchar_t* SpecialActionCommandClass::GetUIName() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_SPECIAL_ACTION", L"Special Action");
}

const wchar_t* SpecialActionCommandClass::GetUICategory() const
{
	return CATEGORY_CONTROL;
}

const wchar_t* SpecialActionCommandClass::GetUIDescription() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_SPECIAL_ACTION_DESC", L"Triggers the special action of the selected units.");
}

// One key for everything. Every selected object performs its own action; the
// Weapon action only arms the unit for its next attack, so nothing here needs a
// target and there is no second key.
void SpecialActionCommandClass::Execute(WWKey eInput) const
{
	if constexpr (!SpecialAction::Enabled)
		return;

	auto const& objects = ObjectClass::CurrentObjects;

	for (int i = 0; i < objects.Count; ++i)
	{
		auto const pTechno = abstract_cast<TechnoClass*>(objects.GetItem(i));

		if (pTechno)
			SpecialAction::Execute(pTechno);
	}
}

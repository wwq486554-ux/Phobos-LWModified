#pragma once

#include "Commands.h"

// Single SpecialAction hotkey.
//
// Actions that need no target run immediately. Aiming (needed only by Weapon) is
// currently not implemented, so a Weapon action must not be configured.
class SpecialActionCommandClass : public CommandClass
{
public:
	virtual const char* GetName() const override;
	virtual const wchar_t* GetUIName() const override;
	virtual const wchar_t* GetUICategory() const override;
	virtual const wchar_t* GetUIDescription() const override;
	virtual void Execute(WWKey eInput) const override;
};

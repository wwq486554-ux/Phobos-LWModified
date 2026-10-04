#include "ViewZoomCommands.h"

#include <Misc/ViewZoom.h>
#include <Utilities/GeneralUtils.h>

const char* ViewZoomInCommandClass::GetName() const
{
	return "View Zoom In";
}

const wchar_t* ViewZoomInCommandClass::GetUIName() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_VIEW_ZOOM_IN", L"Zoom In (Tactical View)");
}

const wchar_t* ViewZoomInCommandClass::GetUICategory() const
{
	return CATEGORY_INTERFACE;
}

const wchar_t* ViewZoomInCommandClass::GetUIDescription() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_VIEW_ZOOM_IN_DESC", L"Zoom the tactical view in. Bind to the numpad + key.");
}

void ViewZoomInCommandClass::Execute(WWKey eInput) const
{
	ViewZoomClass::Step(-1);
}

const char* ViewZoomOutCommandClass::GetName() const
{
	return "View Zoom Out";
}

const wchar_t* ViewZoomOutCommandClass::GetUIName() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_VIEW_ZOOM_OUT", L"Zoom Out (Tactical View)");
}

const wchar_t* ViewZoomOutCommandClass::GetUICategory() const
{
	return CATEGORY_INTERFACE;
}

const wchar_t* ViewZoomOutCommandClass::GetUIDescription() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_VIEW_ZOOM_OUT_DESC", L"Zoom the tactical view out. Bind to the numpad - key.");
}

void ViewZoomOutCommandClass::Execute(WWKey eInput) const
{
	ViewZoomClass::Step(1);
}

const char* ViewZoomResetCommandClass::GetName() const
{
	return "Reset View Zoom";
}

const wchar_t* ViewZoomResetCommandClass::GetUIName() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_VIEW_ZOOM_RESET", L"Reset Tactical View Zoom");
}

const wchar_t* ViewZoomResetCommandClass::GetUICategory() const
{
	return CATEGORY_INTERFACE;
}

const wchar_t* ViewZoomResetCommandClass::GetUIDescription() const
{
	return GeneralUtils::LoadStringUnlessMissing("TXT_VIEW_ZOOM_RESET_DESC", L"Return the tactical view to the resolution-based zoom level.");
}

void ViewZoomResetCommandClass::Execute(WWKey eInput) const
{
	ViewZoomClass::ResetToNative("command");
}

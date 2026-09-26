#include "Phobos.COM.h"

#include <Helpers/Macro.h>

#include <Locomotion/LocomotorWeaponLocomotionClass.h>


// Registers the custom locomotor class factories. This has to happen during
// WinMain: savegames store locomotor instances by CLSID and CoCreateInstance
// needs the factory to already exist when a game is loaded.
DEFINE_HOOK(0x6BD68D, WinMain_PhobosRegistrations, 0x6)
{
	Debug::Log("Starting COM registration...\n");

	// Add new classes to be COM-registered below
	RegisterFactoryForClass<LocomotorWeaponLocomotionClass>();

	Debug::Log("COM registration done!\n");

	return 0;
}

// "Door" number 2: pretends to be dinput8.dll, hands every real request to the
// genuine Windows dinput8.dll, and starts the overlay as a side effect.

#include "core.hpp"

#define FORWARD(name) \
	__pragma(comment(linker, "/export:" #name "=\"C:\\Windows\\System32\\dinput8." #name "\""))

FORWARD(DirectInput8Create)
FORWARD(DllCanUnloadNow)
FORWARD(DllGetClassObject)
FORWARD(DllRegisterServer)
FORWARD(DllUnregisterServer)

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		overlay_start(module);
	}

	return TRUE;
}

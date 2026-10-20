// "Door" number 1: pretends to be version.dll, hands every real request to the
// genuine Windows version.dll, and starts the overlay as a side effect.

#include "core.hpp"

#define FORWARD(name) \
	__pragma(comment(linker, "/export:" #name "=\"C:\\Windows\\System32\\version." #name "\""))

FORWARD(GetFileVersionInfoA)
FORWARD(GetFileVersionInfoW)
FORWARD(GetFileVersionInfoExA)
FORWARD(GetFileVersionInfoExW)
FORWARD(GetFileVersionInfoSizeA)
FORWARD(GetFileVersionInfoSizeW)
FORWARD(GetFileVersionInfoSizeExA)
FORWARD(GetFileVersionInfoSizeExW)
FORWARD(VerFindFileA)
FORWARD(VerFindFileW)
FORWARD(VerInstallFileA)
FORWARD(VerInstallFileW)
FORWARD(VerLanguageNameA)
FORWARD(VerLanguageNameW)
FORWARD(VerQueryValueA)
FORWARD(VerQueryValueW)

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		overlay_start(module);
	}

	return TRUE;
}

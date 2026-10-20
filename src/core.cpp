// S1x debug overlay - version 0.1 ("does it load?" test build)
//
// All this build does is write a log file next to the DLL describing what it can
// see inside the game. It draws nothing and changes nothing in the game.

#include "core.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
	// Campaign (single-player) entity list address, taken from the S1x source files.
	constexpr uintptr_t SP_G_ENTITIES = 0x143C26DC0;

	volatile LONG g_started = 0;
	HMODULE g_self = nullptr;
	char g_log_path[MAX_PATH] = {};

	void log_line(const char* fmt, ...)
	{
		FILE* f = nullptr;
		if (fopen_s(&f, g_log_path, "a") != 0 || !f)
		{
			return;
		}

		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(f, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);

		va_list args;
		va_start(args, fmt);
		vfprintf(f, fmt, args);
		va_end(args);

		fputc('\n', f);
		fclose(f);
	}

	// Copies bytes out of the game's memory, but only if it is safe to do so.
	bool read_bytes(const uintptr_t address, unsigned char* out, const size_t count)
	{
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0)
		{
			return false;
		}

		if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
		{
			return false;
		}

		const auto region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (address + count > region_end)
		{
			return false;
		}

		__try
		{
			memcpy(out, reinterpret_cast<const void*>(address), count);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}

		return true;
	}

	void sample(const int index)
	{
		const bool game_module = GetModuleHandleA("s1_sp64_ship.exe") != nullptr;
		log_line("check %d: separate s1_sp64_ship.exe module present: %s", index, game_module ? "yes" : "no");

		unsigned char bytes[32] = {};
		if (!read_bytes(SP_G_ENTITIES, bytes, sizeof(bytes)))
		{
			log_line("check %d: campaign entity list address NOT readable yet", index);
			return;
		}

		char hex[sizeof(bytes) * 3 + 1] = {};
		for (size_t i = 0; i < sizeof(bytes); ++i)
		{
			sprintf_s(hex + i * 3, 4, "%02X ", bytes[i]);
		}

		log_line("check %d: campaign entity list address readable. first bytes: %s", index, hex);
	}

	DWORD WINAPI worker(LPVOID)
	{
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exe, MAX_PATH);

		log_line("---- add-on loaded (version 0.1, load test) ----");
		log_line("host program: %s", exe);
		log_line("host base address: 0x%p", static_cast<void*>(GetModuleHandleA(nullptr)));
		log_line("process id: %lu", GetCurrentProcessId());

		// Look once every 5 seconds for about 2 minutes, then go quiet.
		for (int i = 1; i <= 24; ++i)
		{
			sample(i);
			Sleep(5000);
		}

		log_line("finished checking; add-on is now idle");
		return 0;
	}
}

void overlay_start(HMODULE self)
{
	if (InterlockedCompareExchange(&g_started, 1, 0) != 0)
	{
		return;
	}

	g_self = self;

	// Put the log file in the same folder as this DLL.
	GetModuleFileNameA(g_self, g_log_path, MAX_PATH);
	char* slash = strrchr(g_log_path, '\\');
	if (slash)
	{
		*(slash + 1) = '\0';
	}
	strcat_s(g_log_path, MAX_PATH, "s1x_debug_overlay.log");

	// Do the real work on a separate thread so loading the game is never held up.
	HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
	if (thread)
	{
		CloseHandle(thread);
	}
}

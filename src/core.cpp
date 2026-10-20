// S1x debug overlay - version 0.2 ("data collection" build)
//
// This build still draws nothing and changes nothing in the game. It only READS
// memory and writes text files next to the DLL so we can work out how the
// campaign stores its entities (triggers, etc.):
//   - entity_dump_N.txt   a hex copy of the start of the entity list
//   - script_strings.txt  names of entity types the game knows about
//   - s1x_debug_overlay.log  what happened, step by step
//
// A dump is taken automatically ~10 seconds after a mission has loaded, and again
// every time you press F9 (up to 6 dumps per game session).

#include "core.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
	// Addresses for the campaign (single-player), taken from the S1x source files.
	constexpr uintptr_t SP_G_ENTITIES = 0x143C26DC0;
	constexpr uintptr_t SP_SL_CONVERT_TO_STRING = 0x140314850;

	constexpr size_t DUMP_SIZE = 0x10000; // 64 KB of the entity list
	constexpr int MAX_DUMPS = 6;

	volatile LONG g_started = 0;
	HMODULE g_self = nullptr;
	char g_dir[MAX_PATH] = {};
	char g_log_path[MAX_PATH] = {};

	void make_path(char* out, const char* file_name)
	{
		strcpy_s(out, MAX_PATH, g_dir);
		strcat_s(out, MAX_PATH, file_name);
	}

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

	// Copies a short plain-text string out of game memory. Returns false if it
	// isn't a sensible string.
	bool safe_copy_string(const char* source, char* out, const size_t capacity)
	{
		__try
		{
			size_t i = 0;
			for (; i + 1 < capacity; ++i)
			{
				const char c = source[i];
				if (c == '\0')
				{
					break;
				}

				if (c < 0x20 || c > 0x7E)
				{
					out[0] = '\0';
					return false;
				}

				out[i] = c;
			}

			out[i] = '\0';
			return i > 0 && i + 1 < capacity;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Asks the game for the text of one of its script strings.
	const char* convert_string(const unsigned int id)
	{
		using function_t = const char* (*)(unsigned int);
		const auto function = reinterpret_cast<function_t>(SP_SL_CONVERT_TO_STRING);

		__try
		{
			return function(id);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	bool starts_with_any(const char* text)
	{
		static const char* const prefixes[] = {
			"trigger", "script_", "info_", "player", "actor", "misc_", "light",
			"func_", "item_", "weapon_", "vehicle", "destructible", "turret",
			"worldspawn", "noclass", "node", "door", "pickup", "sound", "ambient",
		};

		for (const char* prefix : prefixes)
		{
			if (strncmp(text, prefix, strlen(prefix)) == 0)
			{
				return true;
			}
		}

		return false;
	}

	void dump_strings()
	{
		char path[MAX_PATH];
		make_path(path, "script_strings.txt");

		FILE* f = nullptr;
		if (fopen_s(&f, path, "w") != 0 || !f)
		{
			log_line("could not create script_strings.txt");
			return;
		}

		int written = 0;
		for (unsigned int id = 1; id <= 0xFFFF; ++id)
		{
			const char* text = convert_string(id);
			if (!text)
			{
				continue;
			}

			char copy[64];
			if (!safe_copy_string(text, copy, sizeof(copy)) || !starts_with_any(copy))
			{
				continue;
			}

			fprintf(f, "%u\t%s\n", id, copy);
			++written;
		}

		fclose(f);
		log_line("wrote script_strings.txt (%d names)", written);
	}

	void dump_entities(const int number)
	{
		char name[64];
		sprintf_s(name, sizeof(name), "entity_dump_%d.txt", number);

		char path[MAX_PATH];
		make_path(path, name);

		FILE* f = nullptr;
		if (fopen_s(&f, path, "w") != 0 || !f)
		{
			log_line("could not create %s", name);
			return;
		}

		fprintf(f, "base 0x%llX size 0x%zX\n", static_cast<unsigned long long>(SP_G_ENTITIES), DUMP_SIZE);

		unsigned char row[16];
		for (size_t offset = 0; offset < DUMP_SIZE; offset += sizeof(row))
		{
			if (!read_bytes(SP_G_ENTITIES + offset, row, sizeof(row)))
			{
				fprintf(f, "%05zX: ??\n", offset);
				continue;
			}

			fprintf(f, "%05zX:", offset);
			for (const unsigned char byte : row)
			{
				fprintf(f, " %02X", byte);
			}

			fputc('\n', f);
		}

		fclose(f);
		log_line("wrote %s", name);
	}

	bool entity_list_has_data()
	{
		unsigned char bytes[32] = {};
		if (!read_bytes(SP_G_ENTITIES, bytes, sizeof(bytes)))
		{
			return false;
		}

		for (const unsigned char byte : bytes)
		{
			if (byte != 0)
			{
				return true;
			}
		}

		return false;
	}

	DWORD WINAPI worker(LPVOID)
	{
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exe, MAX_PATH);

		log_line("---- add-on loaded (version 0.2, data collection) ----");
		log_line("host program: %s", exe);
		log_line("press F9 in a mission to save an extra dump");

		int dumps = 0;
		int seconds_with_data = 0;
		bool auto_dump_done = false;
		bool f9_was_down = false;

		// 100 ms per tick; stop after about 30 minutes.
		for (int tick = 1; tick <= 18000; ++tick)
		{
			Sleep(100);

			if (tick % 10 == 0)
			{
				seconds_with_data = entity_list_has_data() ? seconds_with_data + 1 : 0;
			}

			const bool f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
			const bool f9_pressed = f9_down && !f9_was_down;
			f9_was_down = f9_down;

			const bool auto_dump = !auto_dump_done && seconds_with_data >= 10;
			if ((auto_dump || f9_pressed) && dumps < MAX_DUMPS)
			{
				if (auto_dump)
				{
					auto_dump_done = true;
					log_line("mission data detected; taking automatic dump");
				}
				else
				{
					log_line("F9 pressed");
				}

				++dumps;
				dump_entities(dumps);

				if (dumps == 1)
				{
					dump_strings();
				}
			}
		}

		log_line("30 minutes passed; add-on is now idle");
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

	// Everything is written into the same folder as this DLL.
	GetModuleFileNameA(g_self, g_dir, MAX_PATH);
	char* slash = strrchr(g_dir, '\\');
	if (slash)
	{
		*(slash + 1) = '\0';
	}

	make_path(g_log_path, "s1x_debug_overlay.log");

	// Do the real work on a separate thread so loading the game is never held up.
	HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
	if (thread)
	{
		CloseHandle(thread);
	}
}

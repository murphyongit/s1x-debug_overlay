// S1x debug overlay - version 0.3 ("entity table" build)
//
// Still invisible: this build only READS memory and writes text files next to the DLL.
//   F9   -> writes entities_N.txt  (every entity in the level: type, name, position, size)
//           and trigger_raw_N.txt  (raw bytes of every trigger, so we can find their sizes)
//   F10  -> writes origin_hits.txt (finds where the game keeps your camera position;
//           stand completely still before pressing it)
// F9 also fires automatically ~10 seconds after a mission has loaded.

#include "core.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
	// Addresses for the campaign (single-player), from the S1x source files.
	constexpr uintptr_t SP_G_ENTITIES = 0x143C26DC0;
	constexpr uintptr_t SP_SL_CONVERT_TO_STRING = 0x140314850;
	constexpr uintptr_t GAME_IMAGE_BASE = 0x140000000;

	// Entity layout worked out from your dump files.
	constexpr size_t ENTITY_SIZE = 0x2F8;
	constexpr int MAX_ENTITIES = 2048;
	constexpr size_t OFF_NUMBER = 0x7C;   // 16-bit entity number
	constexpr size_t OFF_ORIGIN = 0x1C;   // 3 floats
	constexpr size_t OFF_BOUNDS = 0xDC;   // 6 floats: midpoint xyz, half-size xyz
	constexpr size_t OFF_CLASSNAME = 0x14C; // 16-bit script string id

	constexpr int MAX_TABLES = 6;

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
	bool read_bytes(const uintptr_t address, void* out, const size_t count)
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

	// Copies a short plain-text string out of game memory.
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

	bool class_name(const unsigned int id, char* out, const size_t capacity)
	{
		if (id == 0)
		{
			return false;
		}

		const char* text = convert_string(id);
		return text && safe_copy_string(text, out, capacity);
	}

	bool is_trigger_like(const char* name)
	{
		return strncmp(name, "trigger", 7) == 0 || strncmp(name, "script_brushmodel", 17) == 0;
	}

	void write_table(const int number)
	{
		char entities_name[64];
		char raw_name[64];
		sprintf_s(entities_name, sizeof(entities_name), "entities_%d.txt", number);
		sprintf_s(raw_name, sizeof(raw_name), "trigger_raw_%d.txt", number);

		char entities_path[MAX_PATH];
		char raw_path[MAX_PATH];
		make_path(entities_path, entities_name);
		make_path(raw_path, raw_name);

		FILE* table = nullptr;
		FILE* raw = nullptr;
		if (fopen_s(&table, entities_path, "w") != 0 || !table)
		{
			log_line("could not create %s", entities_name);
			return;
		}

		if (fopen_s(&raw, raw_path, "w") != 0 || !raw)
		{
			fclose(table);
			log_line("could not create %s", raw_name);
			return;
		}

		fprintf(table, "index\tclassid\tclassname\torigin_xyz\tbounds_mid_xyz\tbounds_half_xyz\n");

		int listed = 0;
		int triggers = 0;
		unsigned char entity[ENTITY_SIZE];

		for (int index = 0; index < MAX_ENTITIES; ++index)
		{
			const uintptr_t address = SP_G_ENTITIES + static_cast<uintptr_t>(index) * ENTITY_SIZE;
			if (!read_bytes(address, entity, sizeof(entity)))
			{
				continue;
			}

			unsigned short number = 0;
			unsigned short class_id = 0;
			float origin[3];
			float bounds[6];
			memcpy(&number, entity + OFF_NUMBER, sizeof(number));
			memcpy(&class_id, entity + OFF_CLASSNAME, sizeof(class_id));
			memcpy(origin, entity + OFF_ORIGIN, sizeof(origin));
			memcpy(bounds, entity + OFF_BOUNDS, sizeof(bounds));

			if (number != index || class_id == 0)
			{
				continue;
			}

			char name[64] = "?";
			class_name(class_id, name, sizeof(name));

			fprintf(table, "%d\t%u\t%s\t%.1f %.1f %.1f\t%.1f %.1f %.1f\t%.1f %.1f %.1f\n",
			        index, class_id, name,
			        origin[0], origin[1], origin[2],
			        bounds[0], bounds[1], bounds[2],
			        bounds[3], bounds[4], bounds[5]);
			++listed;

			if (is_trigger_like(name))
			{
				++triggers;
				fprintf(raw, "== entity %d (%s)\n", index, name);
				for (size_t offset = 0; offset < ENTITY_SIZE; offset += 16)
				{
					fprintf(raw, "%03zX:", offset);
					for (size_t i = 0; i < 16 && offset + i < ENTITY_SIZE; ++i)
					{
						fprintf(raw, " %02X", entity[offset + i]);
					}

					fputc('\n', raw);
				}
			}
		}

		fclose(table);
		fclose(raw);
		log_line("wrote %s (%d entities) and %s (%d triggers)", entities_name, listed, raw_name, triggers);
	}

	// Finds every place in the game's memory that holds a copy of the player's X and Y.
	void scan_for_origin()
	{
		float player[3];
		if (!read_bytes(SP_G_ENTITIES + OFF_ORIGIN, player, sizeof(player)))
		{
			log_line("scan: could not read player position");
			return;
		}

		log_line("scan: looking for player position %.2f %.2f %.2f", player[0], player[1], player[2]);

		char path[MAX_PATH];
		make_path(path, "origin_hits.txt");

		FILE* f = nullptr;
		if (fopen_s(&f, path, "w") != 0 || !f)
		{
			log_line("could not create origin_hits.txt");
			return;
		}

		fprintf(f, "player position: %.4f %.4f %.4f\n", player[0], player[1], player[2]);

		uint32_t want_x, want_y;
		memcpy(&want_x, &player[0], 4);
		memcpy(&want_y, &player[1], 4);

		int hits = 0;
		uintptr_t cursor = 0x10000;
		MEMORY_BASIC_INFORMATION mbi;

		while (cursor < 0x7FFFFFFE0000ull && VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) != 0)
		{
			const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
			const size_t size = mbi.RegionSize;
			cursor = base + size;

			// Skip "mapped" memory (graphics card windows can be extremely slow to read).
			const bool readable = mbi.State == MEM_COMMIT && mbi.Type != MEM_MAPPED &&
				(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
			if (!readable || size < 16 || hits >= 150)
			{
				continue;
			}

			__try
			{
				const auto* words = reinterpret_cast<const uint32_t*>(base);
				const size_t count = size / 4;
				for (size_t i = 0; i + 1 < count && hits < 150; ++i)
				{
					if (words[i] == want_x && words[i + 1] == want_y)
					{
						const uintptr_t hit = base + i * 4;
						++hits;

						fprintf(f, "\nHIT %d at 0x%llX (region 0x%llX size 0x%llX)", hits,
						        static_cast<unsigned long long>(hit),
						        static_cast<unsigned long long>(base),
						        static_cast<unsigned long long>(size));
						if (hit >= GAME_IMAGE_BASE && hit < GAME_IMAGE_BASE + 0x20000000)
						{
							fprintf(f, " [game exe +0x%llX]", static_cast<unsigned long long>(hit - GAME_IMAGE_BASE));
						}

						fputc('\n', f);

						// 192 bytes starting 64 bytes before the hit.
						const uintptr_t from = hit >= base + 64 ? hit - 64 : base;
						for (uintptr_t row = from; row < from + 192 && row + 16 <= base + size; row += 16)
						{
							fprintf(f, "%llX:", static_cast<unsigned long long>(row));
							for (int b = 0; b < 16; ++b)
							{
								fprintf(f, " %02X", *reinterpret_cast<const unsigned char*>(row + b));
							}

							fputc('\n', f);
						}
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				// Region vanished while we were reading it; carry on.
			}
		}

		fclose(f);
		log_line("scan: finished, %d hits written to origin_hits.txt", hits);
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

		log_line("---- add-on loaded (version 0.3, entity table) ----");
		log_line("host program: %s", exe);
		log_line("F9 = save entity table, F10 = scan for camera position");

		int tables = 0;
		int seconds_with_data = 0;
		bool auto_done = false;
		bool f9_was_down = false;
		bool f10_was_down = false;
		int scans = 0;

		// 100 ms per tick; stop after about 30 minutes.
		for (int tick = 1; tick <= 18000; ++tick)
		{
			Sleep(100);

			if (tick % 10 == 0)
			{
				seconds_with_data = entity_list_has_data() ? seconds_with_data + 1 : 0;
			}

			const bool f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
			const bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
			const bool f9_pressed = f9_down && !f9_was_down;
			const bool f10_pressed = f10_down && !f10_was_down;
			f9_was_down = f9_down;
			f10_was_down = f10_down;

			const bool auto_table = !auto_done && seconds_with_data >= 10;
			if ((auto_table || f9_pressed) && tables < MAX_TABLES)
			{
				if (auto_table)
				{
					auto_done = true;
					log_line("mission data detected; taking automatic table");
				}
				else
				{
					log_line("F9 pressed");
				}

				++tables;
				write_table(tables);
			}

			if (f10_pressed && scans < 3)
			{
				log_line("F10 pressed");
				++scans;
				scan_for_origin();
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

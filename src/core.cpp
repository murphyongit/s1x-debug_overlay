// S1x debug overlay - version 0.7 (first build that DRAWS; 0.6 crashed on a stale material pointer)
//
// Draws a box around every trigger in the campaign. Because this could not be tested
// before shipping, a few keys let you try different drawing settings:
//   F8 = overlay on/off (starts OFF)         F6 = drawing mode (4 choices)
//   F7 = how far away to draw    F5 = field-of-view style (3 choices)
//   F4 = text labels on/off      F9 = save the data files (as before)
// Small squares in the top-left corner show the current F6 / F7 / F5 settings.

#include "core.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

#include "MinHook.h"

namespace
{
	// Addresses for the campaign (single-player), from the S1x source files.
	constexpr uintptr_t SP_G_ENTITIES = 0x143C26DC0;
	constexpr uintptr_t SP_SL_CONVERT_TO_STRING = 0x140314850;

	// Entity layout worked out from your dump files.
	constexpr size_t ENTITY_SIZE = 0x2F8;
	constexpr int MAX_ENTITIES = 2048;
	constexpr size_t OFF_NUMBER = 0x7C;   // 16-bit entity number
	constexpr size_t OFF_ORIGIN = 0x1C;   // 3 floats
	constexpr size_t OFF_BOUNDS = 0xDC;   // 6 floats: midpoint xyz, half-size xyz
	constexpr size_t OFF_CLASSNAME = 0x14C; // 16-bit script string id
	constexpr size_t OFF_CLIENT = 0x118;    // 64-bit pointer to the player's client block
	constexpr size_t CLIENT_DUMP_SIZE = 0x1000;

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

	struct census_entry
	{
		char name[48];
		int count;
	};

	void write_table(const int number)
	{
		char entities_name[64];
		char raw_name[64];
		sprintf_s(entities_name, sizeof(entities_name), "entities_%d.txt", number);
		sprintf_s(raw_name, sizeof(raw_name), "other_raw_%d.txt", number);

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

		fprintf(table, "slot\tnumber\ttypebyte\tclassid\tclassname\torigin_xyz\tbounds_mid_xyz\tbounds_half_xyz\tnonzero_bytes\n");

		census_entry census[64] = {};
		int census_used = 0;
		int listed = 0;
		int empty = 0;
		int unreadable = 0;
		int raw_written = 0;
		unsigned char entity[ENTITY_SIZE];

		for (int index = 0; index < MAX_ENTITIES; ++index)
		{
			const uintptr_t address = SP_G_ENTITIES + static_cast<uintptr_t>(index) * ENTITY_SIZE;
			if (!read_bytes(address, entity, sizeof(entity)))
			{
				++unreadable;
				continue;
			}

			int nonzero = 0;
			for (const unsigned char byte : entity)
			{
				if (byte != 0)
				{
					++nonzero;
				}
			}

			if (nonzero == 0)
			{
				++empty;
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

			char name[48] = "-";
			if (class_id != 0)
			{
				strcpy_s(name, sizeof(name), "?");
				class_name(class_id, name, sizeof(name));
			}

			fprintf(table, "%d\t%u\t%u\t%u\t%s\t%.1f %.1f %.1f\t%.1f %.1f %.1f\t%.1f %.1f %.1f\t%d\n",
			        index, number, entity[0], class_id, name,
			        origin[0], origin[1], origin[2],
			        bounds[0], bounds[1], bounds[2],
			        bounds[3], bounds[4], bounds[5], nonzero);
			++listed;

			int found = -1;
			for (int i = 0; i < census_used; ++i)
			{
				if (strcmp(census[i].name, name) == 0)
				{
					found = i;
					break;
				}
			}

			if (found < 0 && census_used < 64)
			{
				found = census_used++;
				strcpy_s(census[found].name, sizeof(census[found].name), name);
			}

			if (found >= 0)
			{
				++census[found].count;
			}

			const bool ordinary = strcmp(name, "player") == 0 || strcmp(name, "scriptable") == 0;
			if (!ordinary && raw_written < 60)
			{
				++raw_written;
				fprintf(raw, "== slot %d (%s)\n", index, name);
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

		log_line("wrote %s: %d non-empty slots, %d empty, %d unreadable; %d raw entities in %s",
		         entities_name, listed, empty, unreadable, raw_written, raw_name);
		for (int i = 0; i < census_used; ++i)
		{
			log_line("  kind '%s': %d", census[i].name, census[i].count);
		}
	}

	// Saves the first part of the player's own data block (position, view angles, ...).
	void write_client(const int number)
	{
		unsigned long long pointer = 0;
		if (!read_bytes(SP_G_ENTITIES + OFF_CLIENT, &pointer, sizeof(pointer)))
		{
			log_line("client: could not read the player's client pointer");
			return;
		}

		if (pointer < 0x140000000ull || pointer > 0x7FFFFFFE0000ull)
		{
			log_line("client: pointer 0x%llX looks wrong, skipping", pointer);
			return;
		}

		char name[64];
		sprintf_s(name, sizeof(name), "client_%d.txt", number);
		char path[MAX_PATH];
		make_path(path, name);

		FILE* f = nullptr;
		if (fopen_s(&f, path, "w") != 0 || !f)
		{
			log_line("could not create %s", name);
			return;
		}

		fprintf(f, "client block at 0x%llX\n", pointer);

		unsigned char row[16];
		int rows = 0;
		for (size_t offset = 0; offset < CLIENT_DUMP_SIZE; offset += sizeof(row))
		{
			if (!read_bytes(static_cast<uintptr_t>(pointer) + offset, row, sizeof(row)))
			{
				fprintf(f, "%04zX: ??\n", offset);
				continue;
			}

			fprintf(f, "%04zX:", offset);
			for (const unsigned char byte : row)
			{
				fprintf(f, " %02X", byte);
			}

			fputc('\n', f);
			++rows;
		}

		fclose(f);
		log_line("wrote %s (pointer 0x%llX, %d rows)", name, pointer, rows);
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

	// ------------------------------------------------------------------------------
	// Drawing
	// ------------------------------------------------------------------------------
	constexpr uintptr_t SP_COM_FRAME = 0x1402F7E10;
	constexpr uintptr_t SP_DVAR_FIND_VAR = 0x140370860;
	constexpr uintptr_t SP_MATERIAL_REGISTER = 0x1404919D0;
	constexpr uintptr_t SP_DRAW_STRETCH_PIC = 0x1404A2580;
	constexpr uintptr_t SP_DRAW_TEXT = 0x1404A2BF0;
	constexpr uintptr_t SP_REGISTER_FONT = 0x140481F90;

	constexpr size_t OFF_CAM_ORIGIN = 0x80;  // 3 floats, found in your snapshots
	constexpr size_t OFF_CAM_PITCH = 0x10C;  // degrees, positive = looking down
	constexpr size_t OFF_CAM_YAW = 0x110;    // degrees
	constexpr size_t DVAR_CURRENT_OFFSET = 0x18;

	constexpr float EYE_HEIGHT = 60.0f;
	constexpr int MAX_QUADS = 9000;
	constexpr int MAX_BOXES = 64;
	constexpr int MAX_LABELS = 10;

	using com_frame_t = void (*)();
	using find_var_t = void* (*)(const char*);
	using register_t = void* (*)(const char*);
	using draw_stretch_t = void (*)(float, float, float, float, float, float, float, float, float*, void*);
	using draw_text_t = void (*)(const char*, int, void*, float, float, float, float, float, float*, int);

	struct vec3
	{
		float x, y, z;
	};

	struct hit
	{
		float dist;
		int slot;
		unsigned short class_id;
		unsigned char kind;
		bool inside;
		vec3 mid;
		vec3 half;
	};

	com_frame_t g_original_frame = nullptr;

	volatile bool g_overlay_on = false;   // starts OFF; press F8 in a mission to turn it on
	volatile bool g_text_on = false;
	volatile int g_mode = 0;       // bit 0: draw before the game's frame; bit 1: 640x480 coordinates
	volatile int g_dist_level = 1;
	volatile int g_fov_style = 1;
	const float kDistances[4] = {1500.0f, 4000.0f, 12000.0f, 1.0e9f};

	void* g_white = nullptr;
	void* g_font = nullptr;
	float g_font_height = 0.0f;
	void* g_last_white = nullptr;
	bool g_in_game = false;
	unsigned short g_player_class_id = 0;

	float g_fov = 65.0f;
	float g_fov_scale = 1.0f;
	int g_frame_counter = 0;
	int g_failures = 0;
	int g_quads = 0;
	int g_screen_w = 1920;
	int g_screen_h = 1080;
	HWND g_game_window = nullptr;

	// Camera for the current frame.
	vec3 g_eye;
	vec3 g_fwd;
	vec3 g_right;
	vec3 g_up;
	float g_tan_x = 1.0f;
	float g_tan_y = 1.0f;

	// What kind of thing each script-string id is (0 = unknown, 1 = ignore, 2.. = trigger kinds).
	unsigned char g_class_kind[65536];

	float kColors[6][4] = {
		{0.1f, 1.0f, 0.2f, 1.0f},   // 2: trigger_multiple (green)
		{1.0f, 0.2f, 0.2f, 1.0f},   // 3: hurt / damage (red)
		{1.0f, 0.9f, 0.1f, 1.0f},   // 4: use (yellow)
		{1.0f, 0.55f, 0.1f, 1.0f},  // 5: radius (orange)
		{0.2f, 0.8f, 1.0f, 1.0f},   // 6: everything else (cyan)
		{1.0f, 1.0f, 1.0f, 1.0f},   // inside this one (white)
	};

	BOOL CALLBACK find_window(HWND window, LPARAM parameter)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(window, &pid);
		if (pid == GetCurrentProcessId() && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr)
		{
			RECT r;
			if (GetClientRect(window, &r) && r.right > 320 && r.bottom > 240)
			{
				*reinterpret_cast<HWND*>(parameter) = window;
				return FALSE;
			}
		}

		return TRUE;
	}

	void update_screen_size()
	{
		if (!g_game_window || !IsWindow(g_game_window))
		{
			g_game_window = nullptr;
			EnumWindows(find_window, reinterpret_cast<LPARAM>(&g_game_window));
		}

		RECT r;
		if (g_game_window && GetClientRect(g_game_window, &r) && r.right > 0 && r.bottom > 0)
		{
			g_screen_w = r.right;
			g_screen_h = r.bottom;
		}
	}

	bool read_dvar_float(const char* name, float* out)
	{
		void* var = nullptr;
		__try
		{
			var = reinterpret_cast<find_var_t>(SP_DVAR_FIND_VAR)(name);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}

		if (!var)
		{
			return false;
		}

		float value = 0.0f;
		if (!read_bytes(reinterpret_cast<uintptr_t>(var) + DVAR_CURRENT_OFFSET, &value, sizeof(value)))
		{
			return false;
		}

		*out = value;
		return true;
	}

	void* try_register(const uintptr_t function, const char* name)
	{
		__try
		{
			return reinterpret_cast<register_t>(function)(name);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	void* make_font(float* height_out)
	{
		void* font = try_register(SP_REGISTER_FONT, "fonts/consolefont");
		if (!font)
		{
			return nullptr;
		}

		int height = 0;
		if (!read_bytes(reinterpret_cast<uintptr_t>(font) + 8, &height, sizeof(height)) || height < 4 || height > 400)
		{
			return nullptr;
		}

		*height_out = static_cast<float>(height);
		return font;
	}

	void rect(float x, float y, float w, float h, float* color)
	{
		if (g_quads >= MAX_QUADS || w <= 0.0f || h <= 0.0f)
		{
			return;
		}

		++g_quads;

		float sx = 1.0f;
		float sy = 1.0f;
		if ((g_mode & 2) != 0)
		{
			sx = 640.0f / static_cast<float>(g_screen_w);
			sy = 480.0f / static_cast<float>(g_screen_h);
		}

		reinterpret_cast<draw_stretch_t>(SP_DRAW_STRETCH_PIC)(x * sx, y * sy, w * sx, h * sy, 0.0f, 0.0f, 1.0f, 1.0f,
		                                                       color, g_white);
	}

	// Trims a line so it only covers the screen (Liang-Barsky).
	bool clip_line(float& x0, float& y0, float& x1, float& y1)
	{
		const float xmin = -30.0f, ymin = -30.0f;
		const float xmax = static_cast<float>(g_screen_w) + 30.0f;
		const float ymax = static_cast<float>(g_screen_h) + 30.0f;

		const float dx = x1 - x0;
		const float dy = y1 - y0;
		const float p[4] = {-dx, dx, -dy, dy};
		const float q[4] = {x0 - xmin, xmax - x0, y0 - ymin, ymax - y0};

		float t0 = 0.0f;
		float t1 = 1.0f;
		for (int i = 0; i < 4; ++i)
		{
			if (p[i] == 0.0f)
			{
				if (q[i] < 0.0f)
				{
					return false;
				}
			}
			else
			{
				const float t = q[i] / p[i];
				if (p[i] < 0.0f)
				{
					if (t > t1) return false;
					if (t > t0) t0 = t;
				}
				else
				{
					if (t < t0) return false;
					if (t < t1) t1 = t;
				}
			}
		}

		const float nx0 = x0 + t0 * dx;
		const float ny0 = y0 + t0 * dy;
		const float nx1 = x0 + t1 * dx;
		const float ny1 = y0 + t1 * dy;
		x0 = nx0;
		y0 = ny0;
		x1 = nx1;
		y1 = ny1;
		return true;
	}

	// A thin line made of a few flat rectangles (the only shape the game's draw call offers).
	void line(float x0, float y0, float x1, float y1, float* color, const float thick)
	{
		if (!clip_line(x0, y0, x1, y1))
		{
			return;
		}

		const float dx = x1 - x0;
		const float dy = y1 - y0;
		if (std::fabs(dx) >= std::fabs(dy))
		{
			int n = static_cast<int>(std::fabs(dy)) + 1;
			if (n > 1500) n = 1500;
			for (int i = 0; i < n; ++i)
			{
				const float t0 = static_cast<float>(i) / static_cast<float>(n);
				const float t1 = static_cast<float>(i + 1) / static_cast<float>(n);
				const float xa = x0 + dx * t0;
				const float xb = x0 + dx * t1;
				const float ym = y0 + dy * (t0 + t1) * 0.5f;
				rect((xa < xb ? xa : xb), ym - thick * 0.5f, std::fabs(xb - xa) + 1.0f, thick, color);
			}
		}
		else
		{
			int n = static_cast<int>(std::fabs(dx)) + 1;
			if (n > 1500) n = 1500;
			for (int i = 0; i < n; ++i)
			{
				const float t0 = static_cast<float>(i) / static_cast<float>(n);
				const float t1 = static_cast<float>(i + 1) / static_cast<float>(n);
				const float ya = y0 + dy * t0;
				const float yb = y0 + dy * t1;
				const float xm = x0 + dx * (t0 + t1) * 0.5f;
				rect(xm - thick * 0.5f, (ya < yb ? ya : yb), thick, std::fabs(yb - ya) + 1.0f, color);
			}
		}
	}

	vec3 to_camera(const vec3& p)
	{
		const vec3 d = {p.x - g_eye.x, p.y - g_eye.y, p.z - g_eye.z};
		vec3 c;
		c.x = d.x * g_right.x + d.y * g_right.y + d.z * g_right.z;
		c.y = d.x * g_up.x + d.y * g_up.y + d.z * g_up.z;
		c.z = d.x * g_fwd.x + d.y * g_fwd.y + d.z * g_fwd.z;
		return c;
	}

	void to_screen(const vec3& c, float* sx, float* sy)
	{
		const float w = static_cast<float>(g_screen_w);
		const float h = static_cast<float>(g_screen_h);
		*sx = w * 0.5f + (c.x / c.z) / g_tan_x * w * 0.5f;
		*sy = h * 0.5f - (c.y / c.z) / g_tan_y * h * 0.5f;
	}

	void edge(const vec3& a, const vec3& b, float* color)
	{
		vec3 ca = to_camera(a);
		vec3 cb = to_camera(b);
		const float near_plane = 4.0f;

		if (ca.z < near_plane && cb.z < near_plane)
		{
			return;
		}

		if (ca.z < near_plane)
		{
			const float t = (near_plane - ca.z) / (cb.z - ca.z);
			ca = {ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t, near_plane};
		}
		else if (cb.z < near_plane)
		{
			const float t = (near_plane - cb.z) / (ca.z - cb.z);
			cb = {cb.x + (ca.x - cb.x) * t, cb.y + (ca.y - cb.y) * t, near_plane};
		}

		float x0, y0, x1, y1;
		to_screen(ca, &x0, &y0);
		to_screen(cb, &x1, &y1);
		line(x0, y0, x1, y1, color, 2.0f);
	}

	void draw_box(const hit& h)
	{
		float* color = h.inside ? kColors[5] : kColors[h.kind > 1 && h.kind < 7 ? h.kind - 2 : 4];

		vec3 c[8];
		for (int i = 0; i < 8; ++i)
		{
			c[i].x = h.mid.x + ((i & 1) ? h.half.x : -h.half.x);
			c[i].y = h.mid.y + ((i & 2) ? h.half.y : -h.half.y);
			c[i].z = h.mid.z + ((i & 4) ? h.half.z : -h.half.z);
		}

		for (int i = 0; i < 8; ++i)
		{
			for (int bit = 1; bit <= 4; bit <<= 1)
			{
				if ((i & bit) == 0)
				{
					edge(c[i], c[i | bit], color);
				}
			}
		}
	}

	void draw_label(const hit& h)
	{
		if (!g_font)
		{
			return;
		}

		const vec3 c = to_camera(h.mid);
		if (c.z < 8.0f)
		{
			return;
		}

		float sx, sy;
		to_screen(c, &sx, &sy);
		if (sx < 0.0f || sy < 0.0f || sx > static_cast<float>(g_screen_w) || sy > static_cast<float>(g_screen_h))
		{
			return;
		}

		char name[48] = "?";
		class_name(h.class_id, name, sizeof(name));

		char text[96];
		sprintf_s(text, sizeof(text), "%s #%d (%.0f)", name, h.slot, h.dist);

		float scale = 16.0f / g_font_height;
		float x = sx;
		float y = sy;
		if ((g_mode & 2) != 0)
		{
			x *= 640.0f / static_cast<float>(g_screen_w);
			y *= 480.0f / static_cast<float>(g_screen_h);
			scale *= 480.0f / static_cast<float>(g_screen_h);
		}

		float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		reinterpret_cast<draw_text_t>(SP_DRAW_TEXT)(text, 0x7FFFFFFF, g_font, x, y, scale, scale, 0.0f, white, 0);
	}

	// 1 = ignore, 2.. = a kind of trigger. Looked up once and remembered for a while.
	unsigned char classify(const unsigned short id)
	{
		if (g_class_kind[id] != 0)
		{
			return g_class_kind[id];
		}

		char name[48];
		unsigned char kind = 1;
		if (class_name(id, name, sizeof(name)) && strncmp(name, "trigger_", 8) == 0)
		{
			if (strstr(name, "multiple")) kind = 2;
			else if (strstr(name, "hurt") || strstr(name, "damage")) kind = 3;
			else if (strstr(name, "use")) kind = 4;
			else if (strstr(name, "radius")) kind = 5;
			else kind = 6;
		}

		g_class_kind[id] = kind;
		return kind;
	}

	// Finds the closest triggers. Returns how many it put in out[].
	int find_triggers(const vec3& player, const float max_distance, hit* out, const int capacity)
	{
		int count = 0;
		unsigned char buffer[OFF_CLASSNAME + 2];

		for (int index = 0; index < MAX_ENTITIES; ++index)
		{
			bool ok = true;
			__try
			{
				memcpy(buffer, reinterpret_cast<const void*>(SP_G_ENTITIES + static_cast<uintptr_t>(index) * ENTITY_SIZE),
				       sizeof(buffer));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				ok = false;
			}

			if (!ok)
			{
				break;
			}

			unsigned short class_id = 0;
			memcpy(&class_id, buffer + OFF_CLASSNAME, sizeof(class_id));
			if (class_id == 0)
			{
				continue;
			}

			const unsigned char kind = classify(class_id);
			if (kind < 2)
			{
				continue;
			}

			float b[6];
			memcpy(b, buffer + OFF_BOUNDS, sizeof(b));
			bool sane = true;
			for (int i = 0; i < 6; ++i)
			{
				if (!(std::fabs(b[i]) < 200000.0f))
				{
					sane = false;
				}
			}

			if (!sane || b[3] <= 0.0f || b[4] <= 0.0f || b[5] <= 0.0f)
			{
				continue;
			}

			// Distance from the player to the surface of the box (0 when inside).
			const float ox = std::fabs(player.x - b[0]) - b[3];
			const float oy = std::fabs(player.y - b[1]) - b[4];
			const float oz = std::fabs(player.z - b[2]) - b[5];
			const float ex = ox > 0.0f ? ox : 0.0f;
			const float ey = oy > 0.0f ? oy : 0.0f;
			const float ez = oz > 0.0f ? oz : 0.0f;
			const float dist = std::sqrt(ex * ex + ey * ey + ez * ez);
			if (dist > max_distance)
			{
				continue;
			}

			hit h;
			h.dist = dist;
			h.slot = index;
			h.class_id = class_id;
			h.kind = kind;
			h.inside = ox <= 0.0f && oy <= 0.0f && oz <= 0.0f;
			h.mid = {b[0], b[1], b[2]};
			h.half = {b[3], b[4], b[5]};

			// Keep the list sorted, nearest first, and only the closest `capacity`.
			int position = count < capacity ? count : capacity - 1;
			if (count >= capacity && dist >= out[position].dist)
			{
				continue;
			}

			while (position > 0 && out[position - 1].dist > dist)
			{
				if (position < capacity)
				{
					out[position] = out[position - 1];
				}

				--position;
			}

			out[position] = h;
			if (count < capacity)
			{
				++count;
			}
		}

		return count;
	}

	bool key_pressed(const int key, bool* was_down)
	{
		const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
		const bool pressed = down && !*was_down;
		*was_down = down;
		return pressed;
	}

	void draw_indicator(const int row, const int count, float* color)
	{
		for (int i = 0; i <= count; ++i)
		{
			rect(10.0f + static_cast<float>(i) * 18.0f, 10.0f + static_cast<float>(row) * 18.0f, 12.0f, 12.0f, color);
		}
	}

	// True only while a mission is running (slot 0 of the entity list is the player).
	bool in_mission()
	{
		unsigned short id = 0;
		if (!read_bytes(SP_G_ENTITIES + OFF_CLASSNAME, &id, sizeof(id)) || id == 0)
		{
			g_in_game = false;
			return false;
		}

		if (g_frame_counter % 60 == 1 || id != g_player_class_id)
		{
			char name[32];
			g_in_game = class_name(id, name, sizeof(name)) && strcmp(name, "player") == 0;
			g_player_class_id = g_in_game ? id : 0;
		}

		return g_in_game;
	}

	void frame_draw()
	{
		static bool was_f4, was_f5, was_f6, was_f7, was_f8;
		const bool f4 = key_pressed(VK_F4, &was_f4);
		const bool f5 = key_pressed(VK_F5, &was_f5);
		const bool f6 = key_pressed(VK_F6, &was_f6);
		const bool f7 = key_pressed(VK_F7, &was_f7);
		const bool f8 = key_pressed(VK_F8, &was_f8);

		if (f8)
		{
			g_overlay_on = !g_overlay_on;
			log_line("F8: overlay %s", g_overlay_on ? "ON" : "OFF");
		}

		if (f4) g_text_on = !g_text_on;
		if (f5) g_fov_style = (g_fov_style + 1) % 3;
		if (f6) g_mode = (g_mode + 1) % 4;
		if (f7) g_dist_level = (g_dist_level + 1) % 4;

		++g_frame_counter;
		if (g_frame_counter == 1)
		{
			log_line("drawing hook is running (first frame)");
		}

		if (!g_overlay_on || !in_mission())
		{
			return;
		}

		// Where am I and where am I looking?
		unsigned long long pointer = 0;
		if (!read_bytes(SP_G_ENTITIES + OFF_CLIENT, &pointer, sizeof(pointer)) || pointer < 0x140000000ull ||
			pointer > 0x7FFFFFFE0000ull)
		{
			return;
		}

		float origin[3];
		float pitch = 0.0f;
		float yaw = 0.0f;
		if (!read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_ORIGIN, origin, sizeof(origin)) ||
			!read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_PITCH, &pitch, sizeof(pitch)) ||
			!read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_YAW, &yaw, sizeof(yaw)))
		{
			return;
		}

		if (!(std::fabs(origin[0]) < 500000.0f && std::fabs(origin[1]) < 500000.0f && std::fabs(origin[2]) < 500000.0f &&
			std::fabs(pitch) <= 91.0f && std::fabs(yaw) <= 720.0f))
		{
			return;
		}

		// The game moves and replaces its pictures and fonts when a mission loads, so a
		// remembered pointer goes stale (that is what crashed 0.6). Ask again every frame.
		g_white = try_register(SP_MATERIAL_REGISTER, "white");
		if (g_white != g_last_white)
		{
			log_line("white material is now %p (was %p)", g_white, g_last_white);
			g_last_white = g_white;
		}

		if (!g_white)
		{
			return;
		}

		g_font = nullptr;
		if (g_text_on)
		{
			g_font = make_font(&g_font_height);
		}

		if (g_frame_counter % 120 == 1)
		{
			read_dvar_float("cg_fov", &g_fov);
			read_dvar_float("cg_fovScale", &g_fov_scale);
			if (!(g_fov >= 20.0f && g_fov <= 160.0f)) g_fov = 65.0f;
			if (!(g_fov_scale >= 0.2f && g_fov_scale <= 3.0f)) g_fov_scale = 1.0f;
			update_screen_size();
		}

		if (g_frame_counter % 600 == 0)
		{
			memset(g_class_kind, 0, sizeof(g_class_kind));
		}

		// Camera vectors.
		const float rad = 3.14159265f / 180.0f;
		const float cp = std::cos(pitch * rad);
		const float sp = std::sin(pitch * rad);
		const float cy = std::cos(yaw * rad);
		const float sy = std::sin(yaw * rad);
		g_eye = {origin[0], origin[1], origin[2] + EYE_HEIGHT};
		g_fwd = {cp * cy, cp * sy, -sp};
		g_right = {sy, -cy, 0.0f};
		g_up = {sp * cy, sp * sy, cp};

		const float aspect = static_cast<float>(g_screen_w) / static_cast<float>(g_screen_h);
		const float half_fov = g_fov * g_fov_scale * 0.5f * rad;
		if (g_fov_style == 0)
		{
			g_tan_y = std::tan(half_fov) * 0.75f;
			g_tan_x = g_tan_y * aspect;
		}
		else if (g_fov_style == 1)
		{
			g_tan_x = std::tan(half_fov);
			g_tan_y = g_tan_x / aspect;
		}
		else
		{
			g_tan_y = std::tan(half_fov);
			g_tan_x = g_tan_y * aspect;
		}

		g_quads = 0;
		draw_indicator(0, g_mode, kColors[4]);
		draw_indicator(1, g_dist_level, kColors[2]);
		draw_indicator(2, g_fov_style, kColors[3]);

		hit hits[MAX_BOXES];
		const int count = find_triggers({origin[0], origin[1], origin[2] + 30.0f}, kDistances[g_dist_level], hits, MAX_BOXES);

		for (int i = count - 1; i >= 0; --i)
		{
			draw_box(hits[i]);
		}

		if (g_text_on && g_font)
		{
			for (int i = 0; i < count && i < MAX_LABELS; ++i)
			{
				draw_label(hits[i]);
			}
		}

		if (f4 || f5 || f6 || f7)
		{
			log_line("settings: mode=%d dist=%d fovstyle=%d text=%d | screen %dx%d fov %.1f x %.2f | eye %.0f %.0f %.0f pitch %.1f yaw %.1f | %d triggers near, %d rects",
			         static_cast<int>(g_mode), static_cast<int>(g_dist_level), static_cast<int>(g_fov_style),
			         g_text_on ? 1 : 0, g_screen_w, g_screen_h, g_fov, g_fov_scale, g_eye.x, g_eye.y, g_eye.z, pitch,
			         yaw, count, g_quads);
		}
	}

	void safe_frame_draw()
	{
		__try
		{
			frame_draw();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			++g_failures;
			log_line("drawing hit an error (%d of 5)", g_failures);
			if (g_failures >= 5)
			{
				g_overlay_on = false;
				log_line("too many errors: overlay switched itself off");
			}
		}
	}

	void hooked_frame()
	{
		const bool before = (g_mode & 1) != 0;
		if (before)
		{
			safe_frame_draw();
		}

		g_original_frame();

		if (!before)
		{
			safe_frame_draw();
		}
	}

	bool install_hook()
	{
		unsigned char probe[16];
		if (!read_bytes(SP_COM_FRAME, probe, sizeof(probe)))
		{
			log_line("hook: game function is not where expected, not installing");
			return false;
		}

		if (MH_Initialize() != MH_OK)
		{
			log_line("hook: MinHook failed to start");
			return false;
		}

		if (MH_CreateHook(reinterpret_cast<LPVOID>(SP_COM_FRAME), reinterpret_cast<LPVOID>(&hooked_frame),
		                  reinterpret_cast<LPVOID*>(&g_original_frame)) != MH_OK)
		{
			log_line("hook: could not create hook");
			return false;
		}

		if (MH_EnableHook(reinterpret_cast<LPVOID>(SP_COM_FRAME)) != MH_OK)
		{
			log_line("hook: could not enable hook");
			return false;
		}

		log_line("hook installed");
		return true;
	}

	bool is_campaign_exe()
	{
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exe, MAX_PATH);
		for (char* c = exe; *c; ++c)
		{
			if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c + 32);
		}

		return strstr(exe, "s1_sp64_ship") != nullptr;
	}


	DWORD WINAPI worker(LPVOID)
	{
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exe, MAX_PATH);

		log_line("---- add-on loaded (version 0.7, first drawing build, fix 1) ----");
		log_line("host program: %s", exe);
		log_line("F9 = save entity table | F8 on/off | F6 mode | F7 distance | F5 fov style | F4 text");

		bool hook_done = false;

		int tables = 0;
		int seconds_with_data = 0;
		bool auto_done = false;
		bool f9_was_down = false;

		// 100 ms per tick; stop after about 30 minutes.
		for (int tick = 1; tick <= 18000; ++tick)
		{
			Sleep(100);

			// Wait until the game has had time to start up before touching it.
			if (!hook_done && tick >= 150)
			{
				hook_done = true;
				if (is_campaign_exe())
				{
					install_hook();
				}
				else
				{
					log_line("not the campaign program, drawing not installed");
				}
			}

			if (tick % 10 == 0)
			{
				seconds_with_data = entity_list_has_data() ? seconds_with_data + 1 : 0;
			}

			const bool f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
			const bool f9_pressed = f9_down && !f9_was_down;
			f9_was_down = f9_down;

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
				write_client(tables);
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

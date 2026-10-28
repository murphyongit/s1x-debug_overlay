// S1x debug overlay - version 0.10 (Direct3D overlay, smoothed camera)
//
// Draws a box around every trigger in the campaign. Keys:
//   F8 = overlay on/off (starts OFF)     F7 = how far away to draw
//   F6 = line thickness                  F5 = field-of-view style
//   F9 = save the data files (as before)
// Small squares in the top-left corner show the current F7 / F5 / F6 settings.

#include "core.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#include "MinHook.h"

#ifndef VK_NUMPAD1
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#endif
#ifndef VK_NUMPAD5
#define VK_NUMPAD0 0x60
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_DIVIDE 0x6F
#define VK_DECIMAL 0x6E
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#endif

namespace
{
	// Addresses for the campaign (single-player), from the S1x source files.
	// (Names say SP, but they are filled in for multiplayer too by choose_game_layout().)
	uintptr_t SP_G_ENTITIES = 0x143C26DC0;
	uintptr_t SP_SL_CONVERT_TO_STRING = 0x140314850;
	bool g_is_mp = false;

	// Entity layout worked out from your dump files.
	constexpr size_t ENTITY_SIZE = 0x2F8;   // largest size, used for buffers
	size_t g_stride = 0x2F8;                // real distance between entities
	constexpr int MAX_ENTITIES = 10240;   // the real table is longer than 2048
	size_t OFF_NUMBER = 0x7C;   // 16-bit entity number
	size_t OFF_ORIGIN = 0x1C;   // 3 floats
	size_t OFF_BOUNDS = 0xDC;   // 6 floats: midpoint xyz, half-size xyz
	size_t OFF_CLASSNAME = 0x14C; // 16-bit script string id
	size_t OFF_TYPE = 0x150;                // script string id of the type name (u32)
	size_t OFF_NAME = 0x160;                // script string id of the targetname (u32)
	int g_player_slot = 0;                  // the local player entity
	size_t OFF_CLIENT = 0x118;    // 64-bit pointer to the player's client block

	// In multiplayer the bounds are stored relative to the entity position; turn them into world positions.
	void make_world_bounds(float* b, const unsigned char* entity)
	{
		if (g_is_mp)
		{
			float origin[3];
			memcpy(origin, entity + OFF_ORIGIN, sizeof(origin));
			b[0] += origin[0];
			b[1] += origin[1];
			b[2] += origin[2];
		}
	}

	uintptr_t player_entity()
	{
		return SP_G_ENTITIES + static_cast<uintptr_t>(g_player_slot) * g_stride;
	}
	constexpr size_t CLIENT_DUMP_SIZE = 0x1000;

	#ifdef RECORDER_ONLY
	#define ADDON_VERSION "0.70 RECORDER"
	#else
	#define ADDON_VERSION "0.70"
	#endif
	constexpr int MAX_TABLES = 6;

	volatile LONG g_started = 0;
	HMODULE g_self = nullptr;
	char g_dir[MAX_PATH] = {};
	char g_log_path[MAX_PATH] = {};

	thread_local const char* g_subdir = nullptr;   // set by the recorder so its files land in speedrun_data

	void make_path(char* out, const char* file_name)
	{
		strcpy_s(out, MAX_PATH, g_dir);
		if (g_subdir)
		{
			strcat_s(out, MAX_PATH, g_subdir);
		}

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
		// No VirtualQuery here: asking Windows about the memory first took ~19 ms per read while the game was
		// streaming, which stalled everything. Just read it and catch a failure.
		if (address < 0x10000)
		{
			return false;
		}

		__try
		{
			memcpy(out, reinterpret_cast<const void*>(address), count);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
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

	double now_seconds()
	{
		LARGE_INTEGER frequency, counter;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&counter);
		return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
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
			const uintptr_t address = SP_G_ENTITIES + static_cast<uintptr_t>(index) * g_stride;
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
			make_world_bounds(bounds, entity);

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


	// ---- multiplayer: raw data about the local player, so the camera can be worked out ----------
	void write_mp_player_debug(const int number)
	{
		using ps_t = unsigned char* (*)(int);
		unsigned char* ps = nullptr;
		__try
		{
			ps = reinterpret_cast<ps_t>(0x14043E260)(0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ps = nullptr;
		}

		char name[64];
		sprintf_s(name, sizeof(name), "mp_player_%d.txt", number);
		char path[MAX_PATH];
		make_path(path, name);
		FILE* f = nullptr;
		if (fopen_s(&f, path, "wb") != 0 || !f)
		{
			return;
		}

		fprintf(f, "player slot %d, class offset 0x%zX, origin offset 0x%zX\n", g_player_slot, OFF_CLASSNAME, OFF_ORIGIN);
		fprintf(f, "playerState pointer: 0x%llX\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ps)));
		auto dump_block = [&](const char* title, uintptr_t address, size_t size)
		{
			fprintf(f, "\n== %s (0x%llX, 0x%zX bytes)\n", title, static_cast<unsigned long long>(address), size);
			for (size_t row = 0; row < size; row += 16)
			{
				unsigned char line[16];
				if (!read_bytes(address + row, line, sizeof(line)))
				{
					fprintf(f, "%04zX: ??\n", row);
					continue;
				}

				float v[4];
				memcpy(v, line, sizeof(v));
				fprintf(f, "%04zX:", row);
				for (const unsigned char b : line) fprintf(f, " %02X", b);
				fprintf(f, "   %.3f %.3f %.3f %.3f\n", v[0], v[1], v[2], v[3]);
			}
		};

		if (ps)
		{
			dump_block("playerState of client 0", reinterpret_cast<uintptr_t>(ps), 0x700);
		}

		unsigned long long client = 0;
		if (read_bytes(player_entity() + OFF_CLIENT, &client, sizeof(client)) && client > 0x140000000ull)
		{
			dump_block("client block from the entity", static_cast<uintptr_t>(client), 0x400);
		}

		for (int slot = 0; slot < 3; ++slot)
		{
			char title[48];
			sprintf_s(title, sizeof(title), "entity %d", slot);
			dump_block(title, SP_G_ENTITIES + static_cast<uintptr_t>(slot) * g_stride, 0x300);
		}

		fclose(f);
		log_line("wrote %s (playerState pointer 0x%llX)", name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ps)));
	}

	// Saves the first part of the player's own data block (position, view angles, ...).
	void write_client(const int number)
	{
		unsigned long long pointer = 0;
		if (!read_bytes(player_entity() + OFF_CLIENT, &pointer, sizeof(pointer)))
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
	// Drawing: we paint our own lines on top of the finished picture, using the graphics
	// system (Direct3D 11) directly, just before the picture is shown on screen.
	// ------------------------------------------------------------------------------
	uintptr_t SP_DVAR_FIND_VAR = 0x140370860;

	size_t OFF_CAM_ORIGIN = 0x80;  // 3 floats, found in your snapshots
	size_t OFF_CAM_PITCH = 0x10C;  // degrees, positive = looking down
	size_t OFF_CAM_YAW = 0x110;    // degrees
	constexpr size_t DVAR_CURRENT_OFFSET = 0x18;

	constexpr float EYE_HEIGHT = 60.0f;
	constexpr int MAX_BOXES = 96;
	constexpr int MAX_VERTS = 60000;

	using find_var_t = void* (*)(const char*);

	struct vec3
	{
		float x, y, z;
	};

	struct hit
	{
		float dist;
		int slot;
		unsigned short class_id;
		unsigned int name_id;
		unsigned int type_id;
		unsigned int state;      // the entity's "contents" word (changes when walls turn solid / not solid)
		float key;               // sort order (end-of-mission candidates sort first)
		unsigned char flag;      // 0 = normal, 1 = might end the mission / leave the area
		unsigned char kind;
		bool barrier;
		bool disabled;   // a leave-area box that is switched off right now (drawn dimmer)
		bool inside;
		vec3 mid;
		vec3 half;
	};

	struct vertex
	{
		float x, y;
		float r, g, b, a;
	};

	volatile bool g_overlay_on = false;   // starts OFF; press F8 in a mission to turn it on
	volatile int g_dist_level = 1;
	volatile int g_fov_style = 0;   // 0 was confirmed to line up correctly
	volatile int g_thick_level = 1;
	volatile bool g_log_settings = false;
	const float kDistances[4] = {1500.0f, 4000.0f, 12000.0f, 1.0e9f};
	const float kThickness[3] = {1.5f, 2.5f, 4.0f};

	float g_fov = 65.0f;
	int g_frame_counter = 0;
	int g_failures = 0;
	bool g_in_game = false;
	unsigned short g_player_class_id = 0;
	bool g_in_present = false;

	vertex g_verts[MAX_VERTS];
	int g_vert_count = 0;
	float g_w = 1920.0f;
	float g_h = 1080.0f;

	vec3 g_eye;
	vec3 g_fwd;
	vec3 g_right;
	vec3 g_up;
	float g_tan_x = 1.0f;
	float g_tan_y = 1.0f;

	unsigned char g_class_kind[65536];

	float kColors[11][4] = {
		{0.1f, 1.0f, 0.2f, 1.0f},   // trigger_multiple (green)
		{1.0f, 0.2f, 0.2f, 1.0f},   // hurt / damage (red)
		{1.0f, 0.9f, 0.1f, 1.0f},   // use (yellow)
		{1.0f, 0.55f, 0.1f, 1.0f},  // radius (orange)
		{0.2f, 0.8f, 1.0f, 1.0f},   // everything else (blue)
		{1.0f, 1.0f, 1.0f, 1.0f},   // you are inside this one (white)
		{1.0f, 0.3f, 1.0f, 1.0f},   // sets or clears a level flag (magenta)
		{0.2f, 1.0f, 1.0f, 1.0f},   // checkpoint / autosave (cyan)
		{0.65f, 0.65f, 0.75f, 1.0f},  // moving brush models and volumes (grey)
		{0.95f, 0.6f, 0.35f, 0.9f},   // level collision blocks (warm brown)
		{1.0f, 0.25f, 0.65f, 1.0f},   // invisible barrier brush (hot pink)
	};

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

		// Make sure this really is the dvar we asked for (its name is the first field).
		unsigned long long name_pointer = 0;
		char found_name[40] = {};
		if (!read_bytes(reinterpret_cast<uintptr_t>(var), &name_pointer, sizeof(name_pointer)) ||
			!safe_copy_string(reinterpret_cast<const char*>(name_pointer), found_name, sizeof(found_name)) ||
			_stricmp(found_name, name) != 0)
		{
			log_line("dvar %s: layout does not look right (name field reads '%s'), using default", name, found_name);
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

	// ---- building the picture (all in screen pixels) --------------------------------

	void push_vertex(const float px, const float py, const float* color)
	{
		vertex& v = g_verts[g_vert_count++];
		v.x = px / g_w * 2.0f - 1.0f;
		v.y = 1.0f - py / g_h * 2.0f;
		v.r = color[0];
		v.g = color[1];
		v.b = color[2];
		v.a = color[3];
	}

	void add_quad(const float x0, const float y0, const float x1, const float y1, const float x2, const float y2,
	              const float x3, const float y3, const float* color)
	{
		if (g_vert_count + 6 > MAX_VERTS)
		{
			return;
		}

		push_vertex(x0, y0, color);
		push_vertex(x1, y1, color);
		push_vertex(x2, y2, color);
		push_vertex(x0, y0, color);
		push_vertex(x2, y2, color);
		push_vertex(x3, y3, color);
	}

	void add_rect(const float x, const float y, const float w, const float h, const float* color)
	{
		add_quad(x, y, x + w, y, x + w, y + h, x, y + h, color);
	}

	// Trims a line so it only covers the screen (Liang-Barsky).
	bool clip_line(float& x0, float& y0, float& x1, float& y1)
	{
		const float xmin = -40.0f, ymin = -40.0f;
		const float xmax = g_w + 40.0f;
		const float ymax = g_h + 40.0f;

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

	void add_line(float x0, float y0, float x1, float y1, const float* color, const float thick)
	{
		if (!clip_line(x0, y0, x1, y1))
		{
			return;
		}

		const float dx = x1 - x0;
		const float dy = y1 - y0;
		const float length = std::sqrt(dx * dx + dy * dy);
		if (length < 0.01f)
		{
			return;
		}

		const float nx = -dy / length * thick * 0.5f;
		const float ny = dx / length * thick * 0.5f;
		add_quad(x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 - nx, y1 - ny, x1 + nx, y1 + ny, color);
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
		*sx = g_w * 0.5f + (c.x / c.z) / g_tan_x * g_w * 0.5f;
		*sy = g_h * 0.5f - (c.y / c.z) / g_tan_y * g_h * 0.5f;
	}

	bool g_thin_lines = false;
	float g_line_override = 0.0f;

	void edge(const vec3& a, const vec3& b, const float* color)
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
		add_line(x0, y0, x1, y1, color, g_thin_lines ? 1.0f : (g_line_override > 0.0f ? g_line_override : kThickness[g_thick_level]));
	}

	// 1 = ordinary, 2 = sets/clears a level flag, 3 = checkpoint. Looked up once per name.
	unsigned char g_type_special[65536];

	unsigned char type_special(const unsigned int type_id)
	{
		if (type_id < 2 || type_id >= 0x10000)
		{
			return 1;
		}

		if (g_type_special[type_id] != 0)
		{
			return g_type_special[type_id];
		}

		char name[64];
		unsigned char special = 1;
		if (class_name(type_id, name, sizeof(name)))
		{
			if (strstr(name, "flag")) special = 2;
			else if (strstr(name, "autosave") || strstr(name, "checkpoint")) special = 3;
		}

		g_type_special[type_id] = special;
		return special;
	}

	const float* color_for(const hit& h)
	{
		if (h.flag == 1)
		{
			// Slowly drifting rainbow so these stand out from everything else.
			static float rainbow[4] = {1.0f, 0.0f, 0.0f, 1.0f};
			const float hue = static_cast<float>(std::fmod(now_seconds() * 0.08, 1.0)) * 6.0f;
			const int sector = static_cast<int>(hue);
			const float f = hue - static_cast<float>(sector);
			const float q = 1.0f - f;
			switch (sector % 6)
			{
			case 0: rainbow[0] = 1.0f; rainbow[1] = f; rainbow[2] = 0.0f; break;
			case 1: rainbow[0] = q; rainbow[1] = 1.0f; rainbow[2] = 0.0f; break;
			case 2: rainbow[0] = 0.0f; rainbow[1] = 1.0f; rainbow[2] = f; break;
			case 3: rainbow[0] = 0.0f; rainbow[1] = q; rainbow[2] = 1.0f; break;
			case 4: rainbow[0] = f; rainbow[1] = 0.0f; rainbow[2] = 1.0f; break;
			default: rainbow[0] = 1.0f; rainbow[1] = 0.0f; rainbow[2] = q; break;
			}

			if (h.disabled)
			{
				static float dim[4];
				for (int i = 0; i < 3; ++i) dim[i] = rainbow[i] * 0.55f;
				dim[3] = 1.0f;
				return dim;
			}

			return rainbow;
		}

		if (h.inside) return kColors[5];
		if (h.kind == 7 && h.barrier) return kColors[10];
		if (h.kind == 7) return kColors[8];
		if (h.kind == 9) return kColors[9];
		const unsigned char special = type_special(h.type_id);
		if (special == 2) return kColors[6];
		if (special == 3) return kColors[7];
		return kColors[h.kind > 1 && h.kind < 7 ? h.kind - 2 : 4];
	}

	void draw_box(const hit& h)
	{
		const float* color = color_for(h);
		g_line_override = h.flag == 1 ? 6.0f : 0.0f;

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

		g_line_override = 0.0f;
	}

	// ---- tiny built-in text (5x7 letters drawn as little rectangles) ------------------------
	volatile bool g_labels_on = true;   // F2
	volatile bool g_brushes_on = false;   // Home
#ifndef VK_OEM_4
#define VK_OEM_4 0xDB
#endif
#ifndef VK_OEM_6
#define VK_OEM_6 0xDD
#endif
	volatile int g_show_mode = 0;         // [ : 0 all, 1 green only, 2 pink walls only, 3 magenta flag setters only, 4 leave-area only
	volatile bool g_leave_on = true;      // ] : leaving-area / mission-fail boxes (rainbow) shown
	const char* const kShowModes[] = {"ALL", "GREEN ONLY", "PINK WALLS ONLY", "MAGENTA FLAG TRIGGERS ONLY", "LEAVE-AREA ONLY"};
	double g_help_until = 0.0;            // keys help is shown until this time (End key toggles)
	volatile bool g_help_requested = false;   // set by the console command

	const unsigned char* glyph_for(const char c)
	{
		static const unsigned char kLetters[26][7] = {
			{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
			{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
			{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
			{0x07,0x02,0x02,0x02,0x02,0x12,0x0C},{0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
			{0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x19,0x15,0x13,0x11,0x11,0x11},{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
			{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
			{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
			{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},{0x11,0x11,0x11,0x15,0x15,0x1B,0x11},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
			{0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}};
		static const unsigned char kDigits[10][7] = {
			{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
			{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
			{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
			{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}};
		static const unsigned char kUnderscore[7] = {0,0,0,0,0,0,0x1F};
		static const unsigned char kHash[7] = {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A};
		static const unsigned char kDash[7] = {0,0,0,0x1F,0,0,0};
		static const unsigned char kDot[7] = {0,0,0,0,0,0x0C,0x0C};
		static const unsigned char kBlank[7] = {0,0,0,0,0,0,0};

		if (c >= 'a' && c <= 'z') return kLetters[c - 'a'];
		if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
		if (c >= '0' && c <= '9') return kDigits[c - '0'];
		if (c == '_') return kUnderscore;
		if (c == '#') return kHash;
		if (c == '-') return kDash;
		if (c == '.') return kDot;
		return kBlank;
	}

	// Draws text with its top-left corner at x,y. Each letter is 5x7 dots, each dot is `scale` pixels.
	void draw_text(float x, const float y, const char* text, const float scale, const float* color)
	{
		for (const char* p = text; *p; ++p)
		{
			const unsigned char* g = glyph_for(*p);
			for (int row = 0; row < 7; ++row)
			{
				int col = 0;
				while (col < 5)
				{
					if (!(g[row] & (0x10 >> col)))
					{
						++col;
						continue;
					}

					int end = col;
					while (end < 5 && (g[row] & (0x10 >> end)))
					{
						++end;
					}

					add_rect(x + static_cast<float>(col) * scale, y + static_cast<float>(row) * scale,
					         static_cast<float>(end - col) * scale, scale, color);
					col = end;
				}
			}

			x += 6.0f * scale;
		}
	}

	bool learned_flags(int slot, char* out, size_t capacity);

	void draw_label(const hit& h, const float* color)
	{
		const vec3 c = to_camera(h.mid);
		float sx, sy;
		bool on_screen = c.z >= 8.0f;
		if (on_screen)
		{
			to_screen(c, &sx, &sy);
			on_screen = sx >= 0.0f && sy >= 0.0f && sx <= g_w && sy <= g_h;
		}

		if (!on_screen)
		{
			return;
		}

		char name[48];
		if (!class_name(h.class_id, name, sizeof(name)))
		{
			return;
		}

		// The more specific type (such as trigger_multiple_flag_set) is stored next to the class.
		char type_text[64] = {};
		if (h.type_id > 1 && h.type_id < 0x10000 && class_name(h.type_id, type_text, sizeof(type_text)))
		{
			for (const char* ch = type_text; *ch; ++ch)
			{
				if (*ch < 32 || *ch > 126)
				{
					type_text[0] = '\0';
					break;
				}
			}
		}

		const char* source = type_text[0] ? type_text : name;
		const char* shown = strncmp(source, "trigger_", 8) == 0 ? source + 8 : source;
		char text[160];
		char given[40] = {};
		if (h.name_id > 1 && h.name_id < 0x10000 && class_name(h.name_id, given, sizeof(given)))
		{
			for (const char* ch = given; *ch; ++ch)
			{
				if (*ch < 32 || *ch > 126)
				{
					given[0] = '\0';
					break;
				}
			}
		}

		if (h.kind == 7)
		{
			snprintf(text, sizeof(text), "%s - %s #%d %dm c=%X", given[0] ? given : "-", shown, h.slot,
			         static_cast<int>(h.dist / 39.37f), h.state);
		}
		else if (given[0])
		{
			snprintf(text, sizeof(text), "%s - %s #%d %dm", given, shown, h.slot, static_cast<int>(h.dist / 39.37f));
		}
		else
		{
			snprintf(text, sizeof(text), "%s #%d %dm", shown, h.slot, static_cast<int>(h.dist / 39.37f));
		}

		char learned[100];
		if (learned_flags(h.slot, learned, sizeof(learned)))
		{
			const size_t used = strlen(text);
			snprintf(text + used, sizeof(text) - used, " -> %s", learned);
		}

		const float scale = g_h >= 1200.0f ? 2.0f : 1.0f;

		const float width = static_cast<float>(strlen(text)) * 6.0f * scale;
		const float height = 7.0f * scale;
		float x = sx - width * 0.5f;
		float y = sy - height * 0.5f;
		if (x < 4.0f) x = 4.0f;
		if (x + width > g_w - 4.0f) x = g_w - 4.0f - width;
		if (y < 4.0f) y = 4.0f;
		if (y + height > g_h - 4.0f) y = g_h - 4.0f - height;
		const float back[4] = {0.0f, 0.0f, 0.0f, 0.65f};
		add_rect(x - 3.0f, y - 3.0f, width + 5.0f, height + 6.0f, back);
		draw_text(x, y, text, scale, color);
	}

	// ---- script events (what the map scripts are being told) -------------------------------
	// Hooks the game's "notify" function. Every time a script sends a message such as "trigger" or
	// "death", we remember its name and show the newest ones in the top-right corner.
	uintptr_t SP_SCR_NOTIFY_ID = 0x14031CB80;
	constexpr int EVENT_SLOTS = 16;

	struct script_event
	{
		char name[44];
		unsigned int entity;
		int count;
		double time;
	};

	script_event g_events[EVENT_SLOTS];
	volatile int g_event_next = 0;
	volatile bool g_events_on = true;   // F11
	int g_events_logged = 0;
	using notify_t = void (*)(unsigned int, unsigned int, unsigned int);
	notify_t g_original_notify = nullptr;

	// Messages that fire constantly for every character/vehicle and drown out everything else.
	bool is_always_noise(const char* name)
	{
		// Names that always show, no matter how often they are sent.
		static const char* const kImportant[] = {"chyron", "flag", "obj", "mission", "checkpoint", "spawned", "wave",
		                                        "reinforce", "level_", "start_", "trigger_", "explode", "complete",
		                                        "cinematic", "cutscene", "dialogue_", "alarm"};
		for (const char* word : kImportant)
		{
			if (strstr(name, word))
			{
				return false;
			}
		}

		static const char* const kNoise[] = {
			"goal", "touch", "reached_end_node", "turret_", "entitydeleted", "deathanim", "damage", "bulletwhizby",
			"weapon", "sprint_", "path_", "ai_event", "animscript", "anim_", "bad_path", "got_path", "jump", "reload",
			"foley", "sounddone", "movemode", "known_event", "enemy", "cover", "groundEnt", "killanimscript",
			"node_", "begin_", "end_firing", "movedone", "turn", "blendTransition", "traverse", "bullethit", "pain",
			"physics_impact", "suppression", "BUTTON_", "dpad_", "start_move", "stepout", "looking_", "missile_",
			"projectile_", "code_", "boost", "exo_", "single ", "looping ", "run", "walk", "move", "fire", "react",
			"melee", "charge", "stop_", "reload", "death"};
		for (const char* prefix : kNoise)
		{
			if (strncmp(name, prefix, strlen(prefix)) == 0)
			{
				return true;
			}
		}

		// Hidden when the name contains any of these anywhere (animations, idles, button presses).
		static const char* const kContains[] = {"anim", "idle", "pressed", "released", "_loop", "look_at", "lookat",
		                                        "footstep", "stance"};
		for (const char* part : kContains)
		{
			if (strstr(name, part))
			{
				return true;
			}
		}

		return false;
	}

	// Anything that is sent more than 6 times in one second also gets hidden automatically.
	struct rate_entry
	{
		char name[44];
		double window_start;
		int window_count;
		double slow_start;
		int slow_count;
		double last_seen;
		bool muted;
	};

	rate_entry g_rates[256];
	int g_rate_entries = 0;

	bool is_too_frequent(const char* name, const double now)
	{
		rate_entry* entry = nullptr;
		for (int i = 0; i < g_rate_entries; ++i)
		{
			if (strcmp(g_rates[i].name, name) == 0)
			{
				entry = &g_rates[i];
				break;
			}
		}

		if (!entry)
		{
			if (g_rate_entries >= 256)
			{
				return false;
			}

			entry = &g_rates[g_rate_entries++];
			strcpy_s(entry->name, sizeof(entry->name), name);
			entry->window_start = now;
			entry->window_count = 0;
			entry->slow_start = now;
			entry->slow_count = 0;
			entry->last_seen = now;
			entry->muted = false;
		}

		// Hidden until it has been quiet for 15 seconds.
		if (entry->muted)
		{
			if (now - entry->last_seen > 15.0)
			{
				entry->muted = false;
				entry->window_start = entry->slow_start = now;
				entry->window_count = entry->slow_count = 0;
			}
			else
			{
				entry->last_seen = now;
				return true;
			}
		}

		entry->last_seen = now;
		if (now - entry->slow_start > 10.0)
		{
			entry->slow_start = now;
			entry->slow_count = 0;
		}

		if (now - entry->window_start > 1.0)
		{
			entry->window_start = now;
			entry->window_count = 0;
		}

		++entry->slow_count;
		if (++entry->window_count > 6 || entry->slow_count > 4)
		{
			entry->muted = true;
			log_line("event: '%s' is sent too often, hiding it", name);
			return true;
		}

		return false;
	}

	// Every message ever seen (even hidden ones), written to events_summary.txt on F9.
	struct event_stat
	{
		char name[44];
		int count;
		unsigned int first_entity;
		double first_time;
	};

	event_stat g_stats[512];
	int g_stat_count = 0;
	volatile bool g_filter_on = true;   // F4

	void count_event(const char* name, const unsigned int entity, const double now)
	{
		for (int i = 0; i < g_stat_count; ++i)
		{
			if (strcmp(g_stats[i].name, name) == 0)
			{
				++g_stats[i].count;
				return;
			}
		}

		if (g_stat_count < 512)
		{
			event_stat& st = g_stats[g_stat_count++];
			strcpy_s(st.name, sizeof(st.name), name);
			st.count = 1;
			st.first_entity = entity;
			st.first_time = now;
		}
	}

	unsigned char classify(const unsigned short id);

	void write_trigger_names()
	{
		char path[MAX_PATH];
		make_path(path, "trigger_names.txt");
		FILE* f = nullptr;
		if (fopen_s(&f, path, "w") != 0 || !f)
		{
			return;
		}

		fprintf(f, "slot class | text found at entity offsets 150 154 158 15C 160 164 168 16C\n");
		int rows = 0;
		for (int index = 0; index < MAX_ENTITIES && rows < 500; ++index)
		{
			unsigned char buffer[0x1C0];
			if (!read_bytes(SP_G_ENTITIES + static_cast<uintptr_t>(index) * g_stride, buffer, sizeof(buffer)))
			{
				break;
			}

			unsigned short class_id = 0;
			memcpy(&class_id, buffer + OFF_CLASSNAME, sizeof(class_id));
			if (class_id == 0 || classify(class_id) < 2)
			{
				continue;
			}

			char cname[48] = {};
			class_name(class_id, cname, sizeof(cname));
			fprintf(f, "%d %s |", index, cname);
			for (int off = 0x150; off <= 0x16C; off += 4)
			{
				unsigned int id = 0;
				memcpy(&id, buffer + off, sizeof(id));
				char text[48] = {};
				if (id > 1 && id < 0x10000 && class_name(id, text, sizeof(text)))
				{
					fprintf(f, " %X=%s", off, text);
				}
				else
				{
					fprintf(f, " %X=-", off);
				}
			}

			fputc('\n', f);
			++rows;
		}

		fclose(f);
		log_line("wrote trigger_names.txt (%d rows)", rows);
	}

	void write_event_summary()
	{
		char path[MAX_PATH];
		make_path(path, "events_summary.txt");
		FILE* f = nullptr;
		if (fopen_s(&f, path, "w") != 0 || !f)
		{
			return;
		}

		fprintf(f, "every script message seen so far: name, how many times, first entity id, seconds since first one\n");
		const double now = now_seconds();
		for (int i = 0; i < g_stat_count; ++i)
		{
			fprintf(f, "%-40s %8d  entity %u  first seen %.0f s ago\n", g_stats[i].name, g_stats[i].count,
			        g_stats[i].first_entity, now - g_stats[i].first_time);
		}

		fclose(f);
		log_line("wrote events_summary.txt (%d different messages)", g_stat_count);
	}

	void post_event_text(const char* text)
	{
		const double now = now_seconds();
		const int slot = g_event_next % EVENT_SLOTS;
		script_event& e = g_events[slot];
		strcpy_s(e.name, sizeof(e.name), text);
		e.entity = 0;
		e.count = 1;
		e.time = now;
		g_event_next = g_event_next + 1;
		log_line("watch: %s", text);
	}

	// Looks at the level's trigger / wall entities a few times a second and reports when one appears,
	// disappears (for example a used-up trigger) or changes anything about itself other than its
	// position (a wall turning solid or not solid should show up as a changed number).
	unsigned char g_watch_bytes[MAX_ENTITIES][ENTITY_SIZE];
	unsigned short g_watch_class[MAX_ENTITIES];
	double g_watch_last[MAX_ENTITIES];
	int g_watch_offset_changes[ENTITY_SIZE / 4];
	bool g_watch_primed = false;

	bool watch_skips_offset(const size_t offset)
	{
		return (offset >= 0x10 && offset < 0x50) || (offset >= 0xC0 && offset < 0x110) || offset >= 0x2F0;
	}

	void log_recent_entries(const char* why);
	bool barrier_name(unsigned int name_id);

	void entity_watch()
	{
		int posted = 0;
		int gone_count = 0;
		const double now = now_seconds();
		for (int index = 0; index < MAX_ENTITIES; ++index)
		{
			unsigned char current[ENTITY_SIZE];
			if (!read_bytes(SP_G_ENTITIES + static_cast<uintptr_t>(index) * g_stride, current, g_stride))
			{
				break;
			}

			unsigned short class_id = 0;
			memcpy(&class_id, current + OFF_CLASSNAME, sizeof(class_id));
			const unsigned short before_class = g_watch_class[index];
			unsigned char* before = g_watch_bytes[index];
			const unsigned short relevant = class_id ? class_id : before_class;
			const bool interesting = relevant != 0 && classify(relevant) >= 2;

			if (g_watch_primed && interesting && posted < 6)
			{
				char name[48] = {};
				char kind[48] = {};
				unsigned int name_id = 0;
				memcpy(&name_id, (class_id == 0 ? before : current) + OFF_NAME, sizeof(name_id));
				if (name_id > 1 && name_id < 0x10000)
				{
					class_name(name_id, name, sizeof(name));
				}

				class_name(relevant, kind, sizeof(kind));
				const bool wall = classify(relevant) == 7 && barrier_name(name_id);
				char text[44] = {};
				if (before_class != 0 && class_id == 0)
				{
					++gone_count;
					snprintf(text, sizeof(text), "%s %s #%d", wall ? "WALL REMOVED" : "GONE", name[0] ? name : kind, index);
					if (wall) log_line("INVISIBLE WALL REMOVED: %s #%d", name, index);
				}
				else if (before_class == 0 && class_id != 0)
				{
					snprintf(text, sizeof(text), "%s %s #%d", wall ? "WALL ADDED" : "NEW", name[0] ? name : kind, index);
				}
				else if (class_id != 0 && now - g_watch_last[index] > 1.5)
				{
					for (size_t offset = 0; offset < g_stride; offset += 4)
					{
						if (watch_skips_offset(offset) || memcmp(current + offset, before + offset, 4) == 0)
						{
							continue;
						}

						unsigned int old_value, new_value;
						memcpy(&old_value, before + offset, 4);
						memcpy(&new_value, current + offset, 4);
						const int counter = ++g_watch_offset_changes[offset / 4];
						if (counter <= 400)
						{
							log_line("watch detail: %s #%d (%s) +0x%zX %X -> %X", name[0] ? name : "-", index, kind, offset,
							         old_value, new_value);
						}

						// Fields that change all the time everywhere are not interesting on screen.
						if (counter <= 25 && text[0] == '\0')
						{
							snprintf(text, sizeof(text), "%s #%d +%zX %X>%X", name[0] ? name : kind, index, offset, old_value,
							         new_value);
						}
					}
				}

				if (text[0])
				{
					++posted;
					g_watch_last[index] = now;
					post_event_text(text);
				}
			}

			g_watch_class[index] = class_id;
			memcpy(before, current, g_stride);
		}

		g_watch_primed = true;
		{
			// the level empties a few entities per scan, so count over a short window instead of one scan
			static double window_start = 0.0;
			static int window_gone = 0;
			static double last_report = -1000.0;
			const double t = now_seconds();
			if (t - window_start > 4.0)
			{
				window_start = t;
				window_gone = 0;
			}

			window_gone += gone_count;
			if ((gone_count >= 20 || window_gone >= 12) && t - last_report > 60.0)
			{
				last_report = t;
				log_recent_entries("LEVEL UNLOADING (many entities vanished)");
			}
		}
	}

	void push_recent(const char* name, double now);

	void rec_event(const char* name, unsigned int entity);

	void record_event(const unsigned int entity, const unsigned int string_id)
	{
		char name[44];
		if (!class_name(string_id, name, sizeof(name)))
		{
			return;
		}

		const double now = now_seconds();
		count_event(name, entity, now);
		rec_event(name, entity);
		if (!is_always_noise(name))
		{
			push_recent(name, now);
		}

		if (g_filter_on && (is_always_noise(name) || is_too_frequent(name, now)))
		{
			return;
		}

		for (int i = 0; i < EVENT_SLOTS; ++i)
		{
			script_event& e = g_events[i];
			if (e.time > 0.0 && now - e.time < 3.0 && strcmp(e.name, name) == 0)
			{
				++e.count;
				e.time = now;
				return;
			}
		}

		const int slot = g_event_next % EVENT_SLOTS;
		script_event& e = g_events[slot];
		strcpy_s(e.name, sizeof(e.name), name);
		e.entity = entity;
		e.count = 1;
		e.time = now;
		g_event_next = g_event_next + 1;

		if (g_events_logged < 5000)
		{
			++g_events_logged;
			log_line("event: %s (entity id %u)", name, entity);
		}
	}

	void hooked_notify(const unsigned int entity, const unsigned int string_id, const unsigned int count)
	{
		__try
		{
			if (string_id != 0 && string_id < 0x100000)
			{
				record_event(entity, string_id);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}

		g_original_notify(entity, string_id, count);
	}

	// ---- learning which flag a flag-setting trigger sets ------------------------------------
	// The flag's name is not stored in the trigger itself. But when you step into one, the scripts
	// announce the flag right away, so we remember the messages that follow each entry.
	struct recent_event
	{
		char name[44];
		double time;
	};

	recent_event g_recent[64];
	volatile int g_recent_next = 0;

	void push_recent(const char* name, const double now)
	{
		// ignore the very common ones that are never flag names
		static const char* const kSkip[] = {"trigger", "death", "spawned", "enemy", "explode", "sound", "hide", "look",
		                                    "turn", "trans", "react", "pop_up", "crouch", "suppression"};
		for (const char* word : kSkip)
		{
			if (strstr(name, word))
			{
				return;
			}
		}

		const int slot = g_recent_next % 64;
		strcpy_s(g_recent[slot].name, sizeof(g_recent[slot].name), name);
		g_recent[slot].time = now;
		g_recent_next = g_recent_next + 1;
	}

	struct flag_learning
	{
		char names[4][44];
		int counts[4];
		int used;
	};

	flag_learning g_learned[MAX_ENTITIES];
	bool g_was_inside[MAX_ENTITIES];
	double g_entry_time[MAX_ENTITIES];
	double g_consumed[MAX_ENTITIES];

	char g_slot_flags[MAX_ENTITIES][96];

	// Flags confirmed (by playing) to start a mission ending. Add new ones here as they are found.
	//   mission: fission - multiple_flag_set, stepping in starts the ~48 second ending
	const char* const kKnownEndFlags[] = {"ct_final_retreat"};

	// Flags that mean "you are leaving the play area / this kills you".
	bool is_fail_flag(const int slot)
	{
		if (slot < 0 || slot >= MAX_ENTITIES || !g_slot_flags[slot][0])
		{
			return false;
		}

		static const char* const kFail[] = {"player_leaving_map", "leaving_map", "_death", "kill", "fail", "out_of_bounds", "oob",
		                                    "abandon", "mission_area", "leave_area",
		                                    "left_map", "leaving_squad", "left_squad", "returning_to_map"};
		for (const char* word : kFail)
		{
			if (strstr(g_slot_flags[slot], word))
			{
				return true;
			}
		}

		return false;
	}

	bool is_known_end_flag(const int slot)
	{
		if (slot < 0 || slot >= MAX_ENTITIES || !g_slot_flags[slot][0])
		{
			return false;
		}

		for (const char* known : kKnownEndFlags)
		{
			if (strstr(g_slot_flags[slot], known))
			{
				return true;
			}
		}

		return false;
	}

	bool learned_flags(const int slot, char* out, const size_t capacity)
	{
		if (slot < 0 || slot >= MAX_ENTITIES || g_slot_flags[slot][0] == '\0')
		{
			return false;
		}

		snprintf(out, capacity, "%s", g_slot_flags[slot]);
		return true;
	}

	void learn_from_entry(const int slot, const double now)
	{
		const double consumed = g_consumed[slot];
		for (int n = 0; n < 64; ++n)
		{
			const recent_event& e = g_recent[n];
			if (e.time <= consumed || e.time < g_entry_time[slot] - 0.3 || e.time > g_entry_time[slot] + 2.5 || e.time > now)
			{
				continue;
			}

			if (e.time > g_consumed[slot])
			{
				g_consumed[slot] = e.time;
			}

			flag_learning& l = g_learned[slot];
			bool found = false;
			for (int i = 0; i < l.used; ++i)
			{
				if (strcmp(l.names[i], e.name) == 0)
				{
					++l.counts[i];
					found = true;
				}
			}

			if (!found && l.used < 4)
			{
				strcpy_s(l.names[l.used], sizeof(l.names[l.used]), e.name);
				l.counts[l.used] = 1;
				++l.used;
				log_line("flag trigger #%d: message after entering: %s", slot, e.name);
			}
		}
	}

	// Remembers the last few boxes entered, to explain a mission end afterwards.
	struct entry_note
	{
		char text[160];
		double time;
	};

	entry_note g_entries[300];
	int g_entry_next = 0;
	bool g_any_inside[MAX_ENTITIES];

	void note_entry(const hit& h, const double now)
	{
		char type_text[64] = {};
		char given[48] = {};
		if (h.type_id > 1 && h.type_id < 0x10000)
		{
			class_name(h.type_id, type_text, sizeof(type_text));
		}

		if (h.name_id > 1 && h.name_id < 0x10000)
		{
			class_name(h.name_id, given, sizeof(given));
		}

		// Show the interesting ones in the top-right list too.
		{
			const unsigned char special = type_special(h.type_id);
			bool boring = false;
			static const char* const kBoring[] = {"tonemap", "exposure", "lighting", "dof_", "seoul_", "_vol", "visionset",
			                                     "lightgrid", "shadow", "flicker", "lensflare"};
			for (const char* word : kBoring)
			{
				if (given[0] && strstr(given, word))
				{
					boring = true;
				}
			}

			const bool worth = h.flag == 1 || special == 2 || special == 3 || h.kind == 3 || h.kind == 4 || h.kind == 5 ||
				(h.kind == 2 && given[0] && !boring);
			if (worth && h.kind != 7)
			{
				char line[44];
				snprintf(line, sizeof(line), "IN %s #%d", given[0] ? given : (type_text[0] ? type_text + (strncmp(type_text, "trigger_", 8) == 0 ? 8 : 0) : "?"), h.slot);
				post_event_text(line);
			}
		}

		if (is_known_end_flag(h.slot))
		{
			log_line("KNOWN MISSION-END TRIGGER ENTERED: #%d %s (the ending should start now)", h.slot, g_slot_flags[h.slot]);
			post_event_text("MISSION END TRIGGER!");
		}

		entry_note& n = g_entries[g_entry_next % 300];
		char flagtext[100] = {};
		learned_flags(h.slot, flagtext, sizeof(flagtext));
		snprintf(n.text, sizeof(n.text), "%s #%d %s%s%s", type_text[0] ? type_text : "?", h.slot, given[0] ? given : "(no name)",
			flagtext[0] ? " -> " : "", flagtext);
		n.time = now;
		++g_entry_next;
	}

	void log_recent_entries(const char* why)
	{
		const double now = now_seconds();
		log_line("%s - boxes entered in the last 120 seconds (newest last; the mission end is usually 45-50 s before the unload):", why);
		for (int k = 300; k >= 1; --k)
		{
			const int index = ((g_entry_next - k) % 300 + 300) % 300;
			if (g_entry_next - k < 0 || now - g_entries[index].time > 120.0)
			{
				continue;
			}

			log_line("   %.0f s ago: %s", now - g_entries[index].time, g_entries[index].text);
		}
	}

	// Called once per picture with everything near the player.
	void update_flag_learning(const hit* hits, const int count)
	{
		const double now = now_seconds();
		static bool inside_now[MAX_ENTITIES];
		memset(inside_now, 0, sizeof(inside_now));
		for (int i = 0; i < count; ++i)
		{
			if (hits[i].inside && hits[i].slot >= 0 && hits[i].slot < MAX_ENTITIES && !g_any_inside[hits[i].slot])
			{
				note_entry(hits[i], now);
			}
		}

		static bool any_now[MAX_ENTITIES];
		memset(any_now, 0, sizeof(any_now));
		for (int i = 0; i < count; ++i)
		{
			if (hits[i].inside && hits[i].slot >= 0 && hits[i].slot < MAX_ENTITIES)
			{
				any_now[hits[i].slot] = true;
			}
		}

		memcpy(g_any_inside, any_now, sizeof(g_any_inside));

		for (int i = 0; i < count; ++i)
		{
			if (hits[i].slot < 0 || hits[i].slot >= MAX_ENTITIES || !hits[i].inside || type_special(hits[i].type_id) != 2)
			{
				continue;
			}

			const int slot = hits[i].slot;
			inside_now[slot] = true;
			if (!g_was_inside[slot])
			{
				g_entry_time[slot] = now;
			}

			if (now - g_entry_time[slot] < 3.0)
			{
				learn_from_entry(slot, now);
			}
		}

		memcpy(g_was_inside, inside_now, sizeof(g_was_inside));
	}

	// Top-centre list of every box the player is standing in.
	void draw_inside_panel(const hit* hits, const int count)
	{
		const float scale = g_h >= 1200.0f ? 2.0f : 1.0f;
		const float line = 10.0f * scale + 4.0f;
		float y = 10.0f;
		int shown = 0;
		for (int i = 0; i < count && shown < 8; ++i)
		{
			if (!hits[i].inside)
			{
				continue;
			}

			char kind[64] = {};
			char given[48] = {};
			if (hits[i].type_id > 1 && hits[i].type_id < 0x10000)
			{
				class_name(hits[i].type_id, kind, sizeof(kind));
			}

			if (!kind[0])
			{
				class_name(hits[i].class_id, kind, sizeof(kind));
			}

			if (hits[i].name_id > 1 && hits[i].name_id < 0x10000)
			{
				class_name(hits[i].name_id, given, sizeof(given));
			}

			const char* type_short = strncmp(kind, "trigger_", 8) == 0 ? kind + 8 : kind;
			char text[200];
			char learned[100];
			if (learned_flags(hits[i].slot, learned, sizeof(learned)))
			{
				snprintf(text, sizeof(text), "IN: %s - %s #%d -> %s", given[0] ? given : "-", type_short, hits[i].slot, learned);
			}
			else
			{
				snprintf(text, sizeof(text), "IN: %s - %s #%d", given[0] ? given : "-", type_short, hits[i].slot);
			}

			const float width = static_cast<float>(strlen(text)) * 6.0f * scale;
			const float x = g_w * 0.5f - width * 0.5f;
			const float back[4] = {0.0f, 0.0f, 0.0f, 0.6f};
			add_rect(x - 4.0f, y - 3.0f, width + 8.0f, 7.0f * scale + 6.0f, back);
			draw_text(x, y, text, scale, kColors[5]);
			y += line;
			++shown;
		}
	}

	void draw_events()
	{
		const double now = now_seconds();
		const float scale = g_h >= 1200.0f ? 2.0f : 1.0f;
		const float line_height = 11.0f * scale;
		float y = 10.0f;

		// newest first
		for (int n = 1; n <= EVENT_SLOTS; ++n)
		{
			const int index = ((g_event_next - n) % EVENT_SLOTS + EVENT_SLOTS) % EVENT_SLOTS;
			const script_event& e = g_events[index];
			const double age = now - e.time;
			if (e.time <= 0.0 || age > 10.0)
			{
				continue;
			}

			char text[96];
			if (e.count > 1)
			{
				snprintf(text, sizeof(text), "%s x%d", e.name, e.count);
			}
			else
			{
				snprintf(text, sizeof(text), "%s", e.name);
			}

			const float alpha = age < 7.0 ? 1.0f : static_cast<float>((10.0 - age) / 3.0);
			const float width = static_cast<float>(strlen(text)) * 6.0f * scale;
			const float x = g_w - width - 14.0f;
			const float back[4] = {0.0f, 0.0f, 0.0f, 0.65f * alpha};
			const float color[4] = {1.0f, 1.0f, 1.0f, alpha};
			add_rect(x - 4.0f, y - 3.0f, width + 7.0f, 7.0f * scale + 6.0f, back);
			draw_text(x, y, text, scale, color);
			y += line_height + 4.0f;
		}
	}

	// ---- level collision blocks (Delete key) ------------------------------------------------
	// The level's collision data is stored as a tree of boxes. We keep the smallest boxes (the "leaves")
	// and draw the ones near the player.
	struct block
	{
		float mid[3];
		float half[3];
	};

	block* volatile g_blocks = nullptr;
	volatile int g_block_count = 0;
	volatile bool g_collision_on = false;   // Delete
	volatile bool g_collision_refresh = false;
	volatile int g_collision_range = 0;   // PageDown
	const float kCollisionRanges[3] = {600.0f, 1400.0f, 2600.0f};

	void draw_collision_blocks(const vec3& player)
	{
		block* blocks = g_blocks;
		const int count = g_block_count;
		if (!blocks || count <= 0)
		{
			return;
		}

		constexpr int KEEP = 160;
		float best_dist[KEEP];
		int best_index[KEEP];
		int kept = 0;
		const float max_distance = kCollisionRanges[g_collision_range];

		for (int i = 0; i < count; ++i)
		{
			const block& b = blocks[i];
			const float ex = std::fabs(player.x - b.mid[0]) - b.half[0];
			const float ey = std::fabs(player.y - b.mid[1]) - b.half[1];
			const float ez = std::fabs(player.z - b.mid[2]) - b.half[2];
			const float cx = ex > 0.0f ? ex : 0.0f;
			const float cy = ey > 0.0f ? ey : 0.0f;
			const float cz = ez > 0.0f ? ez : 0.0f;
			const float dist = cx * cx + cy * cy + cz * cz;
			if (dist > max_distance * max_distance)
			{
				continue;
			}

			int position = kept < KEEP ? kept : KEEP - 1;
			if (kept >= KEEP && dist >= best_dist[KEEP - 1])
			{
				continue;
			}

			if (kept < KEEP)
			{
				++kept;
			}

			while (position > 0 && best_dist[position - 1] > dist)
			{
				best_dist[position] = best_dist[position - 1];
				best_index[position] = best_index[position - 1];
				--position;
			}

			best_dist[position] = dist;
			best_index[position] = i;
		}

		g_thin_lines = true;
		for (int n = kept - 1; n >= 0; --n)
		{
			const block& b = blocks[best_index[n]];
			hit h = {};
			h.kind = 9;
			h.mid = {b.mid[0], b.mid[1], b.mid[2]};
			h.half = {b.half[0], b.half[1], b.half[2]};
			draw_box(h);
		}

		g_thin_lines = false;
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
		else if (class_name(id, name, sizeof(name)) && (strcmp(name, "script_brushmodel") == 0 || strcmp(name, "info_volume") == 0))
		{
			kind = 7;
		}

		g_class_kind[id] = kind;
		return kind;
	}

	// Does this trigger's name look like it ends the mission, fails it, or marks the edge of the play area?
	unsigned char g_name_flag[65536];

	bool name_has_token(const char* name, const char* token)
	{
		const size_t length = strlen(token);
		const char* p = name;
		while ((p = strstr(p, token)) != nullptr)
		{
			const bool start_ok = p == name || p[-1] == '_';
			const bool end_ok = p[length] == '\0' || p[length] == '_' || (p[length] >= '0' && p[length] <= '9');
			if (start_ok && end_ok)
			{
				return true;
			}

			p += 1;
		}

		return false;
	}

	unsigned char name_flag(const unsigned int name_id)
	{
		if (name_id < 2 || name_id >= 0x10000)
		{
			return 1;
		}

		if (g_name_flag[name_id] != 0)
		{
			return g_name_flag[name_id];
		}

		unsigned char flag = 1;
		char name[64];
		if (class_name(name_id, name, sizeof(name)))
		{
			for (char* c = name; *c; ++c)
			{
				if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c + 32);
			}

			static const char* const kTokens[] = {"end", "endmission", "outro", "fail", "failed", "failure", "kill", "killplayer",
			                                      "oob", "bounds", "boundary", "leaving", "leave", "abandon", "restricted",
			                                      "complete", "completed", "finish", "finished", "credits", "ending",
			                                      "nextmission", "deadzone", "warning"};
			static const char* const kPhrases[] = {"mission_end", "end_mission", "out_of_bounds", "leave_area", "mission_area",
			                                       "playable_area", "kill_player", "player_kill", "mission_fail", "fail_mission",
			                                       "mission_complete", "level_end", "end_level", "return_to", "go_back"};
			for (const char* token : kTokens)
			{
				if (name_has_token(name, token))
				{
					flag = 2;
				}
			}

			for (const char* phrase : kPhrases)
			{
				if (strstr(name, phrase))
				{
					flag = 2;
				}
			}
		}

		g_name_flag[name_id] = flag;
		return flag;
	}

	// Brush models whose name says they are an invisible wall / blocker.
	unsigned char g_barrier_name[65536];

	bool barrier_name(const unsigned int name_id)
	{
		if (name_id < 2 || name_id >= 0x10000)
		{
			return false;
		}

		if (g_barrier_name[name_id] == 0)
		{
			unsigned char verdict = 1;
			char name[64];
			if (class_name(name_id, name, sizeof(name)))
			{
				for (char* c = name; *c; ++c)
				{
					if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c + 32);
				}

				static const char* const kWords[] = {"clip", "blocker", "collision", "_col", "barrier", "invisible", "playerblock", "block_"};
				for (const char* word : kWords)
				{
					if (strstr(name, word))
					{
						verdict = 2;
					}
				}
			}

			g_barrier_name[name_id] = verdict;
		}

		return g_barrier_name[name_id] == 2;
	}

	// Finds the closest triggers. Returns how many it put in out[].
	int find_triggers(const vec3& player, const float max_distance, hit* out, const int capacity)
	{
		int count = 0;
		unsigned char buffer[0x1C0];

		for (int index = 0; index < MAX_ENTITIES; ++index)
		{
			bool ok = true;
			__try
			{
				memcpy(buffer, reinterpret_cast<const void*>(SP_G_ENTITIES + static_cast<uintptr_t>(index) * g_stride),
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
			unsigned int early_name = 0;
			memcpy(&early_name, buffer + OFF_NAME, sizeof(early_name));
			const bool barrier = kind == 7 && barrier_name(early_name);
			if (kind < 2 || (kind == 7 && !g_brushes_on && !barrier))
			{
				continue;
			}

			float b[6];
			memcpy(b, buffer + OFF_BOUNDS, sizeof(b));
			make_world_bounds(b, buffer);
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

			unsigned int name_id_value = 0;
			memcpy(&name_id_value, buffer + OFF_NAME, sizeof(name_id_value));
			const bool end_candidate = kind != 7 && (name_flag(name_id_value) == 2 || is_known_end_flag(index) || is_fail_flag(index));

			// The game moves a switched-off trigger 10000 units up. Draw those where they will be once switched on.
			bool switched_off = false;
			if (end_candidate && b[2] - player.z > 8000.0f && b[2] - player.z < 14000.0f)
			{
				b[2] -= 10000.0f;
				switched_off = true;
			}

			// Distance from the player to the surface of the box (0 when inside).
			const float ox = std::fabs(player.x - b[0]) - b[3];
			const float oy = std::fabs(player.y - b[1]) - b[4];
			const float oz = std::fabs(player.z - b[2]) - b[5];
			const float ex = ox > 0.0f ? ox : 0.0f;
			const float ey = oy > 0.0f ? oy : 0.0f;
			const float ez = oz > 0.0f ? oz : 0.0f;
			const float dist = std::sqrt(ex * ex + ey * ey + ez * ez);
			if (dist > max_distance && !end_candidate && !(kind == 3 && dist < 6000.0f) && !(barrier && dist < (g_show_mode == 2 ? 20000.0f : 4000.0f)))
			{
				continue;
			}

			hit h;
			h.dist = dist;
			h.key = end_candidate ? dist - 1.0e9f : dist;
			h.flag = end_candidate ? 1 : 0;
			h.barrier = barrier;
			h.disabled = switched_off;
			memcpy(&h.state, buffer + OFF_BOUNDS - 4, sizeof(h.state));
			h.slot = index;
			h.class_id = class_id;
			memcpy(&h.name_id, buffer + OFF_NAME, sizeof(h.name_id));
			memcpy(&h.type_id, buffer + OFF_TYPE, sizeof(h.type_id));
			h.kind = kind;
			h.inside = ox <= 0.0f && oy <= 0.0f && oz <= 0.0f;
			h.mid = {b[0], b[1], b[2]};
			h.half = {b[3], b[4], b[5]};

			// [ cycles what is shown, ] turns the leaving-area boxes on / off
			{
				const bool leave = h.flag == 1;
				if (leave && !g_leave_on)
				{
					continue;
				}

				const unsigned char special = type_special(h.type_id);
				const bool pink = kind == 7 && barrier;
				const bool magenta = !leave && kind != 7 && special == 2;
				const bool green = !leave && kind == 2 && special == 1;
				const int mode = g_show_mode;
				if ((mode == 1 && !green && !leave) || (mode == 2 && !pink && !leave) ||
					(mode == 3 && !magenta && !leave) || (mode == 4 && !leave))
				{
					continue;
				}
			}

			// Keep the list sorted, nearest first, and only the closest `capacity`.
			int position = count < capacity ? count : capacity - 1;
			if (count >= capacity && h.key >= out[position].key)
			{
				continue;
			}

			while (position > 0 && out[position - 1].key > h.key)
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

	// Reads the F-keys. Runs once per picture, whether or not anything is being drawn.
	// ---- camera sources ---------------------------------------------------------------
	// Found by the camera search: places where the game keeps the camera it renders with.
	// Source 0 is the player's own data (slow-updating, smoothed by us). The others are read
	// straight from the game; F3 cycles through them, F4 measures how often each one updates.
	struct camera_source
	{
		uintptr_t address;   // where the eye position (3 floats) is
		int kind;            // 0 = axes follow (forward, left, up), 1 = view angles follow
		size_t angle_offset; // for kind 1: where pitch/yaw are, relative to address
	};

	constexpr int SOURCE_COUNT = 10;
	camera_source kSources[SOURCE_COUNT] = {
		{0, 0, 0},                // 0: player data (not a real address)
		{0x14094A7C4, 0, 12},     // 1: eye + axes
		{0x14096FA64, 0, 12},     // 2: eye + axes
		{0x141111CB8, 0, 12},     // 3: eye + axes
		{0x14094AB88, 1, 12},     // 4: eye + angles
		{0x14115DE84, 1, 24},     // 5: eye, eye, angles
		{0x14115DE90, 1, 12},     // 6: eye + angles
		{0x141173784, 1, 12},     // 7: eye + angles
		{0x14117379C, 1, 12},     // 8: eye + angles
		{0x1411738D8, 1, 12},     // 9: eye + angles
	};

	volatile int g_source = 2; // 2 = updated every picture (found by the rate test)

	// Reads eye position and the two view angles (or axes turned back into angles) from a source.
	bool read_source(const int index, float* eye, float* pitch, float* yaw)
	{
		const camera_source& source = kSources[index];
		float first[3];
		if (!read_bytes(source.address, first, sizeof(first)))
		{
			return false;
		}

		memcpy(eye, first, sizeof(first));

		if (source.kind == 1)
		{
			float angles[2];
			if (!read_bytes(source.address + source.angle_offset, angles, sizeof(angles)))
			{
				return false;
			}

			*pitch = angles[0];
			*yaw = angles[1];
			return true;
		}

		float forward[3];
		if (!read_bytes(source.address + source.angle_offset, forward, sizeof(forward)))
		{
			return false;
		}

		const float rad_to_deg = 180.0f / 3.14159265f;
		*pitch = -std::asin(forward[2] > 1.0f ? 1.0f : (forward[2] < -1.0f ? -1.0f : forward[2])) * rad_to_deg;
		*yaw = std::atan2(forward[1], forward[0]) * rad_to_deg;
		return true;
	}

	// ---- how often does each source update? (F4) ----------------------------------------
	bool g_rate_test_active = false;
	double g_rate_test_end = 0.0;
	int g_rate_presents = 0;
	int g_rate_changes[SOURCE_COUNT];
	unsigned char g_rate_last[SOURCE_COUNT][24];
	bool g_rate_have_last[SOURCE_COUNT];

	void rate_test_start()
	{
		g_rate_test_active = true;
		g_rate_test_end = now_seconds() + 4.0;
		g_rate_presents = 0;
		memset(g_rate_changes, 0, sizeof(g_rate_changes));
		memset(g_rate_have_last, 0, sizeof(g_rate_have_last));
		log_line("rate test started: turn around continuously for 4 seconds");
	}

	// Raw bytes that should change when the camera moves or turns.
	bool rate_sample(const int index, unsigned char* out)
	{
		if (index == 0)
		{
			unsigned long long pointer = 0;
			if (!read_bytes(player_entity() + OFF_CLIENT, &pointer, sizeof(pointer)) || pointer < 0x140000000ull)
			{
				return false;
			}

			return read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_ORIGIN, out, 12) &&
				read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_PITCH, out + 12, 8);
		}

		const camera_source& source = kSources[index];
		return read_bytes(source.address, out, 12) && read_bytes(source.address + source.angle_offset, out + 12, 12);
	}

	void rate_test_tick()
	{
		if (!g_rate_test_active)
		{
			return;
		}

		++g_rate_presents;
		for (int i = 0; i < SOURCE_COUNT; ++i)
		{
			unsigned char now_bytes[24] = {};
			if (!rate_sample(i, now_bytes))
			{
				continue;
			}

			if (g_rate_have_last[i] && memcmp(now_bytes, g_rate_last[i], sizeof(now_bytes)) != 0)
			{
				++g_rate_changes[i];
			}

			memcpy(g_rate_last[i], now_bytes, sizeof(now_bytes));
			g_rate_have_last[i] = true;
		}

		if (now_seconds() >= g_rate_test_end)
		{
			g_rate_test_active = false;
			log_line("rate test finished: %d pictures were shown in 4 seconds", g_rate_presents);
			for (int i = 0; i < SOURCE_COUNT; ++i)
			{
				log_line("  source %d (0x%llX): changed in %d of %d pictures", i,
				         static_cast<unsigned long long>(kSources[i].address), g_rate_changes[i], g_rate_presents);
			}
		}
	}

	// Live tuning for the camera (numpad 1-4), mainly for multiplayer.
	volatile int g_pitch_sign = 1;
	volatile int g_yaw_sign = 1;
	volatile int g_yaw_turn = 0;       // times 90 degrees
	volatile bool g_smooth_on = true;

	// ---- ghost replay: watch a recorded run (ghost.txt) and a proposed route (route.txt) in the world ----
	constexpr int GHOST_MAX = 120000;
	constexpr int GS = 10;   // game-time x y z pitch yaw flags54 flags58 viewheight real-time
	struct ghost_track
	{
		int count = 0;
		float* data = nullptr;    // time, x, y, z, pitch, yaw per point
		float color[4] = {1, 1, 1, 1};
		const char* label = "";
		char status[80] = {};
	};

	ghost_track g_ghost[2];
	volatile bool g_ghost_on = false;
	volatile bool g_ghost_pause = false;
	volatile bool g_ghost_full = false;
	volatile bool g_ghost_only = false;
	volatile bool g_ghost_sync = false;    // follow my own game clock
	unsigned int g_self_gt = 0;            // my game clock in ms (set by the camera code)
	unsigned int g_self_gt0 = 0;
	volatile bool g_self_zero = false;   // hide all trigger / collision drawing
	double g_ghost_time = 0.0;
	double g_ghost_last_now = 0.0;
	float g_ghost_speed = 1.0f;
	volatile bool g_ghost_restart = false;
	volatile float g_ghost_jump = 0.0f;

	void load_ghost(ghost_track& track, const char* file_name)
	{
		if (!track.data)
		{
			track.data = static_cast<float*>(malloc(sizeof(float) * GS * GHOST_MAX));
		}

		track.count = 0;
		if (!track.data)
		{
			return;
		}

		char path[MAX_PATH];
		make_path(path, file_name);
		FILE* f = nullptr;
		if (fopen_s(&f, path, "rb") != 0 || !f)
		{
			snprintf(track.status, sizeof(track.status), "%s: file not found", file_name);
			return;
		}

		char line[512];
		while (fgets(line, sizeof(line), f) && track.count < GHOST_MAX)
		{
			char* p = line;
			char* end = nullptr;
			const float t = strtof(p, &end);
			if (end == p)
			{
				continue;
			}

			p = end;
			while (*p == ' ') ++p;
			if (p[0] != 'P' || p[1] != ' ')
			{
				continue;
			}

			p += 2;
			float v[5];
			bool ok = true;
			for (int i = 0; i < 5; ++i)
			{
				v[i] = strtof(p, &end);
				if (end == p) { ok = false; break; }
				p = end;
			}

			if (!ok || (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f))
			{
				continue;   // a reload moment where the game reported all zeros
			}

			float gt_axis = t;
			float* d = track.data + track.count * GS;
			d[0] = t; d[1] = v[0]; d[2] = v[1]; d[3] = v[2]; d[4] = v[3]; d[5] = v[4];
			d[6] = 0.0f; d[7] = 0.0f; d[8] = 60.0f; d[9] = t;
			// newer recordings add: movement flags (hex), stance flags, view height
			while (*p == ' ') ++p;
			if (*p)
			{
				const unsigned long f54 = strtoul(p, &end, 16);
				if (end != p)
				{
					p = end;
					const unsigned long f58 = strtoul(p, &end, 10);
					if (end != p)
					{
						p = end;
						const float vh = strtof(p, &end);
						d[6] = static_cast<float>(f54);
						d[7] = static_cast<float>(f58);
						if (end != p) d[8] = vh;
						p = end;
						const unsigned long gt = strtoul(p, &end, 10);
						if (end != p) gt_axis = static_cast<float>(gt) / 1000.0f;
					}
				}
			}

			d[0] = gt_axis;
			// a checkpoint restart sets the game clock back: forget everything recorded after that moment
			while (track.count > 0 && track.data[(track.count - 1) * GS] >= gt_axis)
			{
				--track.count;
			}

			if (track.count < GHOST_MAX)
			{
				float* dst = track.data + track.count * GS;
				if (dst != d) memmove(dst, d, sizeof(float) * GS);
				++track.count;
			}
		}

		fclose(f);

		// start both clocks at zero
		if (track.count > 0)
		{
			const float g0 = track.data[0];
			const float r0 = track.data[9];
			for (int i = 0; i < track.count; ++i)
			{
				track.data[i * GS] -= g0;
				track.data[i * GS + 9] -= r0;
			}
		}
		snprintf(track.status, sizeof(track.status), "%s: %d points, %.0f seconds", file_name, track.count,
		         track.count > 0 ? track.data[(track.count - 1) * GS] : 0.0f);
		log_line("ghost: %s", track.status);
	}

	void ghost_keys()
	{
		static bool was5, was6, was7, was8, was9, was0, wasp, wasm, wasd, wasdec;
		if (key_pressed(VK_NUMPAD5, &was5))
		{
			g_ghost_on = !g_ghost_on;
			if (g_ghost_on)
			{
				g_ghost[0].color[0] = 1.0f; g_ghost[0].color[1] = 0.55f; g_ghost[0].color[2] = 0.1f; g_ghost[0].color[3] = 1.0f;
				g_ghost[1].color[0] = 0.2f; g_ghost[1].color[1] = 1.0f; g_ghost[1].color[2] = 0.3f; g_ghost[1].color[3] = 1.0f;
				g_ghost[0].label = "RUNNER";
				g_ghost[1].label = "ROUTE";
				load_ghost(g_ghost[0], "ghost.txt");
				load_ghost(g_ghost[1], "route.txt");
				g_ghost_time = 0.0;
				g_self_zero = true;
				g_ghost_last_now = now_seconds();
			}
		}

		if (key_pressed(VK_NUMPAD6, &was6)) g_ghost_pause = !g_ghost_pause;
		if (key_pressed(VK_NUMPAD7, &was7) && g_ghost_speed > 0.13f) g_ghost_speed *= 0.5f;
		if (key_pressed(VK_NUMPAD8, &was8) && g_ghost_speed < 7.9f) g_ghost_speed *= 2.0f;
		if (key_pressed(VK_NUMPAD9, &was9)) g_ghost_restart = true;
		if (key_pressed(VK_NUMPAD0, &was0)) g_ghost_full = !g_ghost_full;
		if (key_pressed(VK_DECIMAL, &wasdec))
		{
			g_ghost_sync = !g_ghost_sync;
			g_self_zero = true;
			log_line("ghost follows my game clock: %s", g_ghost_sync ? "ON" : "OFF");
		}

		if (key_pressed(VK_DIVIDE, &wasd))
		{
			g_ghost_only = !g_ghost_only;
			log_line("ghost only mode %s", g_ghost_only ? "ON" : "OFF");
		}

		if (key_pressed(VK_ADD, &wasp)) g_ghost_jump = 10.0f;
		if (key_pressed(VK_SUBTRACT, &wasm)) g_ghost_jump = -10.0f;
	}

	// out: x y z pitch yaw flags54 flags58 viewheight horizontal-speed
	bool ghost_sample(const ghost_track& tr, const double t, float* out)
	{
		if (tr.count < 2)
		{
			return false;
		}

		const float* d = tr.data;
		int lo = 0, hi = tr.count - 1;
		if (t <= d[0] || t >= d[hi * GS])
		{
			const float* e = t <= d[0] ? d : d + hi * GS;
			for (int i = 0; i < 5; ++i) out[i] = e[1 + i];
			out[5] = e[6]; out[6] = e[7]; out[7] = e[8]; out[8] = 0.0f; out[9] = e[9];
			return true;
		}

		while (hi - lo > 1)
		{
			const int mid = (lo + hi) / 2;
			if (d[mid * GS] <= t) lo = mid; else hi = mid;
		}

		const float* a = d + lo * GS;
		const float* b = d + hi * GS;
		const float span = b[0] - a[0];
		const float f = span > 0.0001f ? static_cast<float>((t - a[0]) / span) : 0.0f;
		for (int i = 0; i < 3; ++i) out[i] = a[1 + i] + (b[1 + i] - a[1 + i]) * f;
		out[3] = a[4] + (b[4] - a[4]) * f;
		float dy = b[5] - a[5];
		while (dy > 180.0f) dy -= 360.0f;
		while (dy < -180.0f) dy += 360.0f;
		out[4] = a[5] + dy * f;
		out[5] = a[6];
		out[6] = a[7];
		out[7] = a[8] + (b[8] - a[8]) * f;
		out[8] = span > 0.0001f ? std::sqrt((b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2])) / span : 0.0f;
		out[9] = a[9] + (b[9] - a[9]) * f;
		return true;
	}

	void ghost_box(const vec3& base, const float yaw, const float half, const float z0, const float z1, const float* color)
	{
		const float rad = 3.14159265f / 180.0f;
		const float c = std::cos(yaw * rad);
		const float s = std::sin(yaw * rad);
		vec3 p[8];
		for (int i = 0; i < 8; ++i)
		{
			const float lx = (i & 1) ? half : -half;
			const float ly = (i & 2) ? half : -half;
			p[i] = {base.x + lx * c - ly * s, base.y + lx * s + ly * c, base.z + ((i & 4) ? z1 : z0)};
		}

		for (int i = 0; i < 8; ++i)
		{
			for (int bit = 1; bit <= 4; bit <<= 1)
			{
				if ((i & bit) == 0) edge(p[i], p[i | bit], color);
			}
		}
	}

	// a box around the straight line between two points (a limb), r = half thickness
	void ghost_limb(const vec3& a, const vec3& b, const float r, const float* color)
	{
		vec3 d = {b.x - a.x, b.y - a.y, b.z - a.z};
		const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		if (len < 0.01f)
		{
			return;
		}

		d = {d.x / len, d.y / len, d.z / len};
		// any vector not parallel to d
		vec3 helper = std::fabs(d.z) < 0.9f ? vec3{0.0f, 0.0f, 1.0f} : vec3{1.0f, 0.0f, 0.0f};
		vec3 u = {d.y * helper.z - d.z * helper.y, d.z * helper.x - d.x * helper.z, d.x * helper.y - d.y * helper.x};
		const float ul = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
		u = {u.x / ul * r, u.y / ul * r, u.z / ul * r};
		vec3 v = {(d.y * u.z - d.z * u.y), (d.z * u.x - d.x * u.z), (d.x * u.y - d.y * u.x)};
		vec3 c[8];
		for (int i = 0; i < 8; ++i)
		{
			const vec3& end = (i & 4) ? b : a;
			const float su = (i & 1) ? 1.0f : -1.0f;
			const float sv = (i & 2) ? 1.0f : -1.0f;
			c[i] = {end.x + u.x * su + v.x * sv, end.y + u.y * su + v.y * sv, end.z + u.z * su + v.z * sv};
		}

		for (int i = 0; i < 8; ++i)
		{
			for (int bit = 1; bit <= 4; bit <<= 1)
			{
				if ((i & bit) == 0) edge(c[i], c[i | bit], color);
			}
		}
	}

	// A simple wireframe person: head, torso, arms holding a weapon where the runner looks, legs that walk.
	void ghost_person(const vec3& feet, const float yaw, const float pitch, const float vh, const bool prone, const bool air,
	                  const float speed, const double t, const float* color)
	{
		const float rad = 3.14159265f / 180.0f;
		const float cy = std::cos(yaw * rad), sy = std::sin(yaw * rad);
		const vec3 f = {cy, sy, 0.0f};          // forward
		const vec3 r = {sy, -cy, 0.0f};         // right
		auto at = [&](const float fwd, const float right, const float up) -> vec3 {
			return {feet.x + f.x * fwd + r.x * right, feet.y + f.y * fwd + r.y * right, feet.z + up};
		};

		if (prone)
		{
			// lying / sliding, head forward, legs trailing behind
			ghost_limb(at(22.0f, 0.0f, 12.0f), at(26.0f, 0.0f, 14.0f), 6.0f, color);            // head
			ghost_limb(at(14.0f, 0.0f, 12.0f), at(-14.0f, 0.0f, 10.0f), 9.0f, color);            // torso
			ghost_limb(at(-14.0f, 4.0f, 8.0f), at(-52.0f, 5.0f, 6.0f), 4.5f, color);             // legs
			ghost_limb(at(-14.0f, -4.0f, 8.0f), at(-52.0f, -5.0f, 6.0f), 4.5f, color);
			ghost_limb(at(12.0f, 9.0f, 10.0f), at(36.0f, 6.0f, 10.0f), 3.5f, color);             // arms forward
			ghost_limb(at(12.0f, -9.0f, 10.0f), at(36.0f, -6.0f, 10.0f), 3.5f, color);
			return;
		}

		const float k = vh / 60.0f < 0.5f ? 0.5f : (vh / 60.0f > 1.0f ? 1.0f : vh / 60.0f);   // squat when crouching
		const float hip = 34.0f * k, shoulder = 56.0f * k + (1.0f - k) * 6.0f, neck = shoulder + 4.0f;
		const float amp = air ? 0.0f : (speed > 200.0f ? 1.0f : speed / 200.0f);
		const float swing = std::sin(static_cast<float>(t) * (6.0f + speed * 0.012f)) * 16.0f * amp;
		const float lift = air ? 10.0f : 0.0f;

		// legs: hip -> knee -> foot, swinging forward and back
		for (int side = -1; side <= 1; side += 2)
		{
			const float sw = swing * static_cast<float>(side);
			const vec3 h = at(0.0f, 5.0f * side, hip);
			const vec3 knee = at(sw * 0.6f + (1.0f - k) * 14.0f + lift * 0.4f, 5.5f * side, hip * 0.5f + lift);
			const vec3 foot = at(sw + lift * 0.6f, 6.0f * side, (side * swing > 6.0f ? 4.0f : 0.0f) + lift);
			ghost_limb(h, knee, 4.0f, color);
			ghost_limb(knee, foot, 3.2f, color);
		}

		// torso and head
		ghost_limb(at(0.0f, 0.0f, hip), at(0.0f, 0.0f, shoulder), 8.0f, color);
		ghost_limb(at(0.0f, 0.0f, shoulder), at(0.0f, 0.0f, neck), 3.0f, color);
		ghost_box({feet.x, feet.y, feet.z}, yaw, 5.5f, neck + 1.0f, neck + 12.0f, color);

		// arms hold the weapon out in front, in the direction the runner is looking
		const float cp = std::cos(pitch * rad), sp = std::sin(pitch * rad);
		const vec3 aim = {cp * cy, cp * sy, -sp};
		const vec3 eye = {feet.x, feet.y, feet.z + vh + 6.0f};
		const vec3 grip = {eye.x + aim.x * 24.0f, eye.y + aim.y * 24.0f, eye.z + aim.z * 24.0f - 8.0f};
		const vec3 grip2 = {eye.x + aim.x * 38.0f, eye.y + aim.y * 38.0f, eye.z + aim.z * 38.0f - 9.0f};
		ghost_limb(at(0.0f, 9.0f, shoulder - 2.0f), grip, 3.0f, color);
		ghost_limb(at(0.0f, -9.0f, shoulder - 2.0f), grip2, 3.0f, color);
		ghost_limb(grip, grip2, 2.5f, color);   // the weapon
	}

	// a box with its own length (along the facing direction) and width, centred ahead of the feet
	void ghost_slab(const vec3& base, const float yaw, const float ahead, const float half_len, const float half_wid, const float z0, const float z1, const float* color)
	{
		const float rad = 3.14159265f / 180.0f;
		const float c = std::cos(yaw * rad);
		const float s = std::sin(yaw * rad);
		vec3 p[8];
		for (int i = 0; i < 8; ++i)
		{
			const float lx = ahead + ((i & 1) ? half_len : -half_len);
			const float ly = (i & 2) ? half_wid : -half_wid;
			p[i] = {base.x + lx * c - ly * s, base.y + lx * s + ly * c, base.z + ((i & 4) ? z1 : z0)};
		}

		for (int i = 0; i < 8; ++i)
		{
			for (int bit = 1; bit <= 4; bit <<= 1)
			{
				if ((i & bit) == 0) edge(p[i], p[i | bit], color);
			}
		}
	}

	void ghost_draw(const double now, const float* viewer)
	{
		if (!g_ghost_on)
		{
			return;
		}

		double dt = now - g_ghost_last_now;
		g_ghost_last_now = now;
		if (dt < 0.0 || dt > 0.25) dt = 0.0;
		float length = 0.0f;
		for (int i = 0; i < 2; ++i)
		{
			if (g_ghost[i].count > 1)
			{
				const float e = g_ghost[i].data[(g_ghost[i].count - 1) * GS];
				if (e > length) length = e;
			}
		}

		if (g_self_zero) { g_self_zero = false; g_self_gt0 = g_self_gt; g_ghost_time = 0.0; }
		if (g_ghost_sync && g_self_gt0 != 0)
		{
			// follow my own game clock, so a checkpoint restart sends the ghost back too
			g_ghost_time = (static_cast<double>(g_self_gt) - static_cast<double>(g_self_gt0)) / 1000.0;
			if (g_ghost_time < 0.0) g_ghost_time = 0.0;
		}
		else
		{
			if (!g_ghost_pause) g_ghost_time += dt * g_ghost_speed;
			if (g_ghost_restart) { g_ghost_restart = false; g_ghost_time = 0.0; g_self_gt0 = g_self_gt; }
			if (g_ghost_jump != 0.0f) { g_ghost_time += g_ghost_jump; g_ghost_jump = 0.0f; if (g_ghost_time < 0.0) g_ghost_time = 0.0; }
			if (length > 0.0f && g_ghost_time > length + 3.0) g_ghost_time = 0.0;
		}

		g_ghost_restart = false;

		// ---- all ghost information, top middle of the screen ----
		const float white[4] = {1, 1, 1, 1};
		const float big = g_h >= 1200.0f ? 2.0f : 1.0f;
		auto clock_text = [](const double sec, char* out, const size_t cap) {
			const double v = sec < 0.0 ? 0.0 : sec;
			const int minutes = static_cast<int>(v / 60.0);
			snprintf(out, cap, "%02d:%04.1f", minutes, v - minutes * 60.0);
		};
		auto centered = [&](const float y, const char* text, const float scale, const float* color) {
			const float width = static_cast<float>(strlen(text)) * 6.0f * scale;
			draw_text((g_w - width) * 0.5f, y, text, scale, color);
		};

		float runner[10] = {};
		const bool have_runner = ghost_sample(g_ghost[0], g_ghost_time, runner);
		const double real_total = g_ghost[0].count > 0 ? g_ghost[0].data[(g_ghost[0].count - 1) * GS + 9] : 0.0;
		char c1[16], c2[16], c3[16], c4[16], c5[16];
		clock_text(g_ghost_time, c1, sizeof(c1));
		clock_text(length, c2, sizeof(c2));
		clock_text(have_runner ? runner[9] : 0.0, c3, sizeof(c3));
		clock_text(real_total, c4, sizeof(c4));
		char text[200];
		snprintf(text, sizeof(text), "GHOST  GAME %s / %s    REAL %s / %s", c1, c2, c3, c4);
		float row = 6.0f;
		centered(row, text, big, white);
		row += 12.0f * big;
		if (g_self_gt0 != 0)
		{
			clock_text((static_cast<double>(g_self_gt) - static_cast<double>(g_self_gt0)) / 1000.0, c5, sizeof(c5));
			snprintf(text, sizeof(text), "YOU  GAME %s    SPEED x%.2f %s%s%s", c5, g_ghost_speed, g_ghost_pause ? "PAUSED " : "",
			         g_ghost_sync ? "SYNCED TO MY CLOCK " : "", g_ghost_full ? "FULL PATH" : "");
		}
		else
		{
			snprintf(text, sizeof(text), "SPEED x%.2f %s%s%s", g_ghost_speed, g_ghost_pause ? "PAUSED " : "", g_ghost_sync ? "SYNCED " : "", g_ghost_full ? "FULL PATH" : "");
		}

		centered(row, text, big, white);
		row += 12.0f * big;
		for (int i = 0; i < 2; ++i)
		{
			if (g_ghost[i].status[0])
			{
				centered(row, g_ghost[i].status, 1.0f, g_ghost[i].color);
				row += 12.0f;
			}
		}

		for (int i = 0; i < 2; ++i)
		{
			const ghost_track& tr = g_ghost[i];
			if (tr.count < 2)
			{
				continue;
			}

			// the path: the last few seconds always, the whole route when asked (Num 0)
			const float* d = tr.data;
			const int step = tr.count > 3000 && g_ghost_full ? tr.count / 3000 : 1;
			int drawn = 0;
			for (int k = 0; k + step < tr.count && drawn < 1500; k += step)
			{
				const float* a = d + k * GS;
				const float* b = d + (k + step) * GS;
				const bool recent = a[0] <= g_ghost_time && a[0] >= g_ghost_time - 6.0;
				if (!g_ghost_full && !recent)
				{
					continue;
				}

				const float dx = a[1] - viewer[0], dy = a[2] - viewer[1];
				if (dx * dx + dy * dy > 4000.0f * 4000.0f)
				{
					continue;
				}

				const vec3 pa = {a[1], a[2], a[3] + 4.0f};
				const vec3 pb = {b[1], b[2], b[3] + 4.0f};
				edge(pa, pb, tr.color);
				++drawn;
			}

			float s[10];
			if (!ghost_sample(tr, g_ghost_time, s))
			{
				continue;
			}

			const vec3 feet = {s[0], s[1], s[2]};
			const unsigned int f54 = static_cast<unsigned int>(s[5]);
			const unsigned int f58 = static_cast<unsigned int>(s[6]) & 0xFu;
			const float vh = s[7];
			const bool prone = f58 == 8 || vh < 25.0f;
			const bool crouch = !prone && (f58 == 4 || vh < 52.0f);
			const bool air = (f54 & 0x2000u) != 0;
			// vertical speed over half a second, to tell a hover (slow steady sinking) from a jump or a fall
			float before[10] = {}, after[10] = {};
			ghost_sample(tr, g_ghost_time - 0.25, before);
			ghost_sample(tr, g_ghost_time + 0.25, after);
			const float vz = (after[2] - before[2]) / 0.5f;
			const char* state = "";
			if (prone) state = s[8] > 150.0f ? "SLIDE" : "PRONE";
			else if (air && s[8] > 280.0f) state = "DASH";
			else if (air && vz < -170.0f) state = "FALL";
			else if (air && vz < -25.0f) state = "HOVER";
			else if (air && vz > 25.0f) state = "JUMP";
			else if (air) state = "AIR";
			else if (crouch) state = "CROUCH";
			else if (f54 & 0x4000u) state = "SPRINT";
			float eye_z = prone ? 16.0f : vh + 6.0f;
			ghost_person(feet, s[4], s[3], vh, prone, air, s[8], g_ghost_time, tr.color);

			if (air)
			{
				ghost_box(feet, s[4], 20.0f, -2.0f, 2.0f, white);   // a flat white plate under the feet while airborne
			}

			const float rad = 3.14159265f / 180.0f;
			const float cp = std::cos(s[3] * rad), sp = std::sin(s[3] * rad);
			const float cy = std::cos(s[4] * rad), sy = std::sin(s[4] * rad);
			const vec3 eye = {s[0], s[1], s[2] + eye_z};
			const vec3 tip = {eye.x + cp * cy * 70.0f, eye.y + cp * sy * 70.0f, eye.z - sp * 70.0f};
			edge(eye, tip, white);
			edge(feet, {s[0], s[1], s[2] + 400.0f}, tr.color);

			const vec3 c = to_camera({s[0], s[1], s[2] + 90.0f});
			if (c.z >= 8.0f)
			{
				float sx, sy2;
				to_screen(c, &sx, &sy2);
				if (sx >= 0.0f && sy2 >= 0.0f && sx <= g_w && sy2 <= g_h)
				{
					char label[48];
					snprintf(label, sizeof(label), "%s %s", tr.label, state);
					draw_text(sx - 20.0f, sy2, label, 1.5f, tr.color);
				}
			}
		}
	}

	void poll_keys()
	{
		static bool was_lbracket, was_rbracket, was_pagedown, was_delete, was_end, was_home, was_f11, was_f2, was_f3, was_f4, was_f5, was_f6, was_f7, was_f8;
		const bool pagedown = key_pressed(VK_NEXT, &was_pagedown);
		const bool delete_key = key_pressed(VK_DELETE, &was_delete);
		const bool end_key = key_pressed(VK_END, &was_end);
		const bool home = key_pressed(VK_HOME, &was_home);
		const bool f11 = key_pressed(VK_F11, &was_f11);
		const bool f2 = key_pressed(VK_F2, &was_f2);
		const bool f3 = key_pressed(VK_F3, &was_f3);
		const bool f4 = key_pressed(VK_F4, &was_f4);
		const bool f5 = key_pressed(VK_F5, &was_f5);
		const bool f6 = key_pressed(VK_F6, &was_f6);
		const bool f7 = key_pressed(VK_F7, &was_f7);
		const bool f8 = key_pressed(VK_F8, &was_f8);
		ghost_keys();
		static bool was_n1, was_n2, was_n3, was_n4;
		if (key_pressed(VK_NUMPAD1, &was_n1)) { g_pitch_sign = -g_pitch_sign; log_line("numpad 1: pitch sign now %d", static_cast<int>(g_pitch_sign)); }
		if (key_pressed(VK_NUMPAD2, &was_n2)) { g_yaw_sign = -g_yaw_sign; log_line("numpad 2: yaw sign now %d", static_cast<int>(g_yaw_sign)); }
		if (key_pressed(VK_NUMPAD3, &was_n3)) { g_yaw_turn = (g_yaw_turn + 1) % 4; log_line("numpad 3: yaw turned by %d degrees", static_cast<int>(g_yaw_turn) * 90); }
		if (key_pressed(VK_NUMPAD4, &was_n4)) { g_smooth_on = !g_smooth_on; log_line("numpad 4: smoothing %s", g_smooth_on ? "on" : "off"); }

		if (f8)
		{
			g_overlay_on = !g_overlay_on;
			log_line("F8: overlay %s", g_overlay_on ? "ON" : "OFF");
			if (g_overlay_on)
			{
				g_help_until = now_seconds() + 12.0;
			}
		}

		if (end_key || g_help_requested)
		{
			g_help_requested = false;
			const double now = now_seconds();
			g_help_until = g_help_until > now ? 0.0 : 1.0e15;
		}

		if (pagedown)
		{
			g_collision_range = (g_collision_range + 1) % 3;
			log_line("PageDown: collision blocks range %d", static_cast<int>(g_collision_range));
		}

		if (delete_key)
		{
			g_collision_on = !g_collision_on;
			g_collision_refresh = g_collision_on;
			log_line("Delete: level collision blocks %s", g_collision_on ? "ON" : "OFF");
		}

		if (key_pressed(VK_OEM_4, &was_lbracket))
		{
			g_show_mode = (g_show_mode + 1) % 5;
			log_line("[: showing %s", kShowModes[g_show_mode]);
		}

		if (key_pressed(VK_OEM_6, &was_rbracket))
		{
			g_leave_on = !g_leave_on;
			log_line("]: leaving-area boxes %s", g_leave_on ? "ON" : "OFF");
		}

		if (home)
		{
			g_brushes_on = !g_brushes_on;
			log_line("Home: moving brush models and volumes %s", g_brushes_on ? "ON" : "OFF");
		}

		if (f11)
		{
			g_events_on = !g_events_on;
			log_line("F11: events %s", g_events_on ? "ON" : "OFF");
		}

		if (f2)
		{
			g_labels_on = !g_labels_on;
			log_line("F2: labels %s", g_labels_on ? "ON" : "OFF");
		}

		if (f3)
		{
			g_source = (g_source + 1) % SOURCE_COUNT;
			log_line("F3: camera source %d", static_cast<int>(g_source));
		}

		if (f4)
		{
			g_filter_on = !g_filter_on;
			log_line("F4: event filter %s", g_filter_on ? "ON (noise hidden)" : "OFF (everything shown)");
		}

		if (f5) g_fov_style = (g_fov_style + 1) % 3;
		if (f6) g_thick_level = (g_thick_level + 1) % 3;
		if (f7) g_dist_level = (g_dist_level + 1) % 4;
		if (f5 || f6 || f7) g_log_settings = true;
	}

	// True only while a mission is running (slot 0 of the entity list is the player).
	bool in_mission()
	{
		unsigned short id = 0;
		if (!read_bytes(player_entity() + OFF_CLASSNAME, &id, sizeof(id)) || id == 0)
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

	// The game updates your position and view angles only a few dozen times per second, but
	// the picture is drawn much faster, so raw values make the boxes stutter. We watch how the
	// numbers change and carry them forward in between updates.
	struct motion_sample
	{
		float origin[3];
		float pitch;
		float yaw;
		double time;
		bool valid;
	};

	motion_sample g_older = {};
	motion_sample g_newer = {};

	float wrap_angle_difference(float delta)
	{
		while (delta > 180.0f) delta -= 360.0f;
		while (delta < -180.0f) delta += 360.0f;
		return delta;
	}

	void smooth_camera(float* origin, float* pitch, float* yaw)
	{
		const double now = now_seconds();

		const bool changed = !g_newer.valid || g_newer.origin[0] != origin[0] || g_newer.origin[1] != origin[1] ||
			g_newer.origin[2] != origin[2] || g_newer.pitch != *pitch || g_newer.yaw != *yaw;

		if (changed)
		{
			// A big jump (new mission, teleport, cutscene cut) must not be "carried forward".
			const bool jump = g_newer.valid && (std::fabs(g_newer.origin[0] - origin[0]) > 400.0f ||
				std::fabs(g_newer.origin[1] - origin[1]) > 400.0f || std::fabs(g_newer.origin[2] - origin[2]) > 400.0f);

			g_older = jump ? motion_sample{} : g_newer;
			g_newer.origin[0] = origin[0];
			g_newer.origin[1] = origin[1];
			g_newer.origin[2] = origin[2];
			g_newer.pitch = *pitch;
			g_newer.yaw = *yaw;
			g_newer.time = now;
			g_newer.valid = true;
		}

		if (!g_older.valid)
		{
			return;
		}

		double interval = g_newer.time - g_older.time;
		if (interval < 0.004 || interval > 0.12)
		{
			return;
		}

		float factor = static_cast<float>((now - g_newer.time) / interval);
		if (factor < 0.0f) factor = 0.0f;
		if (factor > 1.0f) factor = 1.0f;

		for (int i = 0; i < 3; ++i)
		{
			origin[i] = g_newer.origin[i] + (g_newer.origin[i] - g_older.origin[i]) * factor;
		}

		*pitch = g_newer.pitch + (g_newer.pitch - g_older.pitch) * factor;
		*yaw = g_newer.yaw + wrap_angle_difference(g_newer.yaw - g_older.yaw) * factor;
	}

	// Fills g_verts with everything to draw this picture. Returns false if there is nothing to draw.
	bool build_picture(const float width, const float height)
	{
		g_w = width;
		g_h = height;
		g_vert_count = 0;

		unsigned long long pointer = 0;
		if (!read_bytes(player_entity() + OFF_CLIENT, &pointer, sizeof(pointer)) || pointer < 0x140000000ull ||
			pointer > 0x7FFFFFFE0000ull)
		{
			return false;
		}

		float origin[3];
		float pitch = 0.0f;
		float yaw = 0.0f;
		int source_index = g_source;
		bool have_camera = false;
		float eye_height = EYE_HEIGHT;
		read_bytes(static_cast<uintptr_t>(pointer) + 0x4C, &g_self_gt, sizeof(g_self_gt));

		if (source_index == 0)
		{
			have_camera = read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_ORIGIN, origin, sizeof(origin)) &&
				read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_PITCH, &pitch, sizeof(pitch)) &&
				read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_YAW, &yaw, sizeof(yaw));
		}
		else
		{
			// These already include the eye height.
			have_camera = read_source(source_index, origin, &pitch, &yaw);
			eye_height = 0.0f;
			if (!have_camera || !(std::fabs(origin[0]) < 500000.0f && std::fabs(origin[1]) < 500000.0f && std::fabs(origin[2]) < 500000.0f && std::fabs(pitch) <= 91.0f))
			{
				// Not valid right now (loading screen, other mission): use the player data instead.
				source_index = 0;
				eye_height = EYE_HEIGHT;
				have_camera = read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_ORIGIN, origin, sizeof(origin)) &&
					read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_PITCH, &pitch, sizeof(pitch)) &&
					read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_YAW, &yaw, sizeof(yaw));
			}
		}

		if (!have_camera)
		{
			return false;
		}

		if (!(std::fabs(origin[0]) < 500000.0f && std::fabs(origin[1]) < 500000.0f && std::fabs(origin[2]) < 500000.0f &&
			std::fabs(pitch) <= 91.0f && std::fabs(yaw) <= 720.0f))
		{
			return false;
		}

		pitch *= static_cast<float>(g_pitch_sign);
		yaw = yaw * static_cast<float>(g_yaw_sign) + static_cast<float>(g_yaw_turn) * 90.0f;
		if (source_index == 0 && g_smooth_on)
		{
			smooth_camera(origin, &pitch, &yaw);
		}

		if (g_frame_counter % 120 == 1)
		{
			read_dvar_float("cg_fov", &g_fov);
			if (!(g_fov >= 20.0f && g_fov <= 160.0f)) g_fov = 65.0f;
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
		g_eye = {origin[0], origin[1], origin[2] + eye_height};
		g_fwd = {cp * cy, cp * sy, -sp};
		g_right = {sy, -cy, 0.0f};
		g_up = {sp * cy, sp * sy, cp};

		const float aspect = g_w / g_h;
		const float half_fov = g_fov * 0.5f * rad;
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

		// Little squares in the corner show the F7 / F5 / F6 settings.
		for (int row = 0; row < 3; ++row)
		{
			const int count = row == 0 ? g_dist_level : (row == 1 ? g_fov_style : g_thick_level);
			const float* color = row == 0 ? kColors[0] : (row == 1 ? kColors[3] : kColors[4]);
			for (int i = 0; i <= count; ++i)
			{
				add_rect(10.0f + static_cast<float>(i) * 18.0f, 10.0f + static_cast<float>(row) * 18.0f, 12.0f, 12.0f,
				         color);
			}
		}

		for (int i = 0; i <= g_source; ++i)
		{
			add_rect(10.0f + static_cast<float>(i) * 18.0f, 10.0f + 3 * 18.0f, 12.0f, 12.0f, kColors[5]);
		}

		if (g_show_mode != 0 || !g_leave_on)
		{
			char mode_text[96];
			snprintf(mode_text, sizeof(mode_text), "SHOWING: %s%s", kShowModes[g_show_mode], g_leave_on ? "" : "  (LEAVE-AREA OFF)");
			const float cyan[4] = {0.2f, 1.0f, 1.0f, 1.0f};
			draw_text(10.0f, 84.0f, mode_text, g_h >= 1200.0f ? 2.0f : 1.0f, cyan);
		}

		if (g_ghost_only && g_ghost_on)
		{
			ghost_draw(now_seconds(), origin);
			{
				const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
				char tag[48];
				snprintf(tag, sizeof(tag), "OVERLAY v%s GHOST ONLY", ADDON_VERSION);
				draw_text(40.0f, 4.0f, tag, g_h >= 1200.0f ? 2.0f : 1.0f, white);
			}

			return g_vert_count > 0;
		}

		hit hits[MAX_BOXES];
		const int count = find_triggers({origin[0], origin[1], origin[2] + 30.0f - (eye_height == 0.0f ? EYE_HEIGHT : 0.0f)}, kDistances[g_dist_level], hits, MAX_BOXES);

		for (int i = count - 1; i >= 0; --i)
		{
			draw_box(hits[i]);
		}

		if (g_collision_on)
		{
			draw_collision_blocks({origin[0], origin[1], origin[2] + 30.0f - (eye_height == 0.0f ? EYE_HEIGHT : 0.0f)});
		}

		if (g_events_on)
		{
			draw_events();
		}

		ghost_draw(now_seconds(), origin);

		if (g_help_until > now_seconds())
		{
			static const char* const kHelp[] = {
				"F8 overlay on/off", "F7 distance", "F6 line thickness", "F5 field of view style", "F3 camera source",
				"F2 labels", "F11 script events", "F4 event filter", "Home brush models", "[ show all/green/pink/magenta/leave-area", "] leave-area boxes on/off", "F9 save data files",
				"Num5 ghost replay (ghost.txt + route.txt)", "Num6 pause  Num7/8 speed  Num9 restart", "Num0 whole path  Num+/- jump 10s", "Num/ ghost only (hide triggers)", "Num. ghost follows my game clock", "Delete level collision blocks", "PageDown collision range", "PageUp save collision data", "End this help (or console: overlay_help)"};
			const float scale = g_h >= 1200.0f ? 1.5f : 1.0f;
			const float line = 10.0f * scale + 2.0f;
			const float top = g_h - 14.0f - line * static_cast<float>(sizeof(kHelp) / sizeof(kHelp[0]));
			const float dim[4] = {0.85f, 0.85f, 0.85f, 0.7f};
			const float back[4] = {0.0f, 0.0f, 0.0f, 0.4f};
			add_rect(8.0f, top - 6.0f, 330.0f * scale / 1.5f + 20.0f, g_h - top - 2.0f, back);
			for (size_t i = 0; i < sizeof(kHelp) / sizeof(kHelp[0]); ++i)
			{
				draw_text(16.0f, top + line * static_cast<float>(i), kHelp[i], scale, dim);
			}
		}

		if (g_labels_on)
		{
			const int label_count = count < 20 ? count : 20;
			for (int i = label_count - 1; i >= 0; --i)
			{
				if (hits[i].dist > kDistances[g_dist_level])
				{
					continue;
				}

				draw_label(hits[i], color_for(hits[i]));
			}
		}

		update_flag_learning(hits, count);
		draw_inside_panel(hits, count);
		{
			const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
			char tag[48];
			snprintf(tag, sizeof(tag), "OVERLAY v%s P%c Y%c T%d S%c", ADDON_VERSION, g_pitch_sign > 0 ? '+' : '-', g_yaw_sign > 0 ? '+' : '-', static_cast<int>(g_yaw_turn) * 90, g_smooth_on ? '1' : '0');
			draw_text(40.0f, 4.0f, tag, g_h >= 1200.0f ? 2.0f : 1.0f, white);
		}

		if (g_log_settings)
		{
			g_log_settings = false;
			log_line("settings: dist=%d fovstyle=%d thickness=%d | picture %.0fx%.0f fov %.1f | eye %.0f %.0f %.0f pitch %.1f yaw %.1f | %d triggers near, %d vertices",
			         static_cast<int>(g_dist_level), static_cast<int>(g_fov_style), static_cast<int>(g_thick_level),
			         g_w, g_h, g_fov, g_eye.x, g_eye.y, g_eye.z, pitch, yaw, count, g_vert_count);
		}

		return g_vert_count > 0;
	}

	// ---- Direct3D 11 -----------------------------------------------------------------

	const char* kShaderText =
		"struct VSIn { float2 p : POSITION; float4 c : COLOR; };\n"
		"struct VSOut { float4 p : SV_POSITION; float4 c : COLOR; };\n"
		"VSOut vs_main(VSIn i) { VSOut o; o.p = float4(i.p, 0.0, 1.0); o.c = i.c; return o; }\n"
		"float4 ps_main(VSOut i) : SV_Target { return i.c; }\n";

	ID3D11Device* g_device = nullptr;
	ID3D11VertexShader* g_vs = nullptr;
	ID3D11PixelShader* g_ps = nullptr;
	ID3D11InputLayout* g_layout = nullptr;
	ID3D11Buffer* g_buffer = nullptr;
	ID3D11BlendState* g_blend = nullptr;
	ID3D11RasterizerState* g_raster = nullptr;
	ID3D11DepthStencilState* g_depth = nullptr;
	bool g_first_draw_logged = false;
	bool g_init_failed_logged = false;

	template <typename T>
	void release(T*& pointer)
	{
		if (pointer)
		{
			pointer->Release();
			pointer = nullptr;
		}
	}

	void release_resources()
	{
		release(g_vs);
		release(g_ps);
		release(g_layout);
		release(g_buffer);
		release(g_blend);
		release(g_raster);
		release(g_depth);
		release(g_device);
	}

	bool create_resources(ID3D11Device* device)
	{
		const HMODULE compiler_dll = LoadLibraryA("d3dcompiler_47.dll");
		if (!compiler_dll)
		{
			log_line("d3d: d3dcompiler_47.dll not found");
			return false;
		}

		const auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(compiler_dll, "D3DCompile"));
		if (!compile)
		{
			log_line("d3d: D3DCompile not found");
			return false;
		}

		ID3DBlob* vs_blob = nullptr;
		ID3DBlob* ps_blob = nullptr;
		ID3DBlob* errors = nullptr;

		const size_t text_size = strlen(kShaderText);
		HRESULT hr = compile(kShaderText, text_size, nullptr, nullptr, nullptr, "vs_main", "vs_4_0", 0, 0, &vs_blob, &errors);
		if (FAILED(hr) || !vs_blob)
		{
			log_line("d3d: vertex shader failed to compile (0x%08lX) %s", static_cast<unsigned long>(hr),
			         errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
			release(errors);
			return false;
		}

		hr = compile(kShaderText, text_size, nullptr, nullptr, nullptr, "ps_main", "ps_4_0", 0, 0, &ps_blob, &errors);
		if (FAILED(hr) || !ps_blob)
		{
			log_line("d3d: pixel shader failed to compile (0x%08lX) %s", static_cast<unsigned long>(hr),
			         errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
			release(errors);
			release(vs_blob);
			return false;
		}

		bool ok = SUCCEEDED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_vs)) &&
			SUCCEEDED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &g_ps));

		if (ok)
		{
			const D3D11_INPUT_ELEMENT_DESC elements[2] = {
				{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
				{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
			};
			ok = SUCCEEDED(device->CreateInputLayout(elements, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &g_layout));
		}

		release(vs_blob);
		release(ps_blob);
		release(errors);

		if (ok)
		{
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = sizeof(vertex) * MAX_VERTS;
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			ok = SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_buffer));
		}

		if (ok)
		{
			D3D11_BLEND_DESC blend = {};
			blend.RenderTarget[0].BlendEnable = TRUE;
			blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			ok = SUCCEEDED(device->CreateBlendState(&blend, &g_blend));
		}

		if (ok)
		{
			D3D11_RASTERIZER_DESC raster = {};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = TRUE;
			ok = SUCCEEDED(device->CreateRasterizerState(&raster, &g_raster));
		}

		if (ok)
		{
			D3D11_DEPTH_STENCIL_DESC depth = {};
			depth.DepthEnable = FALSE;
			depth.StencilEnable = FALSE;
			ok = SUCCEEDED(device->CreateDepthStencilState(&depth, &g_depth));
		}

		if (!ok)
		{
			log_line("d3d: could not create drawing resources");
		}

		return ok;
	}

	// Everything the game had set up on the graphics card, so we can put it back afterwards.
	struct saved_state
	{
		UINT viewport_count;
		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		UINT scissor_count;
		D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		ID3D11RasterizerState* raster;
		ID3D11BlendState* blend;
		FLOAT blend_factor[4];
		UINT sample_mask;
		ID3D11DepthStencilState* depth;
		UINT stencil_ref;
		ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
		ID3D11DepthStencilView* depth_view;
		D3D11_PRIMITIVE_TOPOLOGY topology;
		ID3D11InputLayout* layout;
		ID3D11Buffer* vertex_buffer;
		UINT stride;
		UINT offset;
		ID3D11VertexShader* vs;
		ID3D11PixelShader* ps;
		ID3D11GeometryShader* gs;
		ID3D11HullShader* hs;
		ID3D11DomainShader* ds;
	};

	void save_state(ID3D11DeviceContext* context, saved_state& s)
	{
		memset(&s, 0, sizeof(s));
		s.viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		s.scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		context->RSGetViewports(&s.viewport_count, s.viewports);
		context->RSGetScissorRects(&s.scissor_count, s.scissors);
		context->RSGetState(&s.raster);
		context->OMGetBlendState(&s.blend, s.blend_factor, &s.sample_mask);
		context->OMGetDepthStencilState(&s.depth, &s.stencil_ref);
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.targets, &s.depth_view);
		context->IAGetPrimitiveTopology(&s.topology);
		context->IAGetInputLayout(&s.layout);
		context->IAGetVertexBuffers(0, 1, &s.vertex_buffer, &s.stride, &s.offset);
		context->VSGetShader(&s.vs, nullptr, nullptr);
		context->PSGetShader(&s.ps, nullptr, nullptr);
		context->GSGetShader(&s.gs, nullptr, nullptr);
		context->HSGetShader(&s.hs, nullptr, nullptr);
		context->DSGetShader(&s.ds, nullptr, nullptr);
	}

	void restore_state(ID3D11DeviceContext* context, saved_state& s)
	{
		context->RSSetViewports(s.viewport_count, s.viewports);
		context->RSSetScissorRects(s.scissor_count, s.scissors);
		context->RSSetState(s.raster);
		context->OMSetBlendState(s.blend, s.blend_factor, s.sample_mask);
		context->OMSetDepthStencilState(s.depth, s.stencil_ref);
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.targets, s.depth_view);
		context->IASetPrimitiveTopology(s.topology);
		context->IASetInputLayout(s.layout);
		context->IASetVertexBuffers(0, 1, &s.vertex_buffer, &s.stride, &s.offset);
		context->VSSetShader(s.vs, nullptr, 0);
		context->PSSetShader(s.ps, nullptr, 0);
		context->GSSetShader(s.gs, nullptr, 0);
		context->HSSetShader(s.hs, nullptr, 0);
		context->DSSetShader(s.ds, nullptr, 0);

		release(s.raster);
		release(s.blend);
		release(s.depth);
		for (auto& target : s.targets)
		{
			release(target);
		}

		release(s.depth_view);
		release(s.layout);
		release(s.vertex_buffer);
		release(s.vs);
		release(s.ps);
		release(s.gs);
		release(s.hs);
		release(s.ds);
	}

	void draw_overlay(IDXGISwapChain* swap_chain)
	{
		poll_keys();
		++g_frame_counter;
		rate_test_tick();

		if (!g_overlay_on || !in_mission())
		{
			return;
		}

		ID3D11Device* device = nullptr;
		if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device)
		{
			return;
		}

		if (device != g_device)
		{
			release_resources();
			if (!create_resources(device))
			{
				if (!g_init_failed_logged)
				{
					g_init_failed_logged = true;
					log_line("overlay: could not set up drawing, giving up");
				}

				g_overlay_on = false;
				release_resources();
				release(device);
				return;
			}

			g_device = device;
			g_device->AddRef();
		}

		ID3D11Texture2D* back_buffer = nullptr;
		if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer))) || !back_buffer)
		{
			release(device);
			return;
		}

		D3D11_TEXTURE2D_DESC texture_desc;
		back_buffer->GetDesc(&texture_desc);

		if (build_picture(static_cast<float>(texture_desc.Width), static_cast<float>(texture_desc.Height)))
		{
			ID3D11RenderTargetView* target = nullptr;
			if (SUCCEEDED(device->CreateRenderTargetView(back_buffer, nullptr, &target)) && target)
			{
				ID3D11DeviceContext* context = nullptr;
				device->GetImmediateContext(&context);

				D3D11_MAPPED_SUBRESOURCE mapped;
				if (context && SUCCEEDED(context->Map(g_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				{
					memcpy(mapped.pData, g_verts, sizeof(vertex) * static_cast<size_t>(g_vert_count));
					context->Unmap(g_buffer, 0);

					saved_state saved;
					save_state(context, saved);

					D3D11_VIEWPORT viewport = {};
					viewport.Width = static_cast<float>(texture_desc.Width);
					viewport.Height = static_cast<float>(texture_desc.Height);
					viewport.MaxDepth = 1.0f;

					const FLOAT blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
					const UINT stride = sizeof(vertex);
					const UINT offset = 0;

					context->RSSetViewports(1, &viewport);
					context->RSSetState(g_raster);
					context->OMSetBlendState(g_blend, blend_factor, 0xFFFFFFFF);
					context->OMSetDepthStencilState(g_depth, 0);
					context->OMSetRenderTargets(1, &target, nullptr);
					context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					context->IASetInputLayout(g_layout);
					context->IASetVertexBuffers(0, 1, &g_buffer, &stride, &offset);
					context->VSSetShader(g_vs, nullptr, 0);
					context->PSSetShader(g_ps, nullptr, 0);
					context->GSSetShader(nullptr, nullptr, 0);
					context->HSSetShader(nullptr, nullptr, 0);
					context->DSSetShader(nullptr, nullptr, 0);
					context->Draw(static_cast<UINT>(g_vert_count), 0);

					restore_state(context, saved);

					if (!g_first_draw_logged)
					{
						g_first_draw_logged = true;
						log_line("overlay: first picture drawn (%u x %u, format %d, %d vertices)", texture_desc.Width,
						         texture_desc.Height, static_cast<int>(texture_desc.Format), g_vert_count);
					}
				}

				release(context);
			}

			release(target);
		}

		release(back_buffer);
		release(device);
	}

	void safe_draw_overlay(IDXGISwapChain* swap_chain)
	{
		__try
		{
			draw_overlay(swap_chain);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			++g_failures;
			log_line("overlay: drawing hit an error (%d of 5)", g_failures);
			if (g_failures >= 5)
			{
				g_overlay_on = false;
				log_line("overlay: too many errors, switched itself off");
			}
		}
	}

	// ---- hooking the moment the picture is shown ---------------------------------------

	using present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	using present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

	present_t g_original_present = nullptr;
	present1_t g_original_present1 = nullptr;

	HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags)
	{
		if (!g_in_present && (flags & DXGI_PRESENT_TEST) == 0)
		{
			g_in_present = true;
			safe_draw_overlay(swap_chain);
			g_in_present = false;
		}

		return g_original_present(swap_chain, sync_interval, flags);
	}

	HRESULT STDMETHODCALLTYPE hooked_present1(IDXGISwapChain1* swap_chain, UINT sync_interval, UINT flags,
	                                          const DXGI_PRESENT_PARAMETERS* parameters)
	{
		if (!g_in_present && (flags & DXGI_PRESENT_TEST) == 0)
		{
			g_in_present = true;
			safe_draw_overlay(swap_chain);
			g_in_present = false;
		}

		return g_original_present1(swap_chain, sync_interval, flags, parameters);
	}

	// Makes a throw-away window and swap chain just to learn where the game's "show the picture"
	// function lives in memory.
	bool find_present_functions(void** present, void** present1)
	{
		const HMODULE d3d11_dll = LoadLibraryA("d3d11.dll");
		if (!d3d11_dll)
		{
			log_line("hook: d3d11.dll not found");
			return false;
		}

		const auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
			GetProcAddress(d3d11_dll, "D3D11CreateDeviceAndSwapChain"));
		if (!create)
		{
			log_line("hook: D3D11CreateDeviceAndSwapChain not found");
			return false;
		}

		WNDCLASSEXA wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = GetModuleHandleA(nullptr);
		wc.lpszClassName = "s1x_overlay_probe";
		RegisterClassExA(&wc);

		const HWND window = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr,
		                                    wc.hInstance, nullptr);
		if (!window)
		{
			log_line("hook: could not create probe window");
			return false;
		}

		DXGI_SWAP_CHAIN_DESC desc = {};
		desc.BufferCount = 1;
		desc.BufferDesc.Width = 100;
		desc.BufferDesc.Height = 100;
		desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.OutputWindow = window;
		desc.SampleDesc.Count = 1;
		desc.Windowed = TRUE;
		desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		const D3D_FEATURE_LEVEL levels[3] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
		D3D_FEATURE_LEVEL level;
		IDXGISwapChain* swap_chain = nullptr;
		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;

		const HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &desc,
		                          &swap_chain, &device, &level, &context);
		bool ok = false;
		if (SUCCEEDED(hr) && swap_chain)
		{
			void** table = *reinterpret_cast<void***>(swap_chain);
			*present = table[8];

			IDXGISwapChain1* swap_chain1 = nullptr;
			if (SUCCEEDED(swap_chain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swap_chain1))) &&
				swap_chain1)
			{
				void** table1 = *reinterpret_cast<void***>(swap_chain1);
				*present1 = table1[22];
				swap_chain1->Release();
			}

			ok = true;
		}
		else
		{
			log_line("hook: could not create probe swap chain (0x%08lX)", static_cast<unsigned long>(hr));
		}

		release(swap_chain);
		release(context);
		release(device);
		DestroyWindow(window);
		UnregisterClassA(wc.lpszClassName, wc.hInstance);
		return ok;
	}

	bool install_hook()
	{
		void* present = nullptr;
		void* present1 = nullptr;
		if (!find_present_functions(&present, &present1))
		{
			return false;
		}

		if (MH_Initialize() != MH_OK)
		{
			log_line("hook: MinHook failed to start");
			return false;
		}

		bool any = false;
		if (MH_CreateHook(present, reinterpret_cast<LPVOID>(&hooked_present), reinterpret_cast<LPVOID*>(&g_original_present)) ==
				MH_OK &&
			MH_EnableHook(present) == MH_OK)
		{
			log_line("hook: Present installed");
			any = true;
		}
		else
		{
			log_line("hook: could not hook Present");
		}

		if (present1 && present1 != present)
		{
			if (MH_CreateHook(present1, reinterpret_cast<LPVOID>(&hooked_present1),
			                  reinterpret_cast<LPVOID*>(&g_original_present1)) == MH_OK &&
				MH_EnableHook(present1) == MH_OK)
			{
				log_line("hook: Present1 installed");
				any = true;
			}
			else
			{
				log_line("hook: could not hook Present1");
			}
		}

		return any;
	}

	// ---- geometry search (Insert key) -------------------------------------------------------
	// Looks through the game's memory for the exact box sizes of a few known trigger volumes. Wherever
	// those numbers also appear in the level's collision data, we learn how that data is laid out.
	struct geo_target
	{
		int slot;
		float mins[3];
		float maxs[3];
		float lmins[3];
		float lmaxs[3];
		char name[40];
	};

	struct geo_hit
	{
		int target;
		uintptr_t address;
	};

	constexpr int GEO_TARGETS = 8;
	constexpr int GEO_MAX_HITS = 400;

	bool near_value(const float a, const float b)
	{
		const float d = a - b;
		return d < 0.06f && d > -0.06f;
	}

	void geometry_search()
	{
		log_line("geometry search: started (this can take up to a minute; the game may stutter)");

		geo_target targets[GEO_TARGETS];
		int target_count = 0;
		int last_taken = -100;
		for (int index = 0; index < MAX_ENTITIES && target_count < GEO_TARGETS; ++index)
		{
			unsigned char buffer[0x1C0];
			if (!read_bytes(SP_G_ENTITIES + static_cast<uintptr_t>(index) * g_stride, buffer, sizeof(buffer)))
			{
				break;
			}

			unsigned short class_id = 0;
			memcpy(&class_id, buffer + OFF_CLASSNAME, sizeof(class_id));
			if (class_id == 0 || classify(class_id) != 2)
			{
				continue;
			}

			float b[6];
			memcpy(b, buffer + OFF_BOUNDS, sizeof(b));
			make_world_bounds(b, buffer);
			if (!(b[3] > 16.0f && b[4] > 16.0f && b[5] > 16.0f && b[3] < 4000.0f && b[4] < 4000.0f && b[5] < 4000.0f &&
				std::fabs(b[0]) < 100000.0f && std::fabs(b[1]) < 100000.0f && std::fabs(b[2]) < 100000.0f))
			{
				continue;
			}

			if (index - last_taken < 40)
			{
				continue;
			}

			last_taken = index;
			geo_target& t = targets[target_count++];
			t.slot = index;
			for (int i = 0; i < 3; ++i)
			{
				t.mins[i] = b[i] - b[3 + i];
				t.maxs[i] = b[i] + b[3 + i];
				t.lmins[i] = -b[3 + i];
				t.lmaxs[i] = b[3 + i];
			}

			class_name(class_id, t.name, sizeof(t.name));
		}

		if (target_count == 0)
		{
			log_line("geometry search: no suitable trigger volumes found (is a mission loaded?)");
			return;
		}

		static geo_hit hits[GEO_MAX_HITS];
		int hit_count = 0;

		// Skip our own copy of the numbers.
		MEMORY_BASIC_INFORMATION own = {};
		VirtualQuery(reinterpret_cast<const void*>(&geometry_search), &own, sizeof(own));
		const uintptr_t own_base = reinterpret_cast<uintptr_t>(own.AllocationBase);
		MEMORY_BASIC_INFORMATION stack_info = {};
		VirtualQuery(reinterpret_cast<const void*>(&target_count), &stack_info, sizeof(stack_info));
		const uintptr_t stack_base = reinterpret_cast<uintptr_t>(stack_info.AllocationBase);

		constexpr size_t CHUNK = 4u * 1024u * 1024u;
		unsigned char* buffer = static_cast<unsigned char*>(malloc(CHUNK + 64));
		if (!buffer)
		{
			return;
		}

		const double started = now_seconds();
		double megabytes = 0.0;
		uintptr_t address = 0x10000;
		const uintptr_t limit = 0x7FFFFFFE0000ull;

		while (address < limit && now_seconds() - started < 90.0 && hit_count < GEO_MAX_HITS)
		{
			MEMORY_BASIC_INFORMATION info = {};
			if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0)
			{
				break;
			}

			const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
			const uintptr_t end = base + info.RegionSize;
			const DWORD protect = info.Protect & 0xFF;
			const bool readable = info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
				(protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
					protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE) &&
				info.Type != MEM_MAPPED;
			const bool ours = reinterpret_cast<uintptr_t>(info.AllocationBase) == own_base ||
				reinterpret_cast<uintptr_t>(info.AllocationBase) == stack_base;

			if (readable && !ours)
			{
				for (uintptr_t position = base < address ? address : base; position < end; position += CHUNK)
				{
					size_t length = end - position;
					if (length > CHUNK)
					{
						length = CHUNK;
					}

					size_t copy = length;
					if (position + length + 24 <= end)
					{
						copy = length + 24;
					}

					bool ok = true;
					__try
					{
						memcpy(buffer, reinterpret_cast<const void*>(position), copy);
					}
					__except (EXCEPTION_EXECUTE_HANDLER)
					{
						ok = false;
					}

					if (!ok)
					{
						continue;
					}

					megabytes += static_cast<double>(length) / 1048576.0;
					for (size_t offset = 0; offset + 24 <= copy && offset < length; offset += 4)
					{
						float f[6];
						memcpy(f, buffer + offset, sizeof(f));
						for (int t = 0; t < target_count; ++t)
						{
							if (near_value(f[0], targets[t].mins[0]) && near_value(f[1], targets[t].mins[1]) &&
								near_value(f[2], targets[t].mins[2]) && near_value(f[3], targets[t].maxs[0]) &&
								near_value(f[4], targets[t].maxs[1]) && near_value(f[5], targets[t].maxs[2]))
							{
								if (hit_count < GEO_MAX_HITS)
								{
									hits[hit_count++] = {t * 2, position + offset};
								}
							}
							else if (near_value(f[0], targets[t].lmins[0]) && near_value(f[1], targets[t].lmins[1]) &&
								near_value(f[2], targets[t].lmins[2]) && near_value(f[3], targets[t].lmaxs[0]) &&
								near_value(f[4], targets[t].lmaxs[1]) && near_value(f[5], targets[t].lmaxs[2]))
							{
								if (hit_count < GEO_MAX_HITS)
								{
									hits[hit_count++] = {t * 2 + 1, position + offset};
								}
							}
						}
					}
				}
			}

			address = end;
		}

		free(buffer);
		log_line("geometry search: scanned %.0f MB in %.1f s, %d matches", megabytes, now_seconds() - started, hit_count);

		char path[MAX_PATH];
		make_path(path, "geometry_search.txt");
		FILE* f = nullptr;
		if (fopen_s(&f, path, "w") != 0 || !f)
		{
			return;
		}

		fprintf(f, "geometry search: %d matches\n", hit_count);
		for (int t = 0; t < target_count; ++t)
		{
			fprintf(f, "target %d: slot %d %s mins %.3f %.3f %.3f maxs %.3f %.3f %.3f\n", t, targets[t].slot, targets[t].name,
			        targets[t].mins[0], targets[t].mins[1], targets[t].mins[2], targets[t].maxs[0], targets[t].maxs[1],
			        targets[t].maxs[2]);
		}

		for (int i = 0; i < hit_count; ++i)
		{
			MEMORY_BASIC_INFORMATION info = {};
			VirtualQuery(reinterpret_cast<const void*>(hits[i].address), &info, sizeof(info));
			fprintf(f, "\nHIT %d target*2+variant %d (0 = world position, 1 = centered on itself) at 0x%llX  region 0x%llX size 0x%llX type 0x%lX protect 0x%lX\n", i, hits[i].target,
			        static_cast<unsigned long long>(hits[i].address),
			        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info.BaseAddress)),
			        static_cast<unsigned long long>(info.RegionSize), static_cast<unsigned long>(info.Type),
			        static_cast<unsigned long>(info.Protect));

			// 0x80 bytes before to 0xA0 after, as hex and as numbers.
			for (int row = -8; row < 10; ++row)
			{
				unsigned char line[16];
				const uintptr_t where = hits[i].address + static_cast<intptr_t>(row) * 16;
				if (!read_bytes(where, line, sizeof(line)))
				{
					fprintf(f, "  %+04d: ??\n", row * 16);
					continue;
				}

				float v[4];
				memcpy(v, line, sizeof(v));
				fprintf(f, "  %+04d:", row * 16);
				for (const unsigned char byte : line)
				{
					fprintf(f, " %02X", byte);
				}

				fprintf(f, "   %.3f %.3f %.3f %.3f\n", v[0], v[1], v[2], v[3]);
			}
		}

		fclose(f);
		log_line("geometry search: wrote geometry_search.txt");
	}

	// ---- level collision data dump (PageUp) ------------------------------------------------
	// Asks the game for its loaded "clipmap" (the level's collision data) and saves what it looks like
	// so the layout can be worked out.
	constexpr int ASSET_TYPE_CLIPMAP_INDEX = 24;
	uintptr_t SP_DB_ENUM_XASSETS = 0x14017D7C0;
	void* g_found_assets[16];
	int g_found_count = 0;

	void found_asset_callback(void* header, void*)
	{
		if (g_found_count < 16)
		{
			g_found_assets[g_found_count++] = header;
		}
	}

	void dump_bytes(FILE* f, const uintptr_t address, const size_t length, const char* indent)
	{
		for (size_t offset = 0; offset < length; offset += 16)
		{
			unsigned char row[16];
			if (!read_bytes(address + offset, row, sizeof(row)))
			{
				fprintf(f, "%s%04zX: ??\n", indent, offset);
				continue;
			}

			int ints[4];
			memcpy(ints, row, sizeof(ints));
			fprintf(f, "%s%04zX:", indent, offset);
			for (const unsigned char byte : row)
			{
				fprintf(f, " %02X", byte);
			}

			fprintf(f, "   %d %d %d %d\n", ints[0], ints[1], ints[2], ints[3]);
		}
	}

	void dump_clipmap()
	{
		log_line("collision dump: asking the game for its level collision data");
		g_found_count = 0;
		using enum_t = void (*)(int, void (*)(void*, void*), void*, bool);
		bool ok = true;
		__try
		{
			reinterpret_cast<enum_t>(SP_DB_ENUM_XASSETS)(ASSET_TYPE_CLIPMAP_INDEX, &found_asset_callback, nullptr, false);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ok = false;
		}

		if (!ok || g_found_count == 0)
		{
			log_line("collision dump: found nothing (%s)", ok ? "no clipmap loaded" : "the call failed");
			return;
		}

		char path[MAX_PATH];
		make_path(path, "clipmap_dump.txt");
		FILE* f = nullptr;
		if (fopen_s(&f, path, "w") != 0 || !f)
		{
			return;
		}

		for (int a = 0; a < g_found_count; ++a)
		{
			const uintptr_t base = reinterpret_cast<uintptr_t>(g_found_assets[a]);
			fprintf(f, "=== clipmap asset %d at 0x%llX ===\n", a, static_cast<unsigned long long>(base));
			dump_bytes(f, base, 0x200, "  ");

			int followed = 0;
			for (size_t offset = 0; offset < 0x200 && followed < 40; offset += 8)
			{
				unsigned long long value = 0;
				if (!read_bytes(base + offset, &value, sizeof(value)) || value < 0x10000ull || value > 0x7FFFFFFE0000ull)
				{
					continue;
				}

				unsigned char probe[8];
				if (!read_bytes(static_cast<uintptr_t>(value), probe, sizeof(probe)))
				{
					continue;
				}

				++followed;
				char text[40];
				const bool is_text = safe_copy_string(reinterpret_cast<const char*>(value), text, sizeof(text));
				fprintf(f, "\n  field +0x%03zX -> 0x%llX%s%s\n", offset, value, is_text ? " text: " : "", is_text ? text : "");
				if (!is_text)
				{
					dump_bytes(f, static_cast<uintptr_t>(value), 0x80, "      ");
				}
			}
		}

		fclose(f);
		log_line("collision dump: wrote clipmap_dump.txt (%d asset(s))", g_found_count);
	}

	// Saves the level's raw entity text (every trigger's script keys, such as which flag it sets).

	struct map_rec
	{
		char type[40];
		char target[40];
		char flags[96];
		float origin[3];
		bool has_origin;
	};

	map_rec g_recs[1500];
	int g_rec_count = 0;
	bool g_flag_mismatch_logged[64];

	// The map entity text is lines of  <key id> "value".  Known ids: 171 classname, 1191 targetname, 31182 and 31188 flag names.
	void parse_map_ents(const char* text, const int length)
	{
		g_rec_count = 0;
		int index = 0;
		map_rec cur = {};
		bool inside = false;
		while (index < length)
		{
			int end = index;
			while (end < length && text[end] != '\n' && text[end] != '\0')
			{
				++end;
			}

			char line[300];
			const int n = end - index < 299 ? end - index : 299;
			memcpy(line, text + index, static_cast<size_t>(n));
			line[n] = '\0';
			index = end + 1;

			if (line[0] == '{')
			{
				inside = true;
				cur = {};
				continue;
			}

			if (line[0] == '}')
			{
				if (inside && strncmp(cur.type, "trigger_", 8) == 0 && strstr(cur.type, "flag") && g_rec_count < 1500)
				{
					g_recs[g_rec_count++] = cur;
				}

				inside = false;
				continue;
			}

			if (!inside)
			{
				continue;
			}

			unsigned int key = 0;
			char value[160] = {};
			bool origin_line = false;
			if (line[0] >= '0' && line[0] <= '9')
			{
				unsigned int k = 0;
				for (const char* d = line; *d >= '0' && *d <= '9'; ++d)
				{
					k = k * 10 + static_cast<unsigned int>(*d - '0');
				}

				if (k != 171 && k != 1191 && k != 31182 && k != 31188)
				{
					// the game's string table calls the origin key "flankkills"; remember the verdict per id
					static unsigned int seen_key[64];
					static signed char seen_verdict[64];
					int slot = -1;
					for (int z = 0; z < 64; ++z)
					{
						if (seen_key[z] == k && seen_verdict[z] != 0) { slot = z; break; }
					}

					if (slot < 0)
					{
						char nm[64] = {};
						const bool is_origin = k == 742 || (class_name(k, nm, sizeof(nm)) && strcmp(nm, "flankkills") == 0);   // 742 is the origin key in the map text
						for (int z = 0; z < 64; ++z)
						{
							if (seen_verdict[z] == 0) { seen_key[z] = k; seen_verdict[z] = is_origin ? 1 : -1; slot = z; break; }
						}

						origin_line = is_origin;
					}
					else
					{
						origin_line = seen_verdict[slot] == 1;
					}
				}
			}
			const char* q = strchr(line, '"');
			if (!q || (line[0] < '0' || line[0] > '9'))
			{
				continue;
			}

			for (const char* d = line; *d >= '0' && *d <= '9'; ++d)
			{
				key = key * 10 + static_cast<unsigned int>(*d - '0');
			}

			const char* e = strrchr(line, '"');
			if (e > q)
			{
				const size_t len = static_cast<size_t>(e - q - 1) < 159 ? static_cast<size_t>(e - q - 1) : 159;
				memcpy(value, q + 1, len);
				value[len] = '\0';
			}

			if (origin_line)
			{
				float o[3] = {};
				const char* c = value;
				int got = 0;
				for (int k = 0; k < 3; ++k)
				{
					char* endp = nullptr;
					o[k] = strtof(c, &endp);
					if (endp == c) break;
					c = endp;
					++got;
				}

				if (got == 3)
				{
					memcpy(cur.origin, o, sizeof(o));
					cur.has_origin = true;
				}
			}
			else if (key == 171)
			{
				snprintf(cur.type, sizeof(cur.type), "%s", value);
			}
			else if (key == 1191)
			{
				snprintf(cur.target, sizeof(cur.target), "%s", value);
			}
			else if (key == 31182 || key == 31188)
			{
				const size_t used = strlen(cur.flags);
				snprintf(cur.flags + used, sizeof(cur.flags) - used, "%s%s", used ? " + " : "", value);
			}
		}
	}

	// Pairs each live flag trigger with its entry in the map text: first by targetname, otherwise by order within its type.
	void rebuild_slot_flags()
	{
		if (g_rec_count == 0)
		{
			return;
		}

		static char types[MAX_ENTITIES][40];
		static unsigned int names[MAX_ENTITIES];
		static float origins[MAX_ENTITIES][6];
		for (int i = 0; i < MAX_ENTITIES; ++i)
		{
			types[i][0] = '\0';
			names[i] = 0;
			unsigned char buffer[0x1C0];
			if (!read_bytes(SP_G_ENTITIES + static_cast<uintptr_t>(i) * g_stride, buffer, sizeof(buffer)))
			{
				continue;
			}

			unsigned int type_id = 0;
			memcpy(&type_id, buffer + OFF_TYPE, sizeof(type_id));
			memcpy(origins[i], buffer + 0x1C, 12);
			memcpy(origins[i] + 3, buffer + 0xDC, 12);
			memcpy(&names[i], buffer + OFF_NAME, sizeof(unsigned int));
			if (type_id > 1 && type_id < 0x10000)
			{
				class_name(type_id, types[i], sizeof(types[i]));
			}
		}

		memset(g_slot_flags, 0, sizeof(g_slot_flags));
		int matched = 0;
		int origin_matches = 0;
		for (int t = 0; t < g_rec_count; ++t)
		{
			// handled per type below
		}

		bool done[1500] = {};
		for (int i = 0; i < MAX_ENTITIES; ++i)
		{
			if (strncmp(types[i], "trigger_", 8) != 0 || !strstr(types[i], "flag"))
			{
				continue;
			}

			int live_total = 0, file_total = 0, ordinal = 0;
			for (int j = 0; j < MAX_ENTITIES; ++j)
			{
				if (strcmp(types[j], types[i]) == 0)
				{
					if (j < i) ++ordinal;
					++live_total;
				}
			}

			int pick = -1;
			int seen = 0;
			for (int r = 0; r < g_rec_count; ++r)
			{
				if (strcmp(g_recs[r].type, types[i]) == 0)
				{
					if (seen == ordinal) pick = r;
					++seen;
					++file_total;
				}
			}

			const bool order_ok = live_total == file_total;
			if (!order_ok)
			{
				unsigned int h = 0;
				for (const char* c = types[i]; *c; ++c) h = h * 31 + static_cast<unsigned char>(*c);
				if (!g_flag_mismatch_logged[h % 64])
				{
					g_flag_mismatch_logged[h % 64] = true;
					log_line("flag match: %s has %d live but %d in the map text, order matching skipped", types[i], live_total, file_total);
				}

				pick = -1;
			}

			// the position is the best key: the map text has each trigger's origin
			int by_origin = -1;
			float best = 6.0f;
			for (int r = 0; r < g_rec_count; ++r)
			{
				if (!g_recs[r].has_origin || strcmp(g_recs[r].type, types[i]) != 0)
				{
					continue;
				}

				for (int k = 0; k < 2; ++k)
				{
					const float* o = origins[i] + k * 3;
					const float d = std::fabs(o[0] - g_recs[r].origin[0]) + std::fabs(o[1] - g_recs[r].origin[1]) + std::fabs(o[2] - g_recs[r].origin[2]);
					if (d < best)
					{
						best = d;
						by_origin = r;
					}
				}
			}

			if (by_origin >= 0)
			{
				pick = by_origin;
				++origin_matches;
			}
			else if (!order_ok)
			{
				pick = -1;
			}

			// a targetname that agrees makes the pairing much safer
			char given[48] = {};
			if (names[i] > 1 && names[i] < 0x10000)
			{
				class_name(names[i], given, sizeof(given));
			}

			if (given[0])
			{
				for (int r = 0; r < g_rec_count; ++r)
				{
					if (strcmp(g_recs[r].type, types[i]) == 0 && strcmp(g_recs[r].target, given) == 0 && !done[r])
					{
						pick = r;
						break;
					}
				}
			}

			if (pick >= 0 && g_recs[pick].flags[0])
			{
				snprintf(g_slot_flags[i], sizeof(g_slot_flags[i]), "%s", g_recs[pick].flags);
				++matched;
			}
		}

		static int last_logged = -1;
		if (matched != last_logged)
		{
			last_logged = matched;
			int shown = 0;
			for (int i = 0; i < MAX_ENTITIES && shown < 6; ++i)
			{
				if (strncmp(types[i], "trigger_", 8) == 0 && strstr(types[i], "flag") && !g_slot_flags[i][0])
				{
					log_line("   unmatched #%d %s origin %.0f %.0f %.0f mid %.0f %.0f %.0f", i, types[i],
						origins[i][0], origins[i][1], origins[i][2], origins[i][3], origins[i][4], origins[i][5]);
					++shown;
				}
			}

			shown = 0;
			for (int r = 0; r < g_rec_count && shown < 6; ++r)
			{
				log_line("   map entry %s origin %.0f %.0f %.0f", g_recs[r].type, g_recs[r].origin[0], g_recs[r].origin[1], g_recs[r].origin[2]);
				++shown;
			}

			log_line("flag match: %d live flag triggers now have flag names, %d by position (map text has %d flag entries)", matched, origin_matches, g_rec_count);
			for (int i = 0; i < MAX_ENTITIES; ++i)
			{
				if (g_slot_flags[i][0])
				{
					log_line("   flags #%d %s: %s", i, types[i], g_slot_flags[i]);
				}
			}
		}
	}

	void dump_map_ents()
	{
		g_found_count = 0;
		using enum_t = void (*)(int, void (*)(void*, void*), void*, bool);
		bool ok = true;
		__try
		{
			reinterpret_cast<enum_t>(SP_DB_ENUM_XASSETS)(29, &found_asset_callback, nullptr, false);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ok = false;
		}

		if (!ok || g_found_count == 0)
		{
			log_line("map entities: nothing found");
			return;
		}

		for (int a = 0; a < g_found_count; ++a)
		{
			const uintptr_t base = reinterpret_cast<uintptr_t>(g_found_assets[a]);
			for (size_t offset = 0; offset < 0x40; offset += 8)
			{
				unsigned long long value = 0;
				if (!read_bytes(base + offset, &value, sizeof(value)) || value < 0x10000ull || value > 0x7FFFFFFE0000ull)
				{
					continue;
				}

				char head[8] = {};
				if (!read_bytes(static_cast<uintptr_t>(value), head, 2) || head[0] != '{')
				{
					continue;
				}

				int length = 0;
				read_bytes(base + offset + 8, &length, sizeof(length));
				if (length < 100 || length > 12 * 1024 * 1024)
				{
					continue;
				}

				char* text = static_cast<char*>(malloc(static_cast<size_t>(length) + 1));
				if (!text)
				{
					return;
				}

				bool good = true;
				for (int done = 0; done < length && good; done += 4096)
				{
					const int n = length - done < 4096 ? length - done : 4096;
					good = read_bytes(static_cast<uintptr_t>(value) + done, text + done, static_cast<size_t>(n));
				}

				text[length] = '\0';
				if (!good)
				{
					free(text);
					continue;
				}

				char name[64];
				sprintf_s(name, sizeof(name), "map_ents_%d.txt", a);
				char path[MAX_PATH];
				make_path(path, name);
				FILE* f = nullptr;
				if (fopen_s(&f, path, "wb") != 0 || !f)
				{
					free(text);
					return;
				}

				// Lines look like  12562 "value"  where the number is a script string id; turn it back into the key name.
				int lines = 0;
				int index = 0;
				while (index < length)
				{
					int end = index;
					while (end < length && text[end] != '\n' && text[end] != '\0')
					{
						++end;
					}

					int cursor = index;
					unsigned int number = 0;
					int digits = 0;
					while (cursor < end && text[cursor] >= '0' && text[cursor] <= '9' && digits < 9)
					{
						number = number * 10 + static_cast<unsigned int>(text[cursor] - '0');
						++cursor;
						++digits;
					}

					char key[64] = {};
					if (digits > 0 && cursor < end && text[cursor] == ' ' && false)
					{
						fprintf(f, "%s", key);
					}
					else
					{
						cursor = index;
					}

					fwrite(text + cursor, 1, static_cast<size_t>(end - cursor), f);
					fputc('\n', f);
					++lines;
					index = end + 1;
				}

				fclose(f);
				parse_map_ents(text, length);
				rebuild_slot_flags();
				free(text);
				log_line("map entities: wrote %s (%d bytes, %d lines)", name, length, lines);
				return;
			}
		}

		log_line("map entities: could not find the entity text in the asset; header looks like:");
		const uintptr_t diag_base = reinterpret_cast<uintptr_t>(g_found_assets[0]);
		for (size_t offset = 0; offset < 0x60; offset += 16)
		{
			unsigned char row[16];
			if (!read_bytes(diag_base + offset, row, sizeof(row)))
			{
				continue;
			}

			char hex[80] = {};
			for (int i = 0; i < 16; ++i)
			{
				sprintf_s(hex + i * 3, sizeof(hex) - i * 3, "%02X ", row[i]);
			}

			log_line("   +%02zX: %s", offset, hex);
		}

		for (size_t offset = 0; offset < 0x60; offset += 8)
		{
			unsigned long long value = 0;
			if (!read_bytes(diag_base + offset, &value, sizeof(value)) || value < 0x10000ull || value > 0x7FFFFFFE0000ull)
			{
				continue;
			}

			char text[49] = {};
			if (read_bytes(static_cast<uintptr_t>(value), text, 48))
			{
				for (char& c : text)
				{
					if (c != 0 && (c < 32 || c > 126)) c = '.';
				}

				log_line("   +%02zX -> 0x%llX : %s", offset, value, text);
			}
		}
	}

	// Finds the collision tree and keeps its small boxes. Also works out which of the three numbers in
	// each box is which direction by checking where the player is standing.
	void refresh_collision()
	{
		g_found_count = 0;
		using enum_t = void (*)(int, void (*)(void*, void*), void*, bool);
		bool ok = true;
		__try
		{
			reinterpret_cast<enum_t>(SP_DB_ENUM_XASSETS)(ASSET_TYPE_CLIPMAP_INDEX, &found_asset_callback, nullptr, false);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ok = false;
		}

		if (!ok || g_found_count == 0)
		{
			log_line("collision: no level collision data found");
			return;
		}

		static uintptr_t last_header = 0;
		static long long last_count = -1;
		const uintptr_t header = reinterpret_cast<uintptr_t>(g_found_assets[0]);
		long long count = 0;
		unsigned long long tree = 0;
		if (!read_bytes(header + 0x50, &count, 4) || !read_bytes(header + 0x58, &tree, 8))
		{
			return;
		}

		count &= 0xFFFFFFFFll;
		if (count <= 0 || count > 4000000 || tree < 0x10000ull)
		{
			log_line("collision: tree looks wrong (count %lld)", count);
			return;
		}

		if (header == last_header && count == last_count && g_blocks)
		{
			return;
		}

		unsigned char* raw = static_cast<unsigned char*>(malloc(static_cast<size_t>(count) * 32));
		if (!raw)
		{
			return;
		}

		bool copied = true;
		for (long long done = 0; done < count && copied; done += 4096)
		{
			const long long n = count - done < 4096 ? count - done : 4096;
			copied = read_bytes(static_cast<uintptr_t>(tree) + static_cast<size_t>(done) * 32, raw + done * 32,
			                    static_cast<size_t>(n) * 32);
		}

		if (!copied)
		{
			log_line("collision: could not copy the tree");
			free(raw);
			return;
		}

		// Player position (feet) for the direction check.
		float player[3] = {0, 0, 0};
		unsigned long long client = 0;
		bool have_player = read_bytes(player_entity() + OFF_CLIENT, &client, sizeof(client)) && client > 0x140000000ull &&
			read_bytes(static_cast<uintptr_t>(client) + OFF_CAM_ORIGIN, player, sizeof(player));

		static const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
		long long leaves = 0;
		long long hist_zero = 0;
		float lo[3] = {1e30f, 1e30f, 1e30f};
		float hi[3] = {-1e30f, -1e30f, -1e30f};
		int contain[6] = {};
		for (long long i = 0; i < count; ++i)
		{
			const unsigned char* node = raw + i * 32;
			float mid[3];
			float half[3];
			unsigned short child_count;
			memcpy(mid, node, 12);
			memcpy(&child_count, node + 14, 2);
			memcpy(half, node + 16, 12);
			if (child_count != 0)
			{
				continue;
			}

			++hist_zero;
			if (!(std::fabs(mid[0]) < 1e6f && std::fabs(mid[1]) < 1e6f && std::fabs(mid[2]) < 1e6f && half[0] >= 0.0f && half[1] >= 0.0f &&
				half[2] >= 0.0f && half[0] < 1e5f && half[1] < 1e5f && half[2] < 1e5f))
			{
				continue;
			}

			++leaves;
			for (int k = 0; k < 3; ++k)
			{
				if (mid[k] < lo[k]) lo[k] = mid[k];
				if (mid[k] > hi[k]) hi[k] = mid[k];
			}

			if (have_player)
			{
				for (int p = 0; p < 6; ++p)
				{
					bool inside = true;
					for (int axis = 0; axis < 3; ++axis)
					{
						const int source = perms[p][axis];
						const float d = std::fabs(player[axis] - mid[source]);
						if (d > half[source] + 24.0f)
						{
							inside = false;
							break;
						}
					}

					if (inside)
					{
						++contain[p];
					}
				}
			}
		}

		int best = 0;
		for (int p = 1; p < 6; ++p)
		{
			if (contain[p] > contain[best])
			{
				best = p;
			}
		}

		log_line("collision: %lld boxes, %lld small ones; range of first numbers %.0f..%.0f, %.0f..%.0f, %.0f..%.0f; player %.0f %.0f %.0f",
		         count, leaves, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], player[0], player[1], player[2]);
		log_line("collision: boxes touching the player for each direction order: %d %d %d %d %d %d (using order %d)", contain[0],
		         contain[1], contain[2], contain[3], contain[4], contain[5], best);

		block* out = static_cast<block*>(malloc(static_cast<size_t>(leaves > 0 ? leaves : 1) * sizeof(block)));
		if (!out)
		{
			free(raw);
			return;
		}

		long long written = 0;
		for (long long i = 0; i < count && written < leaves; ++i)
		{
			const unsigned char* node = raw + i * 32;
			float mid[3];
			float half[3];
			unsigned short child_count;
			memcpy(mid, node, 12);
			memcpy(&child_count, node + 14, 2);
			memcpy(half, node + 16, 12);
			if (child_count != 0 ||
				!(std::fabs(mid[0]) < 1e6f && std::fabs(mid[1]) < 1e6f && std::fabs(mid[2]) < 1e6f && half[0] >= 0.0f && half[1] >= 0.0f &&
					half[2] >= 0.0f && half[0] < 1e5f && half[1] < 1e5f && half[2] < 1e5f))
			{
				continue;
			}

			for (int axis = 0; axis < 3; ++axis)
			{
				out[written].mid[axis] = mid[perms[best][axis]];
				out[written].half[axis] = half[perms[best][axis]];
			}

			++written;
		}

		free(raw);
		last_header = header;
		last_count = count;
		g_blocks = out;          // the previous array is intentionally not freed (the picture thread may still be reading it)
		g_block_count = static_cast<int>(written);
		log_line("collision: %lld blocks ready", written);
	}

	// Console command: overlay_help
	struct console_command
	{
		console_command* next;
		const char* name;
		void (*function)();
	};

	console_command g_help_command = {};
	bool g_command_added = false;

	void help_command_function()
	{
		g_help_requested = true;
	}

	void try_add_console_command()
	{
		if (g_command_added)
		{
			return;
		}

		g_command_added = true;
		const uintptr_t SP_CMD_ADD_INTERNAL = g_is_mp ? 0x1403AF2C0 : 0x1402EDDB0;
		using add_t = void (*)(const char*, void (*)(), console_command*);
		g_help_command.name = "overlay_help";
		g_help_command.function = &help_command_function;
		bool ok = true;
		__try
		{
			reinterpret_cast<add_t>(SP_CMD_ADD_INTERNAL)("overlay_help", &help_command_function, &g_help_command);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ok = false;
		}

		log_line("console command overlay_help %s", ok ? "added" : "could not be added");
	}

	bool g_notify_hooked = false;

	// Tries to hook the script "notify" function. Can be called again and again until it works.
	// MinHook needs a free spot within 1 GB of the function it patches. Once the game is running those
	// spots can all be taken, so we reserve a few early and give them back right before hooking.
	void* g_reserved[8];
	int g_reserved_count = 0;

	void reserve_hook_memory()
	{
		for (uintptr_t address = 0x13F000000ull; address > 0x110000000ull && g_reserved_count < 8; address -= 0x400000ull)
		{
			void* block = VirtualAlloc(reinterpret_cast<void*>(address), 0x10000, MEM_RESERVE, PAGE_NOACCESS);
			if (block)
			{
				g_reserved[g_reserved_count++] = block;
			}
		}

		log_line("reserved %d memory blocks near the game program for hooking", g_reserved_count);
	}

	void release_hook_memory()
	{
		for (int i = 0; i < g_reserved_count; ++i)
		{
			VirtualFree(g_reserved[i], 0, MEM_RELEASE);
		}

		g_reserved_count = 0;
	}

	void try_hook_notify(const int attempt)
	{
		release_hook_memory();
		void* notify = reinterpret_cast<void*>(SP_SCR_NOTIFY_ID);
		unsigned char probe[16];
		if (!read_bytes(SP_SCR_NOTIFY_ID, probe, sizeof(probe)))
		{
			log_line("hook: script events attempt %d: function memory not readable yet", attempt);
			return;
		}

		MH_STATUS created = MH_CreateHook(notify, reinterpret_cast<LPVOID>(&hooked_notify),
		                                  reinterpret_cast<LPVOID*>(&g_original_notify));
		if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED)
		{
			log_line("hook: script events attempt %d: create failed (%s); first bytes %02X %02X %02X %02X %02X %02X", attempt,
			         MH_StatusToString(created), probe[0], probe[1], probe[2], probe[3], probe[4], probe[5]);
			return;
		}

		MH_STATUS enabled = MH_EnableHook(notify);
		if (enabled != MH_OK)
		{
			log_line("hook: script events attempt %d: enable failed (%s)", attempt, MH_StatusToString(enabled));
			return;
		}

		g_notify_hooked = true;
		log_line("hook: script events installed (attempt %d)", attempt);
	}

	bool is_campaign_exe()
	{
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exe, MAX_PATH);
		for (char* c = exe; *c; ++c)
		{
			if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c + 32);
		}

		return strstr(exe, "s1_sp64_ship") != nullptr || strstr(exe, "s1_mp64_ship") != nullptr;
	}


	// ---- single-player vs multiplayer -------------------------------------------------------
	bool exe_contains(const char* word)
	{
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exe, MAX_PATH);
		for (char* c = exe; *c; ++c)
		{
			if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c + 32);
		}

		return strstr(exe, word) != nullptr;
	}

	// Addresses come from the S1x source files (second number of each pair is multiplayer).
	void choose_game_layout()
	{
		if (!exe_contains("s1_mp64_ship"))
		{
			return;
		}

		g_is_mp = true;
		SP_G_ENTITIES = 0x144758C70;
		SP_SL_CONVERT_TO_STRING = 0x1403F0F10;
		SP_DVAR_FIND_VAR = 0x1404BF8B0;
		SP_SCR_NOTIFY_ID = 0x1403F92D0;
		SP_DB_ENUM_XASSETS = 0x14026EC10;
		g_stride = 0x2E0;
		g_source = 0;     // the single-player camera addresses do not exist here
	}

	bool mp_class_ok(const unsigned short id)
	{
		char name[48];
		if (id < 2 || !class_name(id, name, sizeof(name)))
		{
			return false;
		}

		static const char* const kStarts[] = {"player", "script_", "trigger_", "actor", "misc_", "info_", "mp_", "scriptable", "agent", "dog", "vehicle", "light", "worldspawn", "node_"};
		for (const char* start : kStarts)
		{
			if (strncmp(name, start, strlen(start)) == 0)
			{
				return true;
			}
		}

		return false;
	}

	// The multiplayer entity layout is not known, so find the field positions by looking at the data.
	// Returns true once everything has been worked out.
	bool g_mp_calibrated = false;

	bool calibrate_mp_layout()
	{
		const int slots = 600;
		static unsigned char ents[600][0x300];
		int readable = 0;
		for (int i = 0; i < slots; ++i)
		{
			if (read_bytes(SP_G_ENTITIES + static_cast<uintptr_t>(i) * g_stride, ents[i], 0x300))
			{
				++readable;
			}
			else
			{
				memset(ents[i], 0, 0x300);
			}
		}

		if (readable < slots / 2)
		{
			return false;
		}

		// 1. which 16-bit field holds a class name
		size_t best_class = 0;
		int best_score = 0;
		for (size_t off = 0x40; off < 0x2F0; off += 2)
		{
			int score = 0;
			for (int i = 0; i < slots; ++i)
			{
				unsigned short id = 0;
				memcpy(&id, ents[i] + off, 2);
				if (id != 0 && mp_class_ok(id))
				{
					++score;
				}
			}

			if (score > best_score)
			{
				best_score = score;
				best_class = off;
			}
		}

		if (best_score < 80)
		{
			log_line("multiplayer layout: no class-name field found yet (best score %d); will retry", best_score);
			return false;
		}

		log_line("multiplayer layout: class name field at +0x%zX (%d entities with a sensible class)", best_class, best_score);
		OFF_CLASSNAME = best_class;

		// 1b. which entity is the player, where the type and name strings are
		g_player_slot = 0;
		for (int i = 0; i < 64; ++i)
		{
			unsigned short id = 0;
			memcpy(&id, ents[i] + best_class, 2);
			char name[48];
			if (id && class_name(id, name, sizeof(name)) && strcmp(name, "player") == 0)
			{
				g_player_slot = i;
				break;
			}
		}

		log_line("multiplayer layout: local player is entity %d", g_player_slot);
		{
			int best_type = 0;
			size_t type_off = OFF_TYPE;
			for (size_t off = best_class > 0x60 ? best_class - 0x60 : 0; off <= best_class + 0x80; off += 4)
			{
				int score = 0;
				for (int i = 0; i < slots; ++i)
				{
					unsigned short id = 0;
					memcpy(&id, ents[i] + best_class, 2);
					unsigned int sid = 0;
					memcpy(&sid, ents[i] + off, 4);
					char a[48], b[48];
					if (id && sid > 1 && sid < 0x10000 && class_name(id, a, sizeof(a)) && strncmp(a, "trigger_", 8) == 0 &&
						class_name(sid, b, sizeof(b)) && strncmp(b, "trigger_", 8) == 0)
					{
						++score;
					}
				}

				if (score > best_type)
				{
					best_type = score;
					type_off = off;
				}
			}

			OFF_TYPE = type_off;
			size_t name_off = OFF_NAME;
			int best_names = 0;
			for (size_t off = best_class > 0x60 ? best_class - 0x60 : 0; off <= best_class + 0x80; off += 4)
			{
				if (off == type_off) continue;
				unsigned int seen[300];
				int distinct = 0;
				for (int i = 0; i < slots; ++i)
				{
					unsigned short id = 0;
					memcpy(&id, ents[i] + best_class, 2);
					unsigned int sid = 0;
					memcpy(&sid, ents[i] + off, 4);
					char a[48], b[64];
					if (!(id && sid > 1 && sid < 0x10000 && class_name(id, a, sizeof(a)) && (strncmp(a, "trigger_", 8) == 0 || strcmp(a, "script_brushmodel") == 0) &&
						class_name(sid, b, sizeof(b)) && strlen(b) >= 3 && strncmp(b, "trigger_", 8) != 0))
					{
						continue;
					}

					bool printable = true;
					for (const char* c = b; *c; ++c) if (static_cast<unsigned char>(*c) < 32 || static_cast<unsigned char>(*c) > 126) printable = false;
					bool dup = false;
					for (int k = 0; k < distinct; ++k) if (seen[k] == sid) dup = true;
					if (printable && !dup && distinct < 300) seen[distinct++] = sid;
				}

				if (distinct > best_names)
				{
					best_names = distinct;
					name_off = off;
				}
			}

			OFF_NAME = name_off;
			log_line("multiplayer layout: type name at +0x%zX (%d triggers), target names at +0x%zX (%d different)", OFF_TYPE, best_type, OFF_NAME, best_names);
		}

		// 2. origin: a float triple that appears twice in the entity (a copy 0xD0 further on) and differs from entity to entity.
		//    The bounds are stored relative to that origin.
		{
			size_t best_origin = 0x68;
			int best_origin_score = 0;
			for (size_t oo = 0x10; oo + 0xD0 + 12 <= 0x2F0; oo += 4)
			{
				int score = 0;
				for (int i = 0; i < slots; ++i)
				{
					unsigned short id = 0;
					memcpy(&id, ents[i] + best_class, 2);
					if (id == 0) continue;
					float o[3], c[3];
					memcpy(o, ents[i] + oo, 12);
					memcpy(c, ents[i] + oo + 0xD0, 12);
					const bool sane = std::fabs(o[0]) < 100000.0f && std::fabs(o[1]) < 100000.0f && std::fabs(o[2]) < 100000.0f;
					if (sane && (std::fabs(o[0]) > 1.0f || std::fabs(o[1]) > 1.0f) && o[0] == c[0] && o[1] == c[1] && o[2] == c[2]) ++score;
				}

				if (score > best_origin_score)
				{
					best_origin_score = score;
					best_origin = oo;
				}
			}

			OFF_ORIGIN = best_origin;
			OFF_BOUNDS = best_origin + 0x9C;
			log_line("multiplayer layout: world position at +0x%zX (%d entities), bounds (relative to it) at +0x%zX", OFF_ORIGIN, best_origin_score, OFF_BOUNDS);
			if (best_origin_score < 5)
			{
				return false;
			}
		}

		// 3. client pointer of the local player (entity 0): near the class field in the single-player layout, +0x158 per the headers
		size_t client_off = 0;
		const size_t candidates[] = {0x158, OFF_CLIENT + (best_class > 0x14C ? best_class - 0x14C : 0), 0x118};
		for (const size_t cand : candidates)
		{
			unsigned long long ptr = 0;
			memcpy(&ptr, ents[g_player_slot] + cand, 8);
			if (ptr > 0x140000000ull && ptr < 0x7FFFFFFE0000ull)
			{
				client_off = cand;
				break;
			}
		}

		if (client_off)
		{
			OFF_CLIENT = client_off;
			log_line("multiplayer layout: client pointer at +0x%zX", client_off);
			unsigned long long ptr = 0;
			memcpy(&ptr, ents[g_player_slot] + client_off, 8);
			float origin0[3];
			memcpy(origin0, ents[g_player_slot] + OFF_ORIGIN, 12);
			// Worked out from the saved player data: in the playerState the position is at +0x78 and the
			// view angles (pitch, yaw) at +0x90 / +0x94.
			float ps_origin[3] = {};
			if (read_bytes(static_cast<uintptr_t>(ptr) + 0x78, ps_origin, sizeof(ps_origin)))
			{
				const bool same = std::fabs(ps_origin[0] - origin0[0]) < 1.0f && std::fabs(ps_origin[1] - origin0[1]) < 1.0f &&
					std::fabs(ps_origin[2] - origin0[2]) < 1.0f;
				OFF_CAM_ORIGIN = 0x78;
				OFF_CAM_PITCH = 0x90;
				OFF_CAM_YAW = 0x94;
				g_source = 0;
				g_pitch_sign = -1;   // multiplayer reports pitch the other way round (confirmed by looking up/down)
				log_line("multiplayer layout: playerState position +0x78, pitch +0x90, yaw +0x94 (position matches entity: %s)", same ? "yes" : "NO");
			}
		}
		else
		{
			log_line("multiplayer layout: no client pointer found in entity 0");
		}

		return true;
	}


	// ---- multiplayer camera search -----------------------------------------------------------
	// Looks through the game's memory for "eye position followed by three direction vectors" where the eye is
	// at the local player's position. Every spot found becomes a camera source (F3 cycles through them).
	int g_mp_cam_found = 0;
	int g_mp_cam_tries = 0;
	uintptr_t g_cand[64];
	int g_cand_count = 0;
	float g_cand_pos[3];
	bool g_cand_final = false;

	bool axes_ok(const float* m);

	void use_candidates()
	{
		for (int i = 1; i < SOURCE_COUNT; ++i)
		{
			kSources[i].address = 0;
		}

		for (int i = 0; i < g_cand_count && i < SOURCE_COUNT - 1; ++i)
		{
			kSources[i + 1].address = g_cand[i];
			kSources[i + 1].kind = 0;
			kSources[i + 1].angle_offset = 12;
		}
	}

	// Some of the places found are old copies. Once the player has walked a bit, keep only the ones that followed.
	void mp_camera_prune()
	{
		if (g_cand_final || g_cand_count == 0)
		{
			return;
		}

		float feet[3];
		if (!read_bytes(player_entity() + OFF_ORIGIN, feet, sizeof(feet)))
		{
			return;
		}

		const float moved = std::fabs(feet[0] - g_cand_pos[0]) + std::fabs(feet[1] - g_cand_pos[1]);
		if (moved < 80.0f)
		{
			return;
		}

		int kept = 0;
		for (int i = 0; i < g_cand_count; ++i)
		{
			float v[12];
			if (read_bytes(g_cand[i], v, sizeof(v)) && std::fabs(v[0] - feet[0]) < 2.0f && std::fabs(v[1] - feet[1]) < 2.0f &&
				v[2] > feet[2] - 1.0f && v[2] < feet[2] + 90.0f && axes_ok(v + 3))
			{
				g_cand[kept++] = g_cand[i];
			}
		}

		log_line("camera search: after you moved, %d of %d places still follow the player", kept, g_cand_count);
		g_cand_count = kept;
		if (kept == 0)
		{
			g_mp_cam_found = 0;     // start over
			return;
		}

		use_candidates();
		for (int i = 0; i < kept && i < 9; ++i)
		{
			log_line("   source %d at 0x%llX", i + 1, static_cast<unsigned long long>(g_cand[i]));
		}

		g_cand_final = true;
		g_source = 1;
	}

	bool axes_ok(const float* m)
	{
		for (int r = 0; r < 3; ++r)
		{
			const float len = m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2];
			if (!(len > 0.97f && len < 1.03f)) return false;
		}

		for (int a = 0; a < 3; ++a)
		{
			for (int b = a + 1; b < 3; ++b)
			{
				const float d = m[a * 3] * m[b * 3] + m[a * 3 + 1] * m[b * 3 + 1] + m[a * 3 + 2] * m[b * 3 + 2];
				if (!(std::fabs(d) < 0.03f)) return false;
			}
		}

		return true;
	}

	void mp_camera_search()
	{
		float feet[3];
		if (!read_bytes(player_entity() + OFF_ORIGIN, feet, sizeof(feet)) || !(std::fabs(feet[0]) < 100000.0f) || (feet[0] == 0.0f && feet[1] == 0.0f))
		{
			return;
		}

		++g_mp_cam_tries;
		SYSTEM_INFO info;
		GetSystemInfo(&info);
		uintptr_t address = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
		const uintptr_t limit = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
		static unsigned char chunk[1 << 20];
		uintptr_t* found = g_cand;
		int found_count = 0;
		while (address < limit && found_count < 64)
		{
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)))
			{
				break;
			}

			const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
			const uintptr_t end = base + mbi.RegionSize;
			const bool readable = mbi.State == MEM_COMMIT && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY) && mbi.RegionSize < (512ull << 20);
			if (readable)
			{
				for (uintptr_t pos = base; pos < end && found_count < 64; pos += sizeof(chunk) - 64)
				{
					const size_t n = (end - pos) < sizeof(chunk) ? static_cast<size_t>(end - pos) : sizeof(chunk);
					if (n < 48 || !read_bytes(pos, chunk, n))
					{
						continue;
					}

					for (size_t o = 0; o + 48 <= n && found_count < 64; o += 4)
					{
						float v[12];
						memcpy(v, chunk + o, 48);
						if (std::fabs(v[0] - feet[0]) < 1.5f && std::fabs(v[1] - feet[1]) < 1.5f && v[2] > feet[2] - 1.0f && v[2] < feet[2] + 90.0f && axes_ok(v + 3))
						{
							found[found_count++] = pos + o;
						}
					}
				}
			}

			address = end;
		}

		log_line("camera search (try %d): %d places hold the player's eye followed by direction vectors - now walk around a bit", g_mp_cam_tries, found_count);
		g_cand_count = found_count;
		memcpy(g_cand_pos, feet, sizeof(feet));
		g_cand_final = false;
		if (found_count > 0)
		{
			g_mp_cam_found = found_count;
			use_candidates();
			g_source = 1;
		}
	}


	// ===========================================================================================
	// Speed run recorder: writes everything useful about a run into the speedrun_data folder.
	// ===========================================================================================
	// The ordinary read_bytes asks Windows about the memory first, which was taking ~19 ms per read while the
	// game is streaming. This one just reads and catches a failure.
	bool read_fast(const uintptr_t address, void* out, const size_t count)
	{
		if (address < 0x10000)
		{
			return false;
		}

		__try
		{
			memcpy(out, reinterpret_cast<const void*>(address), count);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	CRITICAL_SECTION g_rec_lock;
	bool g_rec_lock_ready = false;
	FILE* g_rec = nullptr;
	double g_rec_start = 0.0;
	char g_rec_map[64] = {};
	bool g_rec_flag_cache_ready = false;
	bool g_prev_reset = true;
	volatile bool g_scan_go = false;
	volatile bool g_rs_silent = false;      // after a reload: re-learn the entities without writing hundreds of SPAWN lines
	bool g_rec_gap = false;
	int g_rec_gap_ticks = 0;

	void rec_line(const char* fmt, ...)
	{
		if (!g_rec_lock_ready || !g_rec)
		{
			return;
		}

		char text[3000];
		va_list args;
		va_start(args, fmt);
		vsnprintf(text, sizeof(text), fmt, args);
		va_end(args);

		EnterCriticalSection(&g_rec_lock);
		if (g_rec)
		{
			fprintf(g_rec, "%.2f %s\n", now_seconds() - g_rec_start, text);
		}

		LeaveCriticalSection(&g_rec_lock);
	}

	// every script message, one line each (very quick repeats of the same message are thinned out)
	void rec_event(const char* name, const unsigned int entity)
	{
		if (!g_rec)
		{
			return;
		}

		static double last_time[256];
		unsigned int h = 5381;
		for (const char* c = name; *c; ++c) h = h * 33 + static_cast<unsigned char>(*c);
		const double now = now_seconds();
		if (now - last_time[h & 255] < 0.05)
		{
			return;
		}

		last_time[h & 255] = now;
		rec_line("EVENT %s ent=%u", name, entity);
	}

	void rec_flush()
	{
		if (g_rec_lock_ready && g_rec)
		{
			EnterCriticalSection(&g_rec_lock);
			if (g_rec) fflush(g_rec);
			LeaveCriticalSection(&g_rec_lock);
		}
	}

	void rec_close(const char* why)
	{
		if (!g_rec)
		{
			return;
		}

		g_scan_go = false;
		g_rec_gap = false;
		g_rec_gap_ticks = 0;
		rec_line("END %s", why);
		EnterCriticalSection(&g_rec_lock);
		fclose(g_rec);
		g_rec = nullptr;
		LeaveCriticalSection(&g_rec_lock);
		log_line("recorder: closed the run file (%s)", why);
	}

	// ---- scripts and other game data ------------------------------------------------------
	thread_local void* g_assets_big[6000];
	thread_local int g_assets_big_count = 0;

	void big_asset_callback(void* header, void*)
	{
		if (g_assets_big_count < 6000)
		{
			g_assets_big[g_assets_big_count++] = header;
		}
	}

	unsigned int g_dumped_hash[16384];
	int g_dumped_count = 0;

	bool already_dumped(const char* name)
	{
		unsigned int h = 5381;
		for (const char* c = name; *c; ++c) h = h * 33 + static_cast<unsigned char>(*c);
		if (h == 0) h = 1;
		for (int i = 0; i < g_dumped_count; ++i)
		{
			if (g_dumped_hash[i] == h) return true;
		}

		if (g_dumped_count < 16384) g_dumped_hash[g_dumped_count++] = h;
		return false;
	}

	void sanitize_name(const char* in, char* out, const size_t capacity)
	{
		size_t n = 0;
		for (; *in && n + 1 < capacity; ++in)
		{
			const char c = *in;
			out[n++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-') ? c : '_';
		}

		out[n] = '\0';
	}

	bool enum_assets(const int type)
	{
		g_assets_big_count = 0;
		using enum_t = void (*)(int, void (*)(void*, void*), void*, bool);
		__try
		{
			reinterpret_cast<enum_t>(SP_DB_ENUM_XASSETS)(type, &big_asset_callback, nullptr, false);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}

		return true;
	}

	bool write_bytes_from_game(FILE* f, const uintptr_t address, const long long length)
	{
		static unsigned char chunk[65536];
		for (long long done = 0; done < length; done += sizeof(chunk))
		{
			const size_t n = static_cast<size_t>(length - done < static_cast<long long>(sizeof(chunk)) ? length - done : static_cast<long long>(sizeof(chunk)));
			if (!read_fast(address + static_cast<uintptr_t>(done), chunk, n))
			{
				return false;
			}

			fwrite(chunk, 1, n, f);
		}

		return true;
	}

	volatile LONG g_script_thread_running = 0;

	// Saves every script file the game has loaded (the level logic) plus an index of raw files.
	void dump_scripts()
	{
		char path[MAX_PATH];
		if (!enum_assets(50))   // ASSET_TYPE_SCRIPTFILE
		{
			log_line("scripts: could not list the script files");
			return;
		}

		log_line("scripts: the game has %d script files loaded", g_assets_big_count);
		make_path(path, "scripts_index.txt");
		FILE* index = nullptr;
		fopen_s(&index, path, "ab");
		int written = 0;
		long long total = 0;
		void* headers[6000];
		const int count = g_assets_big_count;
		memcpy(headers, g_assets_big, sizeof(void*) * static_cast<size_t>(count));
		for (int i = 0; i < count; ++i)
		{
			const uintptr_t base = reinterpret_cast<uintptr_t>(headers[i]);
			unsigned long long name_ptr = 0, buffer_ptr = 0, bytecode_ptr = 0;
			int compressed = 0, length = 0, bytecode_len = 0;
			read_fast(base, &name_ptr, 8);
			read_fast(base + 8, &compressed, 4);
			read_fast(base + 12, &length, 4);
			read_fast(base + 16, &bytecode_len, 4);
			read_fast(base + 24, &buffer_ptr, 8);
			read_fast(base + 32, &bytecode_ptr, 8);
			char name[160] = {};
			if (!name_ptr || !safe_copy_string(reinterpret_cast<const char*>(name_ptr), name, sizeof(name)))
			{
				continue;
			}

			if (already_dumped(name))
			{
				continue;
			}

			if (compressed < 0 || compressed > 16 * 1024 * 1024 || bytecode_len < 0 || bytecode_len > 16 * 1024 * 1024)
			{
				if (index) fprintf(index, "%s  (odd sizes %d %d %d, skipped)\n", name, compressed, length, bytecode_len);
				continue;
			}

			char clean[200];
			sanitize_name(name, clean, sizeof(clean));
			char file[260];
			snprintf(file, sizeof(file), "scripts\\%s.gscbin", clean);
			make_path(path, file);
			FILE* f = nullptr;
			if (fopen_s(&f, path, "wb") != 0 || !f)
			{
				continue;
			}

			// header as found in the game: three sizes, then the packed script, then the bytecode
			fwrite(&compressed, 4, 1, f);
			fwrite(&length, 4, 1, f);
			fwrite(&bytecode_len, 4, 1, f);
			bool ok = write_bytes_from_game(f, static_cast<uintptr_t>(buffer_ptr), compressed);
			ok = ok && write_bytes_from_game(f, static_cast<uintptr_t>(bytecode_ptr), bytecode_len);
			fclose(f);
			if (index)
			{
				fprintf(index, "%s  file=%s packed=%d size=%d bytecode=%d %s\n", name, file, compressed, length, bytecode_len, ok ? "ok" : "PARTIAL");
			}

			++written;
			total += compressed + bytecode_len;
			Sleep(15);   // go easy on the disk and the virus scanner so the game never stutters
		}

		if (index) fclose(index);
		log_line("scripts: saved %d new script files (%lld bytes)", written, total);

		// raw files: names and sizes, and the small text-like ones in full
		if (enum_assets(49))   // ASSET_TYPE_RAWFILE
		{
			const int raw_count = g_assets_big_count;
			make_path(path, "rawfiles_index.txt");
			FILE* ri = nullptr;
			fopen_s(&ri, path, "ab");
			int saved = 0;
			for (int i = 0; i < raw_count; ++i)
			{
				const uintptr_t base = reinterpret_cast<uintptr_t>(g_assets_big[i]);
				unsigned long long name_ptr = 0, buffer_ptr = 0;
				int compressed = 0, length = 0;
				read_fast(base, &name_ptr, 8);
				read_fast(base + 8, &compressed, 4);
				read_fast(base + 12, &length, 4);
				read_fast(base + 16, &buffer_ptr, 8);
				char name[160] = {};
				if (!name_ptr || !safe_copy_string(reinterpret_cast<const char*>(name_ptr), name, sizeof(name)) || already_dumped(name))
				{
					continue;
				}

				if (ri) fprintf(ri, "%s packed=%d size=%d\n", name, compressed, length);
				const size_t nl = strlen(name);
				const bool texty = nl > 4 && (strcmp(name + nl - 4, ".gsc") == 0 || strcmp(name + nl - 4, ".csv") == 0 || strcmp(name + nl - 4, ".txt") == 0 ||
					strcmp(name + nl - 4, ".cfg") == 0 || strcmp(name + nl - 4, ".gsx") == 0);
				if (texty && compressed > 0 && compressed < 2 * 1024 * 1024 && saved < 400)
				{
					Sleep(15);
					char clean[200];
					sanitize_name(name, clean, sizeof(clean));
					char file[260];
					snprintf(file, sizeof(file), "rawfiles\\%s.bin", clean);
					make_path(path, file);
					FILE* f = nullptr;
					if (fopen_s(&f, path, "wb") == 0 && f)
					{
						write_bytes_from_game(f, static_cast<uintptr_t>(buffer_ptr), compressed);
						fclose(f);
						++saved;
					}
				}
			}

			if (ri) fclose(ri);
			log_line("rawfiles: %d listed, %d saved", raw_count, saved);
		}
	}

	DWORD WINAPI script_thread(LPVOID)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
		g_subdir = "speedrun_data\\";
		Sleep(20000);   // let the mission settle first
		dump_scripts();
		g_subdir = nullptr;
		InterlockedExchange(&g_script_thread_running, 0);
		return 0;
	}

	// ---- the live recording ------------------------------------------------------------------
	struct rec_slot
	{
		unsigned short class_id;
		unsigned int name_id;
		bool inside;
		unsigned int state;
		double state_time;
		float pos[3];
		float tz;          // trigger height last seen (a script switching a trigger on/off moves it 10000 units)
		bool tz_ok;
	};

	rec_slot g_rs[MAX_ENTITIES];
	char g_rec_class_cache[65536];   // 0 unknown, 1 other, 2 actor, 3 vehicle
	int g_rec_last_actors = -1, g_rec_last_vehicles = -1;
	double g_rec_gone_burst_time = 0.0;

	bool rec_player_alive()
	{
		unsigned short id = 0;
		if (!read_fast(player_entity() + OFF_CLASSNAME, &id, sizeof(id)) || id == 0)
		{
			return false;
		}

		char name[32];
		return class_name(id, name, sizeof(name)) && strcmp(name, "player") == 0;
	}

	unsigned int g_rec_f54 = 0, g_rec_f58 = 0, g_rec_gt = 0;
	float g_rec_vh = 60.0f;

	bool rec_player_state(float* origin, float* pitch, float* yaw)
	{
		unsigned long long client = 0;
		if (!read_fast(player_entity() + OFF_CLIENT, &client, sizeof(client)) || client < 0x140000000ull || client > 0x7FFFFFFE0000ull)
		{
			return false;
		}

		// stance data (campaign): 0x54 movement flags, 0x58 stance (0 stand, 4 crouch, 8 prone/slide), 0x120 view height
		read_fast(static_cast<uintptr_t>(client) + 0x54, &g_rec_f54, 4);
		read_fast(static_cast<uintptr_t>(client) + 0x58, &g_rec_f58, 4);
		read_fast(static_cast<uintptr_t>(client) + 0x120, &g_rec_vh, 4);
		read_fast(static_cast<uintptr_t>(client) + 0x4C, &g_rec_gt, 4);   // the game clock in milliseconds
		return read_fast(static_cast<uintptr_t>(client) + OFF_CAM_ORIGIN, origin, 12) &&
			read_fast(static_cast<uintptr_t>(client) + OFF_CAM_PITCH, pitch, 4) &&
			read_fast(static_cast<uintptr_t>(client) + OFF_CAM_YAW, yaw, 4);
	}

	void rec_describe(const int slot, const unsigned char* buffer, char* out, const size_t capacity)
	{
		unsigned short class_id = 0;
		memcpy(&class_id, buffer + OFF_CLASSNAME, 2);
		unsigned int type_id = 0, name_id = 0;
		memcpy(&type_id, buffer + OFF_TYPE, 4);
		memcpy(&name_id, buffer + OFF_NAME, 4);
		char cls[64] = {}, type[64] = {}, name[64] = {};
		class_name(class_id, cls, sizeof(cls));
		if (type_id > 1 && type_id < 0x10000) class_name(type_id, type, sizeof(type));
		if (name_id > 1 && name_id < 0x10000) class_name(name_id, name, sizeof(name));
		char flags[100] = {};
		learned_flags(slot, flags, sizeof(flags));
		float b[6];
		memcpy(b, buffer + OFF_BOUNDS, sizeof(b));
		make_world_bounds(b, buffer);
		snprintf(out, capacity, "#%d %s/%s name=%s%s%s at %.0f %.0f %.0f size %.0f %.0f %.0f", slot, cls[0] ? cls : "?", type[0] ? type : "-",
			name[0] ? name : "-", flags[0] ? " flags=" : "", flags, b[0], b[1], b[2], b[3] * 2, b[4] * 2, b[5] * 2);
	}

	bool rec_current_map(char* map, const size_t capacity)
	{
		if (enum_assets(29) && g_assets_big_count > 0)
		{
			unsigned long long name_ptr = 0;
			char full[128] = {};
			if (read_fast(reinterpret_cast<uintptr_t>(g_assets_big[0]), &name_ptr, 8) && name_ptr &&
				safe_copy_string(reinterpret_cast<const char*>(name_ptr), full, sizeof(full)))
			{
				const char* slash = strrchr(full, '/');
				sanitize_name(slash ? slash + 1 : full, map, capacity);
				char* dot = strchr(map, '.');
				if (dot) *dot = '\0';
				return true;
			}
		}

		return false;
	}

	void rec_open()
	{
		g_scan_go = false;
		// map name from the map entities asset
		char map[64] = "unknown";
		if (enum_assets(29) && g_assets_big_count > 0)
		{
			unsigned long long name_ptr = 0;
			char full[128] = {};
			if (read_fast(reinterpret_cast<uintptr_t>(g_assets_big[0]), &name_ptr, 8) && name_ptr &&
				safe_copy_string(reinterpret_cast<const char*>(name_ptr), full, sizeof(full)))
			{
				const char* slash = strrchr(full, '/');
				const char* base = slash ? slash + 1 : full;
				sanitize_name(base, map, sizeof(map));
				char* dot = strchr(map, '.');
				if (dot) *dot = '\0';
			}
		}

		snprintf(g_rec_map, sizeof(g_rec_map), "%s", map);
		SYSTEMTIME t;
		GetLocalTime(&t);
		char file[200];
		snprintf(file, sizeof(file), "%s_%04d%02d%02d_%02d%02d%02d.txt", map, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		char path[MAX_PATH];
		make_path(path, file);
		FILE* f = nullptr;
		if (fopen_s(&f, path, "wb") != 0 || !f)
		{
			log_line("recorder: could not create %s", path);
			return;
		}

		g_rec_start = now_seconds();
		memset(g_rs, 0, sizeof(g_rs));
		g_rec_last_actors = g_rec_last_vehicles = -1;
		g_prev_reset = true;
		EnterCriticalSection(&g_rec_lock);
		g_rec = f;
		LeaveCriticalSection(&g_rec_lock);
		rec_line("RUN FILE  tool version %s  map %s  mode %s", ADDON_VERSION, map, g_is_mp ? "multiplayer" : "campaign");
		rec_line("COLUMNS  P x y z pitch yaw | ENTER/EXIT #slot class/type name flags | SPAWN/REMOVED/STATE | EVENT name | ACTOR+/ACTOR- #slot type name at x y z | ACTORS n | TRIG (all triggers at start)");
		log_line("recorder: started %s", file);

		// the map's entity text (flag names) and the game's scripts
		dump_map_ents();
		rebuild_slot_flags();
		if (InterlockedCompareExchange(&g_script_thread_running, 1, 0) == 0)
		{
			HANDLE t = CreateThread(nullptr, 0, script_thread, nullptr, 0, nullptr);
			if (t) CloseHandle(t); else InterlockedExchange(&g_script_thread_running, 0);
		}

		g_scan_go = false;
		// every trigger and solid-brush entity as it is at the start
		for (int slot = 0; slot < MAX_ENTITIES; ++slot)
		{
			unsigned char buffer[0x1C0];
			if (!read_fast(SP_G_ENTITIES + static_cast<uintptr_t>(slot) * g_stride, buffer, sizeof(buffer)))
			{
				break;
			}

			unsigned short class_id = 0;
			memcpy(&class_id, buffer + OFF_CLASSNAME, 2);
			if (class_id == 0 || classify(class_id) < 2)
			{
				continue;
			}

			char text[300];
			rec_describe(slot, buffer, text, sizeof(text));
			rec_line("TRIG %s", text);
		}

		// how long is the entity table?
		{
			int last_used = 0, used = 0, triggers = 0;
			for (int slot = 0; slot < MAX_ENTITIES; ++slot)
			{
				unsigned char buffer[0x1C0];
				if (!read_fast(SP_G_ENTITIES + static_cast<uintptr_t>(slot) * g_stride, buffer, sizeof(buffer)))
				{
					rec_line("ENTITY TABLE: reading stopped at slot %d", slot);
					break;
				}

				unsigned short class_id = 0;
				memcpy(&class_id, buffer + OFF_CLASSNAME, 2);
				if (class_id != 0)
				{
					++used;
					last_used = slot;
					if (classify(class_id) >= 2) ++triggers;
				}
			}

			rec_line("ENTITY TABLE: %d slots in use, last used slot %d, %d triggers", used, last_used, triggers);
		}

		g_scan_go = true;
	}

	double g_t_read = 0.0, g_t_name = 0.0, g_t_line = 0.0, g_t_all = 0.0;
	int g_n_read = 0, g_n_name = 0, g_n_scans = 0;
	double g_perf_scan_max = 0.0, g_perf_tick_max = 0.0, g_perf_last_tick = 0.0;
	double g_perf_report = 0.0;

	void rec_scan()
	{
		float origin[3] = {}, pitch = 0, yaw = 0;
		const double scan_start = now_seconds();
		const bool have_player = rec_player_state(origin, &pitch, &yaw);
		int gone = 0, spawned = 0, actors = 0, vehicles = 0;
		for (int slot = 0; slot < MAX_ENTITIES; ++slot)
		{
			unsigned char buffer[0x1C0];
			const double r0 = now_seconds();
			const bool got = read_fast(SP_G_ENTITIES + static_cast<uintptr_t>(slot) * g_stride, buffer, sizeof(buffer));
			g_t_read += now_seconds() - r0;
			++g_n_read;
			if (!got)
			{
				break;
			}

			unsigned short class_id = 0;
			memcpy(&class_id, buffer + OFF_CLASSNAME, 2);
			rec_slot& r = g_rs[slot];
			if (class_id != 0)
			{
				if (g_rec_class_cache[class_id] == 0)
				{
					char name[64] = {};
					const double n0 = now_seconds();
					class_name(class_id, name, sizeof(name));
					g_t_name += now_seconds() - n0;
					++g_n_name;
					g_rec_class_cache[class_id] = strncmp(name, "actor", 5) == 0 ? 2 : (strncmp(name, "script_vehicle", 14) == 0 ? 3 : 1);
				}

				if (g_rec_class_cache[class_id] == 2) ++actors;
				if (g_rec_class_cache[class_id] == 3) ++vehicles;
			}

			// enemies / allies appearing and disappearing, with name and position (so spawn groups can be matched to triggers)
			if (class_id != 0 && g_rec_class_cache[class_id] == 2)
			{
				memcpy(r.pos, buffer + OFF_ORIGIN, sizeof(r.pos));

				// the squad (Will, Cormack, Jackson ...): where they are twice a second
				{
					static unsigned char hero_cache[65536];   // 0 unknown, 1 no, 2 hero
					static double hero_time[MAX_ENTITIES];
					unsigned int type_id = 0;
					memcpy(&type_id, buffer + OFF_TYPE, 4);
					if (type_id > 1 && type_id < 0x1000000)
					{
						if (type_id >= 0x10000)
						{
							char big[64] = {};
							class_name(type_id, big, sizeof(big));
							static double big_time[MAX_ENTITIES];
							const double bnow = now_seconds();
							if (strstr(big, "hero") && bnow - big_time[slot] >= 0.5)
							{
								big_time[slot] = bnow;
								rec_line("HERO #%d %s at %.0f %.0f %.0f", slot, big, r.pos[0], r.pos[1], r.pos[2]);
							}
						}
					}
					if (type_id > 1 && type_id < 0x10000)
					{
						if (hero_cache[type_id] == 0)
						{
							char tname[64] = {};
							class_name(type_id, tname, sizeof(tname));
							hero_cache[type_id] = strstr(tname, "hero") ? 2 : 1;
						}

						const double hnow = now_seconds();
						if (hero_cache[type_id] == 2 && hnow - hero_time[slot] >= 0.5)
						{
							hero_time[slot] = hnow;
							char tname[64] = {};
							class_name(type_id, tname, sizeof(tname));
							rec_line("HERO #%d %s at %.0f %.0f %.0f", slot, tname, r.pos[0], r.pos[1], r.pos[2]);
						}
					}
				}

				if (r.class_id == 0 && !g_rs_silent)
				{
					unsigned int type_id = 0, nid = 0;
					memcpy(&type_id, buffer + OFF_TYPE, 4);
					memcpy(&nid, buffer + OFF_NAME, 4);
					char type[64] = {}, nm[64] = {};
					if (type_id > 1 && type_id < 0x10000) class_name(type_id, type, sizeof(type));
					if (nid > 1 && nid < 0x10000) class_name(nid, nm, sizeof(nm));
					rec_line("ACTOR+ #%d type=%s name=%s at %.0f %.0f %.0f", slot, type[0] ? type : "-", nm[0] ? nm : "-", r.pos[0], r.pos[1], r.pos[2]);
				}
			}
			else if (class_id == 0 && r.class_id != 0 && g_rec_class_cache[r.class_id] == 2 && !g_rs_silent)
			{
				rec_line("ACTOR- #%d at %.0f %.0f %.0f", slot, r.pos[0], r.pos[1], r.pos[2]);
			}

			const unsigned char kind = class_id ? classify(class_id) : (r.class_id ? classify(r.class_id) : 1);
			if (kind >= 2)
			{
				unsigned int name_id = 0, state = 0;
				memcpy(&name_id, buffer + OFF_NAME, 4);
				memcpy(&state, buffer + OFF_BOUNDS - 4, 4);
				if (class_id != 0 && r.class_id == 0)
				{
					if (!g_rs_silent)
					{
						char text[300];
						rec_describe(slot, buffer, text, sizeof(text));
						rec_line("SPAWN %s", text);
					}

					++spawned;
				}
				else if (class_id == 0 && r.class_id != 0)
				{
					char name[64] = {}, cls[64] = {};
					if (r.name_id > 1 && r.name_id < 0x10000) class_name(r.name_id, name, sizeof(name));
					class_name(r.class_id, cls, sizeof(cls));
					if (!g_rs_silent) rec_line("REMOVED #%d %s name=%s", slot, cls, name[0] ? name : "-");
					++gone;
					r.inside = false;
				}
				else if (class_id != 0 && state != r.state && now_seconds() - r.state_time > 0.5)
				{
					char name[64] = {}, cls[64] = {};
					if (name_id > 1 && name_id < 0x10000) class_name(name_id, name, sizeof(name));
					class_name(class_id, cls, sizeof(cls));
					rec_line("STATE #%d %s name=%s contents %08X -> %08X", slot, cls, name[0] ? name : "-", r.state, state);
					r.state_time = now_seconds();
				}

				r.state = state;
				r.name_id = name_id;

				// a script switching a trigger on or off (the finale prompt, the bomb pickup, colour triggers ...)
				if (class_id != 0)
				{
					float oz = 0.0f;
					memcpy(&oz, buffer + OFF_ORIGIN + 8, 4);
					if (r.tz_ok && std::fabs(oz - r.tz) > 3000.0f && !g_rs_silent)
					{
						char text[300];
						rec_describe(slot, buffer, text, sizeof(text));
						rec_line("SWITCH %s %s (height %.0f -> %.0f)", oz > r.tz ? "OFF" : "ON", text, r.tz, oz);
					}

					r.tz = oz;
					r.tz_ok = true;
				}
				else
				{
					r.tz_ok = false;
				}

				if (class_id != 0 && have_player && kind != 7)
				{
					float b[6];
					memcpy(b, buffer + OFF_BOUNDS, sizeof(b));
					make_world_bounds(b, buffer);
					const bool sane = std::fabs(b[0]) < 200000.0f && std::fabs(b[1]) < 200000.0f && std::fabs(b[2]) < 200000.0f && b[3] > 0.0f && b[4] > 0.0f && b[5] > 0.0f;
					const bool inside = sane && std::fabs(origin[0] - b[0]) <= b[3] && std::fabs(origin[1] - b[1]) <= b[4] &&
						origin[2] + 30.0f >= b[2] - b[5] && origin[2] + 30.0f <= b[2] + b[5];
					if (inside != r.inside)
					{
						char text[300];
						rec_describe(slot, buffer, text, sizeof(text));
						rec_line("%s %s", inside ? "ENTER" : "EXIT", text);
						r.inside = inside;
					}
				}
			}
			else if (class_id == 0 && r.class_id != 0)
			{
				++gone;
			}

			r.class_id = class_id;
		}

		if (actors != g_rec_last_actors || vehicles != g_rec_last_vehicles)
		{
			rec_line("ACTORS %d VEHICLES %d", actors, vehicles);
			g_rec_last_actors = actors;
			g_rec_last_vehicles = vehicles;
		}

		g_t_all += now_seconds() - scan_start;
		++g_n_scans;
		g_rs_silent = false;
		if (gone >= 20)
		{
			// the level is reloading or ending: keep the file open, the next mission / restart is handled in recorder_tick
			rec_line("VANISHED %d entities (level reloading or ending)", gone);
			g_rs_silent = true;
		}
	}

	// The entity scan runs on its own thread so a slow scan can never delay the position recording.
	DWORD WINAPI scan_thread(LPVOID)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		for (;;)
		{
			Sleep(200);
			if (g_rec && g_scan_go)
			{
				const double t0 = now_seconds();
				rec_scan();
				const double took = now_seconds() - t0;
				if (took > g_perf_scan_max) g_perf_scan_max = took;
			}
		}

		return 0;
	}

	// Stance discovery: write every changed 4-byte value of the player's data blocks (R lines) so the
	// jump / crouch / slide / hover fields can be found afterwards by comparing with the MARK lines (F10).
	unsigned int g_prev_client[256];
	unsigned int g_prev_entity[112];

	void rec_raw_changes()
	{
		unsigned long long client = 0;
		if (!read_fast(player_entity() + OFF_CLIENT, &client, sizeof(client)) || client < 0x140000000ull || client > 0x7FFFFFFE0000ull)
		{
			return;
		}

		unsigned int now_c[256];
		unsigned int now_e[112];
		if (!read_fast(static_cast<uintptr_t>(client), now_c, sizeof(now_c)) || !read_fast(player_entity(), now_e, sizeof(now_e)))
		{
			return;
		}

		if (g_prev_reset)
		{
			memset(g_prev_client, 0, sizeof(g_prev_client));
			memset(g_prev_entity, 0, sizeof(g_prev_entity));
			g_prev_reset = false;
		}

		for (int block = 0; block < 2; ++block)
		{
			const unsigned int* now = block == 0 ? now_c : now_e;
			unsigned int* prev = block == 0 ? g_prev_client : g_prev_entity;
			const int n = block == 0 ? 256 : 112;
			char text[2800];
			int used = snprintf(text, sizeof(text), "R%c", block == 0 ? 'c' : 'e');
			int changed = 0;
			for (int i = 0; i < n && used < 2700; ++i)
			{
				if (now[i] != prev[i])
				{
					used += snprintf(text + used, sizeof(text) - static_cast<size_t>(used), " %X:%X", i * 4, now[i]);
					prev[i] = now[i];
					++changed;
				}
			}

			if (changed)
			{
				rec_line("%s", text);
			}
		}
	}

	// Runs from the worker loop every 100 ms.
	void recorder_tick(const int tick)
	{
		static bool was_mark = false;
		static int alive_ticks = 0;
		static int dead_ticks = 0;
		if (!g_rec_lock_ready)
		{
			return;
		}

		const bool alive = tick % 5 == 0 ? rec_player_alive() : (alive_ticks > 0);
		if (tick % 5 == 0)
		{
			alive_ticks = alive ? alive_ticks + 1 : 0;
			dead_ticks = alive ? 0 : dead_ticks + 1;
		}

		if (!g_rec)
		{
			if (alive_ticks >= 4 && tick % 5 == 0)     // about two seconds in the mission
			{
				rec_open();
			}

			return;
		}

		if (dead_ticks >= 6 && !g_rec_gap)
		{
			g_rec_gap = true;
			g_rec_gap_ticks = 0;
			g_scan_go = false;
			rec_line("GAP the player is gone (loading screen, restart or menu)");
		}

		if (g_rec_gap)
		{
			++g_rec_gap_ticks;
			if (alive_ticks >= 4 && tick % 5 == 0)
			{
				char now_map[64];
				if (!rec_current_map(now_map, sizeof(now_map)) || strcmp(now_map, g_rec_map) == 0)
				{
					g_rec_gap = false;
					g_rec_gap_ticks = 0;
					g_rs_silent = true;
					g_prev_reset = true;
					memset(g_rs, 0, sizeof(g_rs));
					rec_line("RELOAD same mission again");
					g_scan_go = true;
				}
				else
				{
					rec_close("a different mission started");
					rec_open();
				}
			}
			else if (g_rec_gap_ticks > 900)
			{
				rec_close("left the mission (90 seconds without a player)");
			}

			return;
		}

		{
			static int mark_number = 0;
			const bool down = (GetAsyncKeyState(VK_MULTIPLY) & 0x8000) != 0;
			if (down && !was_mark)
			{
				rec_line("MARK %d", ++mark_number);
			}

			was_mark = down;
		}

		rec_raw_changes();

		{
			float origin[3], pitch, yaw;
			// while a level reloads the player data reads as all zero for a moment: ignore that sample
			if (rec_player_state(origin, &pitch, &yaw) && !(origin[0] == 0.0f && origin[1] == 0.0f && origin[2] == 0.0f))
			{
				{
					static unsigned int last_gt = 0;
					if (last_gt > 1000 && g_rec_gt + 500 < last_gt)
					{
						rec_line("RELOAD the game clock went back from %u to %u (checkpoint restart)", last_gt, g_rec_gt);
						g_rs_silent = true;
						g_prev_reset = true;
					}

					last_gt = g_rec_gt;
				}

				rec_line("P %.1f %.1f %.1f %.1f %.1f %X %u %.1f %u", origin[0], origin[1], origin[2], pitch, yaw, g_rec_f54, g_rec_f58, g_rec_vh, g_rec_gt);
			}
		}

		{
			static bool scan_started = false;
			if (!scan_started)
			{
				scan_started = true;
				HANDLE t = CreateThread(nullptr, 0, scan_thread, nullptr, 0, nullptr);
				if (t) CloseHandle(t);
			}
		}

		{
			const double n = now_seconds();
			if (g_perf_last_tick > 0.0 && n - g_perf_last_tick > g_perf_tick_max) g_perf_tick_max = n - g_perf_last_tick;
			g_perf_last_tick = n;
			if (n - g_perf_report > 10.0)
			{
				rec_line("PERF longest gap between recorder ticks %.0f ms, longest entity scan %.0f ms | %d scans: total %.0f ms, reading %.0f ms (%d reads), names %.0f ms (%d lookups)",
					g_perf_tick_max * 1000.0, g_perf_scan_max * 1000.0, g_n_scans, g_t_all * 1000.0, g_t_read * 1000.0, g_n_read, g_t_name * 1000.0, g_n_name);
				g_t_read = g_t_name = g_t_all = 0.0; g_n_read = g_n_name = g_n_scans = 0;
				g_perf_report = n;
				g_perf_tick_max = g_perf_scan_max = 0.0;
			}
		}

		if (tick % 10 == 0)
		{
			rec_flush();
		}
	}

	DWORD WINAPI worker(LPVOID)
	{
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exe, MAX_PATH);

		log_line("################################################################");
		log_line("########   VERSION NUMBER:  %s   (S1x debug overlay)   ########", ADDON_VERSION);
		log_line("################################################################");
		log_line("---- add-on loaded (version %s, speed run recorder) ----", ADDON_VERSION);
		log_line("host program: %s", exe);
		log_line("F8 on/off | F2 labels | F11 events | Home brush models | [ show filter | ] leave-area boxes | F7 distance | F6 thickness | F5 fov style | F4 event filter on/off | Insert geometry search | F3 camera source");

		bool hook_done = false;
		InitializeCriticalSection(&g_rec_lock);
		g_rec_lock_ready = true;
		{
			char dir[MAX_PATH];
			strcpy_s(dir, MAX_PATH, g_dir);
			strcat_s(dir, MAX_PATH, "speedrun_data");
			CreateDirectoryA(dir, nullptr);
			strcat_s(dir, MAX_PATH, "\\scripts");
			CreateDirectoryA(dir, nullptr);
			strcpy_s(dir, MAX_PATH, g_dir);
			strcat_s(dir, MAX_PATH, "speedrun_data\\rawfiles");
			CreateDirectoryA(dir, nullptr);
		}

		choose_game_layout();
		log_line("game mode: %s", g_is_mp ? "multiplayer (layout is being worked out from the data)" : "campaign");
		reserve_hook_memory();
		int notify_tries = 0;

		int tables = 0;
		int seconds_with_data = 0;
		bool auto_done = false;
		bool f9_was_down = false;
		bool insert_was_down = false;
		bool pageup_was_down = false;
		bool command_tried = false;

		// 100 ms per tick; stop after about 30 minutes.
		#ifdef RECORDER_ONLY
		for (int tick = 1; tick < 2000000000; ++tick)
#else
		for (int tick = 1; tick <= 18000; ++tick)
#endif
		{
			Sleep(100);

			// Wait until the game has had time to start up before touching it.
			if (!hook_done && tick >= 150)
			{
				hook_done = true;
#ifdef RECORDER_ONLY
				// the recorder build never draws anything on screen
				if (!is_campaign_exe())
				{
					log_line("not a game program");
				}
#else
				if (is_campaign_exe())
				{
					install_hook();
				}
				else
				{
					log_line("not a game program, drawing not installed");
				}
#endif
			}

			if (hook_done && !g_notify_hooked && notify_tries < 24 && tick % 50 == 0)
			{
				++notify_tries;
				try_hook_notify(notify_tries);
			}

			if (tick % 10 == 0)
			{
				seconds_with_data = entity_list_has_data() ? seconds_with_data + 1 : 0;
			}

			if (hook_done && !g_is_mp)
			{
				g_subdir = "speedrun_data\\";
				recorder_tick(tick);
				g_subdir = nullptr;
			}

			if (g_is_mp && g_mp_calibrated && tick % 50 == 0)
			{
				// a new map invalidates what was learned; notice when the class names stop making sense
				int sensible = 0;
				for (int i = 0; i < 600; ++i)
				{
					unsigned short id = 0;
					if (read_bytes(SP_G_ENTITIES + static_cast<uintptr_t>(i) * g_stride + OFF_CLASSNAME, &id, sizeof(id)) && id && mp_class_ok(id))
					{
						++sensible;
					}
				}

				if (sensible < 40)
				{
					log_line("multiplayer layout: only %d sensible entities, recalibrating", sensible);
					g_mp_calibrated = false;
					g_mp_cam_found = 0;
					g_mp_cam_tries = 0;
				}
			}

			if (g_is_mp && !g_mp_calibrated && seconds_with_data >= 3 && tick % 20 == 0)
			{
				g_mp_calibrated = calibrate_mp_layout();
				if (g_mp_calibrated && OFF_CAM_ORIGIN == 0x78)
				{
					g_mp_cam_tries = 99;   // the camera is known, no memory search needed
				}
			}

			if (g_is_mp && g_mp_calibrated && g_overlay_on && g_mp_cam_found > 0 && !g_cand_final && tick % 5 == 0)
			{
				mp_camera_prune();
			}

			if (g_is_mp && g_mp_calibrated && g_overlay_on && g_mp_cam_found == 0 && g_mp_cam_tries < 12 && tick % 30 == 0)
			{
				mp_camera_search();
			}

			if (!command_tried && seconds_with_data >= 5)
			{
				command_tried = true;
				try_add_console_command();
			}

			if (g_overlay_on && tick % 3 == 0)
			{
				entity_watch();
			}

			if (g_rec_count > 0 && tick % 100 == 0)
			{
				rebuild_slot_flags();
			}

			if (g_collision_on && (g_collision_refresh || tick % 150 == 0))
			{
				g_collision_refresh = false;
				refresh_collision();
			}

			const bool pageup_down = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
			if (pageup_down && !pageup_was_down)
			{
				log_line("PageUp pressed (collision data dump)");
				dump_clipmap();
			}

			pageup_was_down = pageup_down;

			const bool insert_down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
			if (insert_down && !insert_was_down)
			{
				log_line("Insert pressed (geometry search)");
				geometry_search();
			}

			insert_was_down = insert_down;

			const bool f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
			const bool f9_pressed = f9_down && !f9_was_down;
			f9_was_down = f9_down;

			#ifdef RECORDER_ONLY
			const bool auto_table = false;   // the recorder writes its own files
#else
			const bool auto_table = !auto_done && seconds_with_data >= 10;
#endif
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
				write_event_summary();
				write_trigger_names();
				dump_map_ents();
				write_client(tables);
				if (g_is_mp)
				{
					write_mp_player_debug(tables);
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

	#ifdef RECORDER_ONLY
	make_path(g_log_path, "speedrun_recorder.log");
#else
	make_path(g_log_path, "s1x_debug_overlay.log");
#endif

	// Do the real work on a separate thread so loading the game is never held up.
	HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
	if (thread)
	{
		CloseHandle(thread);
	}
}

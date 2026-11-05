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
#include <cctype>

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
	#define ADDON_VERSION "1.26 RECORDER"
	#else
	#define ADDON_VERSION "1.30"
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
	constexpr int MAX_VERTS = 150000;

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

	volatile bool g_route_on = true;      // v0.92: \ key toggles the Seoul route guide lines
	volatile int g_speedo_mode = 2;       // v0.94: ; key cycles speedometer + strafe helper (2) / speed only (1) / off (0)
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
	volatile double g_v_time = 0.0;   // v0.81: when the player passed begin_fob_combat_vignette
	volatile double g_l_time = 0.0;   // v0.83: when Cormack landed (i_have_landed) after that
	volatile double g_in_time = 0.0;  // v0.83: when the player entered player_leaving_map after that
	volatile double g_s_time = 0.0;   // v0.86: canal_strategy_scene_complete (Will is sent to the platform)
	volatile double g_f2_time = 0.0;
	volatile double g_wave_time = 0.0, g_press_time = 0.0, g_prompt_time = 0.0;
	volatile double g_will_van_time = 0.0, g_will_cc_time = 0.0, g_will_camp_time = 0.0;   // v1.03
	volatile double g_will_left_van_time = 0.0;   // v1.08
	volatile double g_will_ledge_time = 0.0;      // v1.12: Will reached his ledge spot (from his position)
	volatile double g_blocker_time = 0.0;         // v1.11: a soldier standing on/next to Will's doorway goal
	volatile double g_enter2_time = 0.0;          // v1.10: Will's opening jump (second ledge enter)   // v1.02: finale plant timing
	volatile float g_wg_goal[3] = {}, g_wg_pos[3] = {};   // v0.98: Will's walking destination and position (set by the recorder)
	volatile double g_wg_time = 0.0, g_wg_still_since = 0.0;
	volatile float g_wc_pos[3] = {};   // v1.00: Cormack position (set by the recorder)
	volatile double g_wc_time = 0.0;
	volatile double g_bbt_time = 0.0;   // v0.91: begin_building_traversal (start of the level, also after a restart)
	volatile double g_enter1_time = 0.0; // v0.91: first first_landassist_enter (Cormack at the ledge)
	volatile int g_enter_count = 0;      // v0.91: how many ledge "enter" animations finished
	volatile double g_will_msg_time = 0.0; volatile int g_will_msg = 0; // 1 jumped, 2 missed, 3 snapped mid-level, 4 snapped at ending
	volatile double g_box_time = 0.0; // v0.88: first entry into player_leaving_map after V
	volatile double g_corm_dist = -1.0; // v0.88: live Cormack distance to his door spot (struct_building_exit_cormack_wait)
	volatile double g_corm_speed = 0.0; // v0.88: his speed (units/s, smoothed)
	volatile double g_corm_seen = 0.0;  // v0.88: when that was last updated
	volatile double g_f1_time = 0.0;  // v0.87: fob_talk_scene_start (camp scene started)
	volatile double g_fe_time = 0.0;  // v0.87: fob_talk_scene_end (camp scene finished)  // v0.86: dialogue_start_fob_meetup (camp scene part 2)
	volatile double g_t_time = 0.0;   // v0.81: when trigger_allies_to_fob fired after that
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

	// v1.14/v1.15: key triggers per mission, drawn thick yellow in every [ mode, with labels.
	// Matched on x/y only (some are parked - moved far down - until a script switches them on).
	extern char g_rec_map[64];
	struct key_trig { const char* map; float x, y, lz; const char* text; };
	static const key_trig kKeyTrigs[] = {
		{"seoul", 16063.0f, 20176.0f, 1776.0f, "BINOC 1: ESCAPED SWARM"},
		{"seoul", 15852.0f, 20461.0f, 1932.0f, "BINOC 2: UPSTAIRS"},
		{"seoul", 15368.0f, 20700.0f, 1932.0f, "BINOC 3: WINDOW APPROACH"},
		{"seoul", 15188.0f, 20202.0f, 1860.0f, "BINOC 4: WINDOW (STARTS SCENE)"},
		{"recovery", -2783.0f, 3314.0f, 220.0f, "STAIRS DOWN - HIT EARLY (GIDEON STOPS WAITING)"},
		{"recovery", -2218.0f, 3041.0f, 140.0f, "ILONA TRIGGER - GIDEON GOES TO RANGE DOOR"},
		{"recovery", 380.0f, -2002.0f, 270.0f, "SIM 2: TRUCK TRIGGER"},
		{"recovery", 579.0f, -1500.0f, 660.0f, "SIM 2 TEST: EARLY AMBUSH (TOUCH IN THE AIR)"},
		{"recovery", 1973.0f, -2774.0f, 110.0f, "SIM 2: WARBIRD BOARDING SPOT"},
		{"recovery", 1506.0f, -2115.0f, -10360.0f, "ATLAS END CUBE - TOUCH = MISSION COMPLETE"},
	};
	// v1.26: Atlas's hidden ending trigger (#1714, sets outro_start -> nextmission). Box 903 x 936 x 178, parked far below.
	constexpr float END_CUBE_X = 1506.0f, END_CUBE_Y = -2115.0f, END_CUBE_Z = -10456.0f;
	constexpr float END_CUBE_HX = 451.5f, END_CUBE_HY = 468.0f, END_CUBE_HZ = 89.0f;
	bool is_end_cube(const float x, const float y)
	{
		return strcmp(g_rec_map, "recovery") == 0 && std::fabs(x - END_CUBE_X) < 10.0f && std::fabs(y - END_CUBE_Y) < 10.0f;
	}

	int binoc_index(const hit& h)
	{
		const int n = static_cast<int>(sizeof(kKeyTrigs) / sizeof(kKeyTrigs[0]));
		for (int i = 0; i < n; ++i)
			if (strcmp(g_rec_map, kKeyTrigs[i].map) == 0 && std::fabs(h.mid.x - kKeyTrigs[i].x) < 10.0f && std::fabs(h.mid.y - kKeyTrigs[i].y) < 10.0f) return i + 1;
		return 0;
	}

	void draw_box(const hit& h)
	{
		const float* color = color_for(h);
		g_line_override = h.flag == 1 ? 6.0f : 0.0f;
		// v1.14: the four triggers the binocular scene needs, drawn bright and thick (visible in every [ mode)
		if (binoc_index(h) > 0)
		{
			static const float yel[4] = {1.0f, 0.85f, 0.0f, 1.0f};
			if (!h.inside) color = yel;
			g_line_override = 6.0f;
		}

		if (is_end_cube(h.mid.x, h.mid.y))
		{
			// flashing bright cyan / white, very thick
			static float flash[4] = {0.0f, 1.0f, 1.0f, 1.0f};
			const bool on = std::fmod(now_seconds(), 0.6) < 0.3;
			flash[0] = on ? 1.0f : 0.0f; flash[1] = 1.0f; flash[2] = 1.0f;
			color = flash;
			g_line_override = 12.0f;
		}

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
#ifndef VK_OEM_1
#define VK_OEM_1 0xBA
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
		static const unsigned char kPlus[7] = {0,0x04,0x04,0x1F,0x04,0x04,0};
		static const unsigned char kOpen[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02};
		static const unsigned char kClose[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08};
		static const unsigned char kColon[7] = {0,0x0C,0x0C,0,0x0C,0x0C,0};
		static const unsigned char kSemi[7] = {0,0x0C,0x0C,0,0x0C,0x04,0x08};
		if (c == '+') return kPlus;
		if (c == '(') return kOpen;
		if (c == ')') return kClose;
		if (c == ':') return kColon;
		if (c == ';') return kSemi;
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
	thread_local bool g_in_code_notify = false;

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

	// ---- position bookmark: Home remembers where you stand, End puts you back there ----------
	// The work happens on the game thread (inside hooked_notify) so the game is not touched while it updates.
	volatile LONG g_dev_save_req = 0;
	volatile LONG g_dev_load_req = 0;
	volatile LONG g_dev_result = 0;   // 1 remembered, 2 moved back, -1 could not read/write
	float g_bm_pos[3] = {0, 0, 0};
	float g_bm_ang[2] = {0, 0};
	bool g_bm_ok = false;

	void dev_do_requests()
	{
		if (g_dev_save_req == 0 && g_dev_load_req == 0)
		{
			return;
		}

		const LONG save = InterlockedExchange(&g_dev_save_req, 0);
		const LONG load = InterlockedExchange(&g_dev_load_req, 0);
		__try
		{
			unsigned long long client = 0;
			if (!read_bytes(player_entity() + OFF_CLIENT, &client, sizeof(client)) || client < 0x140000000ull || client > 0x7FFFFFFE0000ull)
			{
				g_dev_result = -1;
				return;
			}

			float* org = reinterpret_cast<float*>(static_cast<uintptr_t>(client) + OFF_CAM_ORIGIN);
			if (save)
			{
				g_bm_pos[0] = org[0]; g_bm_pos[1] = org[1]; g_bm_pos[2] = org[2];
				g_bm_ang[0] = *reinterpret_cast<float*>(static_cast<uintptr_t>(client) + OFF_CAM_PITCH);
				g_bm_ang[1] = *reinterpret_cast<float*>(static_cast<uintptr_t>(client) + OFF_CAM_YAW);
				g_bm_ok = true;
				g_dev_result = 1;
				log_line("bookmark: saved position %.1f %.1f %.1f (looking pitch %.1f yaw %.1f)", g_bm_pos[0], g_bm_pos[1], g_bm_pos[2], g_bm_ang[0], g_bm_ang[1]);
			}
			else if (load)
			{
				if (!g_bm_ok)
				{
					g_dev_result = -1;
					return;
				}

				// the player's position lives in the client block; the entity keeps a copy that we move too
				org[0] = g_bm_pos[0]; org[1] = g_bm_pos[1]; org[2] = g_bm_pos[2];
				float* ent = reinterpret_cast<float*>(player_entity() + OFF_ORIGIN);
				ent[0] = g_bm_pos[0]; ent[1] = g_bm_pos[1]; ent[2] = g_bm_pos[2];
				float* vel = reinterpret_cast<float*>(static_cast<uintptr_t>(client) + OFF_CAM_ORIGIN + 12);
				vel[0] = 0; vel[1] = 0; vel[2] = 0;   // stop all movement
				g_dev_result = 2;
				log_line("bookmark: moved back to %.1f %.1f %.1f", g_bm_pos[0], g_bm_pos[1], g_bm_pos[2]);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			g_dev_result = -1;
		}
	}


	// ------------------------------------------------------------------ v1.27: lab mode (glitchlab experiments)
	// OFF unless the file speedrun_data\lab\lab.ini exists. Then the overlay reads speedrun_data\lab\queue.txt,
	// one experiment per line:   id x y z vx vy vz seconds [stop_z]
	// For each one it moves the player to x y z, sets the velocity, and writes every game frame to
	// speedrun_data\lab\result_<id>.txt ("ms x y z vx vy vz pm_flags") until 'seconds' of game time pass or
	// the player drops below stop_z. Finished ids are added to lab\done.txt. Nothing else changes.
	struct lab_exp
	{
		char id[64];
		float pos[3];
		float vel[3];
		float seconds;
		float stop_z;
	};
	volatile LONG g_lab_on = 0;
	volatile LONG g_lab_state = 0;      // 0 idle, 1 start requested (main thread), 2 running (game thread)
	lab_exp g_lab_cur = {};
	unsigned int g_lab_start_ms = 0, g_lab_last_ms = 0;
	FILE* g_lab_out = nullptr;
	// v1.28: wait for the player to be settled (alive, clock running steadily) before each test, and retry a
	// test that a death/respawn/checkpoint load interrupted instead of marking it done
	unsigned int g_lab_seen_ms = 0, g_lab_steady_since = 0;
	float g_lab_prev[3] = {};
	float g_lab_prev_rec[3] = {};
	bool g_lab_have_prev = false;

	void lab_path(char* out, const char* name)
	{
		strcpy_s(out, MAX_PATH, g_dir);
		strcat_s(out, MAX_PATH, "speedrun_data\\lab\\");
		strcat_s(out, MAX_PATH, name);
	}

	void lab_mark_done(const char* id, const char* why)
	{
		char path[MAX_PATH];
		lab_path(path, "done.txt");
		FILE* f = nullptr;
		if (fopen_s(&f, path, "a") == 0 && f)
		{
			fprintf(f, "%s %s\n", id, why);
			fclose(f);
		}
	}

	// main thread, about once a second
	void lab_poll()
	{
		char path[MAX_PATH];
		lab_path(path, "lab.ini");
		const bool on = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
		if (on != (g_lab_on != 0))
		{
			InterlockedExchange(&g_lab_on, on ? 1 : 0);
			log_line("lab mode %s", on ? "ON (speedrun_data\\lab\\lab.ini found)" : "off");
		}
		if (!on || g_lab_state != 0)
		{
			return;
		}
		// ids already done
		static char done[16384];
		done[0] = 0;
		lab_path(path, "done.txt");
		FILE* f = nullptr;
		if (fopen_s(&f, path, "r") == 0 && f)
		{
			char line[256];
			size_t used = 0;
			while (fgets(line, sizeof(line), f))
			{
				char id[64] = {};
				if (sscanf_s(line, "%63s", id, static_cast<unsigned>(sizeof(id))) == 1 && used + strlen(id) + 3 < sizeof(done))
				{
					strcat_s(done, sizeof(done), " ");
					strcat_s(done, sizeof(done), id);
					strcat_s(done, sizeof(done), " ");
					used = strlen(done);
				}
			}
			fclose(f);
		}
		lab_path(path, "queue.txt");
		if (fopen_s(&f, path, "r") != 0 || !f)
		{
			return;
		}
		char line[512];
		while (fgets(line, sizeof(line), f))
		{
			lab_exp e = {};
			e.stop_z = -1.0e9f;
			const int n = sscanf_s(line, "%63s %f %f %f %f %f %f %f %f", e.id, static_cast<unsigned>(sizeof(e.id)),
			                       &e.pos[0], &e.pos[1], &e.pos[2], &e.vel[0], &e.vel[1], &e.vel[2], &e.seconds, &e.stop_z);
			if (n < 8 || e.id[0] == '#')
			{
				continue;
			}
			char key[70];
			snprintf(key, sizeof(key), " %s ", e.id);
			if (strstr(done, key))
			{
				continue;
			}
			g_lab_cur = e;
			InterlockedExchange(&g_lab_state, 1);
			break;
		}
		fclose(f);
	}

	// game thread (called from the notify hook, many times a frame)
	void lab_tick()
	{
		if (g_lab_on == 0 || g_lab_state == 0)
		{
			return;
		}
		__try
		{
			unsigned long long client = 0;
			if (!read_bytes(player_entity() + OFF_CLIENT, &client, sizeof(client)) || client < 0x140000000ull || client > 0x7FFFFFFE0000ull)
			{
				return;
			}
			float* org = reinterpret_cast<float*>(static_cast<uintptr_t>(client) + OFF_CAM_ORIGIN);
			float* vel = org + 3;
			const unsigned int ms = *reinterpret_cast<unsigned int*>(static_cast<uintptr_t>(client) + 0x4C);
			const unsigned int flags = *reinterpret_cast<unsigned int*>(static_cast<uintptr_t>(client) + 0x54);
			// steadiness: the game clock moves forward in small steps and the player isn't being moved around
			if (ms != g_lab_seen_ms)
			{
				const float dx = org[0] - g_lab_prev[0], dy = org[1] - g_lab_prev[1], dz = org[2] - g_lab_prev[2];
				const bool jumpy = dx * dx + dy * dy + dz * dz > 300.0f * 300.0f;
				if (ms < g_lab_seen_ms || ms > g_lab_seen_ms + 500 || (jumpy && g_lab_state != 2))
				{
					g_lab_steady_since = ms;
				}
				g_lab_seen_ms = ms;
				g_lab_prev[0] = org[0]; g_lab_prev[1] = org[1]; g_lab_prev[2] = org[2];
			}
			if (g_lab_state == 1)
			{
				if (ms - g_lab_steady_since < 3000)
				{
					return;      // wait 3 s of steady game time (after loading, dying, respawning)
				}
				org[0] = g_lab_cur.pos[0]; org[1] = g_lab_cur.pos[1]; org[2] = g_lab_cur.pos[2];
				float* ent = reinterpret_cast<float*>(player_entity() + OFF_ORIGIN);
				ent[0] = org[0]; ent[1] = org[1]; ent[2] = org[2];
				vel[0] = g_lab_cur.vel[0]; vel[1] = g_lab_cur.vel[1]; vel[2] = g_lab_cur.vel[2];
				char name[96], path[MAX_PATH];
				snprintf(name, sizeof(name), "result_%s.txt", g_lab_cur.id);
				lab_path(path, name);
				if (g_lab_out) { fclose(g_lab_out); g_lab_out = nullptr; }
				fopen_s(&g_lab_out, path, "w");
				if (g_lab_out)
				{
					fprintf(g_lab_out, "# lab %s map %s tool %s start %.2f %.2f %.2f vel %.2f %.2f %.2f seconds %.2f stop_z %.1f\n",
					        g_lab_cur.id, g_rec_map, ADDON_VERSION, org[0], org[1], org[2], vel[0], vel[1], vel[2],
					        g_lab_cur.seconds, g_lab_cur.stop_z);
				}
				g_lab_start_ms = ms;
				g_lab_last_ms = ms;
				g_lab_prev_rec[0] = org[0]; g_lab_prev_rec[1] = org[1]; g_lab_prev_rec[2] = org[2];
				g_lab_have_prev = true;
				InterlockedExchange(&g_lab_state, 2);
				return;
			}
			if (ms == g_lab_last_ms)
			{
				return;
			}
			const float jx = org[0] - g_lab_prev_rec[0], jy = org[1] - g_lab_prev_rec[1], jz = org[2] - g_lab_prev_rec[2];
			const bool moved_away = g_lab_have_prev && (jx * jx + jy * jy + jz * jz) > 1500.0f * 1500.0f &&
			                        (ms - g_lab_last_ms) < 200;
			if (ms < g_lab_start_ms || moved_away)    // death/respawn, checkpoint load or restart during the test
			{
				if (g_lab_out) { fprintf(g_lab_out, "END interrupted_retry\n"); fclose(g_lab_out); g_lab_out = nullptr; }
				g_lab_steady_since = ms;
				g_lab_have_prev = false;
				InterlockedExchange(&g_lab_state, 1);      // not marked done: run it again once things settle
				return;
			}
			g_lab_last_ms = ms;
			g_lab_prev_rec[0] = org[0]; g_lab_prev_rec[1] = org[1]; g_lab_prev_rec[2] = org[2];
			g_lab_have_prev = true;
			if (g_lab_out)
			{
				fprintf(g_lab_out, "%u %.2f %.2f %.2f %.2f %.2f %.2f %X\n", ms - g_lab_start_ms, org[0], org[1], org[2],
				        vel[0], vel[1], vel[2], flags);
			}
			const bool timeout = (ms - g_lab_start_ms) >= static_cast<unsigned int>(g_lab_cur.seconds * 1000.0f);
			const bool below = org[2] < g_lab_cur.stop_z;
			if (timeout || below)
			{
				if (g_lab_out) { fprintf(g_lab_out, "END %s\n", below ? "stop_z" : "time"); fclose(g_lab_out); g_lab_out = nullptr; }
				lab_mark_done(g_lab_cur.id, below ? "stop_z" : "time");
				// stop the player so a fall doesn't carry on into anything
				vel[0] = vel[1] = vel[2] = 0.0f;
				InterlockedExchange(&g_lab_state, 0);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	void hooked_notify(const unsigned int entity, const unsigned int string_id, const unsigned int count)
	{
		dev_do_requests();
		lab_tick();
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

		g_in_code_notify = true;
		g_original_notify(entity, string_id, count);
		g_in_code_notify = false;
	}

	// Script-made notifies ("level notify" used by flag_set etc.) do not pass through the function above.
	// They all end in the VM notify function, so we watch that one too and log the ones the first hook did not see.
	void rec_snotify(const char* name, unsigned int owner);
	typedef unsigned long long (*vm_notify_t)(unsigned int, unsigned int, void*);
	vm_notify_t g_original_vm_notify = nullptr;
	constexpr uintptr_t SP_VM_NOTIFY = 0x140320E50ull;

	unsigned long long hooked_vm_notify(const unsigned int owner, const unsigned int string_id, void* top)
	{
		if (!g_in_code_notify && string_id != 0 && string_id < 0x100000)
		{
			__try
			{
				char name[44];
				if (class_name(string_id, name, sizeof(name)))
				{
					static double last_time[1024];
					unsigned int h = 5381;
					for (const char* c = name; *c; ++c) h = h * 33 + static_cast<unsigned char>(*c);
					const double now = now_seconds();
					if (now - last_time[h & 1023] >= 0.25)
					{
						last_time[h & 1023] = now;
						rec_snotify(name, owner);
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
		}

		return g_original_vm_notify(owner, string_id, top);
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
		float y = 10.0f + 3.0f * (7.0f * scale + 12.0f);   // v1.19/v1.26: below the coordinates and end-cube line

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
	extern char g_rec_map[64];   // v0.90: defined with the recorder further down

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
	const float kCollisionRanges[4] = {180.0f, 500.0f, 1200.0f, 3000.0f};   // v1.19: shortest = about 15 feet
	vec3 g_feet = {0, 0, 0};   // v1.19: player feet position (for the coordinates display and the ' key)
	volatile bool g_here_requested = false;

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


	// ---- v1.17: the level's real collision brushes (Delete key, modes 1 and 2) ----------------------
	// Every brush is a closed convex shape. We work out its corners and faces once, then draw the ones near
	// the player. Class 1 = invisible wall (blocks the player but every face is a clip/caulk material),
	// class 2 = mantle helper, class 3 = ordinary visible solid brush.
	struct cbrush_draw
	{
		float lo[3], hi[3];
		int first_edge, edge_count;   // into g_bedges (pairs of points)
		int first_face, face_count;   // into g_bfaces
		unsigned int contents;
		unsigned char cls;
		short model;   // v1.23: -1 = level, otherwise a brush model (its shape is stored relative to the object)
	};

	// v1.23: brush models (objects like hangar_reveal): which entity slot carries each one, and where it is now
	constexpr int MAX_MODELS = 1024;
	volatile int g_model_slot[MAX_MODELS];      // entity slot, -1 = not found
	float g_model_local_mid[MAX_MODELS][3];

	struct cbrush_face
	{
		int first_point, point_count;   // into g_bpoints
	};

	cbrush_draw* volatile g_bbrushes = nullptr;
	volatile int g_bbrush_count = 0;
	vec3* volatile g_bedges = nullptr;
	cbrush_face* volatile g_bfaces = nullptr;
	vec3* volatile g_bpoints = nullptr;
	volatile int g_collision_mode = 0;   // Delete: 0 off, 1 invisible walls, 2 invisible walls + all solid brushes, 3 old leaf boxes

	void draw_poly_fill(const vec3* pts, const int n, const float* color)
	{
		if (n < 3 || n > 64)
		{
			return;
		}

		// camera space, then trim against the near plane
		vec3 in[64];
		vec3 out[130];
		for (int i = 0; i < n; ++i) in[i] = to_camera(pts[i]);
		const float near_plane = 4.0f;
		int m = 0;
		for (int i = 0; i < n; ++i)
		{
			const vec3& a = in[i];
			const vec3& b = in[(i + 1) % n];
			const bool ina = a.z >= near_plane;
			const bool inb = b.z >= near_plane;
			if (ina) out[m++] = a;
			if (ina != inb)
			{
				const float t = (near_plane - a.z) / (b.z - a.z);
				out[m++] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, near_plane};
			}
		}

		if (m < 3)
		{
			return;
		}

		float sx[130], sy[130];
		for (int i = 0; i < m; ++i) to_screen(out[i], &sx[i], &sy[i]);
		for (int i = 1; i + 1 < m; ++i)
		{
			add_quad(sx[0], sy[0], sx[i], sy[i], sx[i + 1], sy[i + 1], sx[i + 1], sy[i + 1], color);
		}
	}

	void draw_collision_brushes(const vec3& player)
	{
		cbrush_draw* brushes = g_bbrushes;
		const int count = g_bbrush_count;
		vec3* edges = g_bedges;
		cbrush_face* faces = g_bfaces;
		vec3* points = g_bpoints;
		if (!brushes || count <= 0 || !edges || !faces || !points)
		{
			return;
		}

		// where each brush model is right now (its entity's origin)
		static float offs[MAX_MODELS][3];
		static bool off_ok[MAX_MODELS];
		static bool in_use[MAX_MODELS];
		static int in_use_count = -1;
		if (in_use_count != count)
		{
			for (int m = 0; m < MAX_MODELS; ++m) in_use[m] = false;
			for (int i = 0; i < count; ++i) if (brushes[i].model >= 0 && brushes[i].model < MAX_MODELS) in_use[brushes[i].model] = true;
			in_use_count = count;
		}
		for (int m = 0; m < MAX_MODELS; ++m)
		{
			off_ok[m] = false;
			if (!in_use[m]) continue;
			const int slot = g_model_slot[m];
			if (slot < 0) continue;
			float o[3];
			if (read_bytes(SP_G_ENTITIES + static_cast<uintptr_t>(slot) * g_stride + OFF_ORIGIN, o, sizeof(o)) &&
				std::fabs(o[0]) < 200000.0f && std::fabs(o[1]) < 200000.0f && std::fabs(o[2]) < 200000.0f)
			{
				offs[m][0] = o[0]; offs[m][1] = o[1]; offs[m][2] = o[2];
				off_ok[m] = true;
			}
		}

		const bool all = g_collision_mode == 2;
		constexpr int KEEP = 700;
		float best_dist[KEEP];
		int best_index[KEEP];
		int kept = 0;
		const float max_distance = kCollisionRanges[g_collision_range];
		for (int i = 0; i < count; ++i)
		{
			const cbrush_draw& b = brushes[i];
			if (b.cls == 0 || (b.cls == 3 && !all))
			{
				continue;
			}

			float sh[3] = {0.0f, 0.0f, 0.0f};
			if (b.model >= 0)
			{
				if (b.model >= MAX_MODELS || !off_ok[b.model]) continue;
				sh[0] = offs[b.model][0]; sh[1] = offs[b.model][1]; sh[2] = offs[b.model][2];
			}

			float d2 = 0.0f;
			const float p[3] = {player.x - sh[0], player.y - sh[1], player.z - sh[2]};
			for (int k = 0; k < 3; ++k)
			{
				const float e = p[k] < b.lo[k] ? b.lo[k] - p[k] : (p[k] > b.hi[k] ? p[k] - b.hi[k] : 0.0f);
				d2 += e * e;
			}

			if (d2 > max_distance * max_distance)
			{
				continue;
			}

			if (b.cls == 3) d2 += 1.0e4f;   // invisible walls first when the list is full
			if (kept >= KEEP && d2 >= best_dist[KEEP - 1])
			{
				continue;
			}

			int position = kept < KEEP ? kept : KEEP - 1;
			if (kept < KEEP) ++kept;
			while (position > 0 && best_dist[position - 1] > d2)
			{
				best_dist[position] = best_dist[position - 1];
				best_index[position] = best_index[position - 1];
				--position;
			}

			best_dist[position] = d2;
			best_index[position] = i;
		}

		static const float wall_line[4] = {0.75f, 0.35f, 1.0f, 1.0f};
		static const float wall_fill[4] = {0.7f, 0.2f, 1.0f, 0.13f};
		static const float mantle_line[4] = {0.3f, 1.0f, 0.75f, 0.9f};
		static const float solid_line[4] = {0.6f, 0.6f, 0.6f, 0.45f};
		static const float model_line[4] = {1.0f, 0.55f, 0.1f, 1.0f};     // v1.23: brush models (orange)
		static const float model_fill[4] = {1.0f, 0.5f, 0.05f, 0.13f};
		g_thin_lines = true;
		for (int n = kept - 1; n >= 0; --n)
		{
			const cbrush_draw& b = brushes[best_index[n]];
			const bool is_model = b.model >= 0;
			const float* line = is_model ? model_line : (b.cls == 1 ? wall_line : (b.cls == 2 ? mantle_line : solid_line));
			const float* fill = is_model ? model_fill : wall_fill;
			float sh[3] = {0.0f, 0.0f, 0.0f};
			if (is_model) { sh[0] = offs[b.model][0]; sh[1] = offs[b.model][1]; sh[2] = offs[b.model][2]; }
			if (b.cls == 1)
			{
				for (int f = 0; f < b.face_count; ++f)
				{
					const cbrush_face& face = faces[b.first_face + f];
					if (!is_model)
					{
						draw_poly_fill(points + face.first_point, face.point_count, fill);
						continue;
					}

					vec3 moved[64];
					const int pc = face.point_count < 64 ? face.point_count : 64;
					for (int q = 0; q < pc; ++q)
					{
						const vec3& v = points[face.first_point + q];
						moved[q] = {v.x + sh[0], v.y + sh[1], v.z + sh[2]};
					}
					draw_poly_fill(moved, pc, fill);
				}
			}

			for (int e = 0; e < b.edge_count; ++e)
			{
				const vec3& a = edges[(b.first_edge + e) * 2];
				const vec3& c = edges[(b.first_edge + e) * 2 + 1];
				edge({a.x + sh[0], a.y + sh[1], a.z + sh[2]}, {c.x + sh[0], c.y + sh[1], c.z + sh[2]}, line);
			}
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
			const bool end_cube = is_end_cube(b[0], b[1]);
			if (end_candidate && !end_cube && b[2] - player.z > 8000.0f && b[2] - player.z < 14000.0f)
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
			if (dist > max_distance && !end_candidate && !end_cube && !(kind == 3 && dist < 6000.0f) && !(barrier && dist < (g_show_mode == 2 ? 20000.0f : 4000.0f)))
			{
				continue;
			}

			hit h;
			h.dist = dist;
			h.key = end_cube ? -2.0e9f : (end_candidate ? dist - 1.0e9f : dist);
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
				// v1.14: the binocular-scene triggers stay visible in every [ mode (e.g. leave-area only)
				const bool start_hl = binoc_index(h) > 0;
				const int mode = start_hl ? 0 : g_show_mode;
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
		static bool was_backslash;
		if (key_pressed(VK_OEM_5, &was_backslash))
		{
			g_route_on = !g_route_on;
			log_line("\\ key: route guide lines %s", g_route_on ? "ON" : "OFF");
		}
		static bool was_semicolon;
		if (key_pressed(VK_OEM_1, &was_semicolon))
		{
			g_speedo_mode = (g_speedo_mode + 2) % 3;
			log_line("; key: speedometer mode %d (2 = speed + strafe helper, 1 = speed only, 0 = off)", static_cast<int>(g_speedo_mode));
		}
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
			g_collision_range = (g_collision_range + 1) % 4;
			log_line("PageDown: collision blocks range %d", static_cast<int>(g_collision_range));
		}

		if (delete_key)
		{
			g_collision_mode = (g_collision_mode + 1) % 4;
			g_collision_on = g_collision_mode == 3;
			g_collision_refresh = g_collision_mode != 0;
			static const char* const kModes[] = {"OFF", "INVISIBLE WALLS", "INVISIBLE WALLS + ALL SOLID BRUSHES", "OLD COLLISION BLOCKS"};
			log_line("Delete: collision view %s", kModes[g_collision_mode]);
		}

		{
			static bool was_quote;
			if (key_pressed(0xDE, &was_quote))   // ' key
			{
				g_here_requested = true;
				log_line("': saving the collision pieces around X %.0f Y %.0f Z %.0f", g_feet.x, g_feet.y, g_feet.z);
			}
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

		// v1.16: scripts change the real field of view (lerpfov: drones, cutscenes, the warbird) without touching
		// cg_fov, so the boxes drift. The game's render view keeps x, y, width, height and the two half-FOV tangents
		// just before the eye position that camera sources 1-3 point at - use those when they look valid.
		if (g_source >= 1 && g_source <= 3)
		{
			struct { int x, y, w, h; float tx, ty; } rd = {};
			if (read_bytes(kSources[g_source].address - 0x18, &rd, sizeof(rd)) && rd.w >= 320 && rd.w <= 16384 && rd.h >= 200 && rd.h <= 16384 &&
				rd.tx > 0.05f && rd.tx < 20.0f && rd.ty > 0.05f && rd.ty < 20.0f && std::fabs(rd.tx / rd.ty - static_cast<float>(rd.w) / rd.h) < 0.08f)
			{
				static double last_log = 0.0;
				static float last_tx = 0.0f;
				if (std::fabs(rd.tx - last_tx) > 0.01f && now_seconds() - last_log > 5.0)
				{
					log_line("view: game FOV tangents %.3f %.3f (%dx%d) - cg_fov says %.1f", rd.tx, rd.ty, rd.w, rd.h, g_fov);
					last_log = now_seconds(); last_tx = rd.tx;
				}
				g_tan_x = rd.tx;
				g_tan_y = rd.ty;
			}
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

		// v1.02: finale plant timing. If the camp dialogue starts right after the binoculars (S), Cormack's wave
		// comes ~17.3 s after S and pulls Will away. Planting BEFORE the wave = soft lock (wave lands mid-plant);
		// planting AFTER the wave = part 2 skipped (seoul_20261029_215054).
		if (g_s_time > 0.0 && now_seconds() - g_s_time < 60.0 && g_press_time < g_s_time)
		{
			const double now2 = now_seconds();
			const float sc = g_h >= 1200.0f ? 4.0f : 3.0f;
			const float red[4] = {1.0f, 0.15f, 0.15f, 1.0f};
			const float grn[4] = {0.2f, 1.0f, 0.2f, 1.0f};
			const float gry[4] = {0.85f, 0.85f, 0.85f, 1.0f};
			char buf[96];
			const bool setup = g_f2_time >= g_s_time - 0.5 && g_f2_time - g_s_time < 6.0;   // camp wave still to come
			const float org[4] = {1.0f, 0.55f, 0.1f, 1.0f};
			if (g_will_camp_time > g_s_time)
			{
				draw_text(g_w * 0.5f - 21.0f * 3.0f * sc, g_h * 0.36f, "WILL SENT TO CAMP - NO PLANT - RESTART", sc, red);
			}
			else if (g_prompt_time < g_s_time)
			{
			}
			else if (setup && g_wave_time < g_s_time)
			{
				// v1.14: soft-lock guard only (planting just before Cormack's wave locks the game)
				const double eta = (g_f2_time + 16.8) - now2;
				snprintf(buf, sizeof(buf), "WAVE COMING - DO NOT PLANT YET  %.1f", eta > 0.0 ? eta : 0.0);
				draw_text(g_w * 0.5f - static_cast<float>(strlen(buf)) * 3.0f * sc, g_h * 0.36f, buf, sc, org);
			}
			else if (setup && g_wave_time >= g_s_time && now2 - g_wave_time < 2.5)
			{
				const float yel[4] = {1.0f, 0.85f, 0.1f, 1.0f};
				snprintf(buf, sizeof(buf), "WAVE - WAIT  %.1f", 2.5 - (now2 - g_wave_time));
				draw_text(g_w * 0.5f - static_cast<float>(strlen(buf)) * 3.0f * sc, g_h * 0.36f, buf, sc, yel);
			}
			else if (setup && g_wave_time >= g_s_time && now2 - g_wave_time < 6.0)
			{
				draw_text(g_w * 0.5f - 9.0f * 3.0f * sc, g_h * 0.36f, "PLANT NOW", sc, grn);
			}
			(void)gry;
		}

		// v1.14/v1.15: labels on the key triggers of this mission
		for (const key_trig& l : kKeyTrigs)
		{
			if (strcmp(g_rec_map, l.map) != 0) continue;
			const vec3 c = to_camera({l.x, l.y, l.lz});
			const float dx = l.x - origin[0], dy = l.y - origin[1];
			if (c.z > 20.0f && (dx * dx + dy * dy < 6000.0f * 6000.0f || is_end_cube(l.x, l.y)))
			{
				float sx, sy;
				to_screen(c, &sx, &sy);
				const float col[4] = {1.0f, 0.85f, 0.0f, 1.0f};
				const float sc = g_h >= 1200.0f ? 3.0f : 2.0f;
				draw_text(sx - static_cast<float>(strlen(l.text)) * 3.0f * sc, sy - 10.0f * sc, l.text, sc, col);
			}
		}

		// v0.92: route guide - thin white straight lines between the key points of the Seoul route (\\ key toggles)
		if (g_route_on && strcmp(g_rec_map, "seoul") == 0)
		{
			struct wp { float x, y, z; const char* name; };
			static const wp route[] = {
				{35946.0f, 10431.0f, 4328.0f, "START"},
				{34869.0f, 8683.0f, 1336.0f, "DROP LANDING"},
				{32813.0f, 6098.0f, 1359.0f, "TENT SKIP"},
				{28188.0f, 7466.0f, 1316.0f, "TURRET AREA (VAN WARP)"},
				{28107.0f, 7452.0f, 1329.0f, "TURRET"},
				{23934.0f, 12530.0f, 2212.0f, "CONVENTION CENTRE EXIT"},
				{21322.0f, 13698.0f, 2957.0f, "HIGH HOVER"},
				{16063.0f, 20176.0f, 1626.0f, "1: ESCAPED-SWARM TRIGGER"},
				{15852.0f, 20461.0f, 1792.0f, "2: UPSTAIRS TRIGGER"},
				{15368.0f, 20700.0f, 1792.0f, "3: WINDOW-APPROACH TRIGGER"},
				{12930.0f, 19559.0f, 1333.0f, "CHARGES (THEN GUARDS)"},
				{15188.0f, 20202.0f, 1770.0f, "BINOCULARS (WINDOW, BACK SIDE)"},
				{13960.0f, 17022.0f, 1377.0f, "PLATFORM (PLANT)"},
			};
			const float wht[4] = {1.0f, 1.0f, 1.0f, 0.85f};
			const float lab[4] = {1.0f, 1.0f, 1.0f, 0.95f};
			const int n = static_cast<int>(sizeof(route) / sizeof(route[0]));
			g_thin_lines = true;
			for (int i = 0; i + 1 < n; ++i)
				edge({route[i].x, route[i].y, route[i].z}, {route[i + 1].x, route[i + 1].y, route[i + 1].z}, wht);
			for (int i = 0; i < n; ++i)
			{
				const vec3 p = {route[i].x, route[i].y, route[i].z};
				edge({p.x - 40.0f, p.y, p.z}, {p.x + 40.0f, p.y, p.z}, wht);
				edge({p.x, p.y - 40.0f, p.z}, {p.x, p.y + 40.0f, p.z}, wht);
				edge({p.x, p.y, p.z - 40.0f}, {p.x, p.y, p.z + 40.0f}, wht);
				const vec3 c = to_camera(p);
				const float dx = p.x - origin[0], dy = p.y - origin[1], dz = p.z - origin[2];
				if (c.z > 50.0f && dx * dx + dy * dy + dz * dz < 9000.0f * 9000.0f)
				{
					float sx, sy;
					to_screen(c, &sx, &sy);
					draw_text(sx + 6.0f, sy - 6.0f, route[i].name, 1.0f, lab);
				}
			}
			g_thin_lines = false;
		}

		// v0.80: the "no Will" box (trigger_convention_center_combat_06, Seoul). Always drawn, very bright, thick.
		{
			const float m = 16.0f;   // player body radius, so the drawn edge is where touching really starts
			const float x0 = 24187.0f - m, x1 = 25053.0f + m, y0 = 11449.0f - m, y1 = 11675.0f + m, z0 = 1511.0f, z1 = 2057.0f;
			const float px = origin[0], py = origin[1], pz = origin[2];
			const float dx = px < x0 ? x0 - px : (px > x1 ? px - x1 : 0.0f);
			const float dy = py < y0 ? y0 - py : (py > y1 ? py - y1 : 0.0f);
			const float hd = std::sqrt(dx * dx + dy * dy);
			if (hd < 5000.0f && std::fabs(pz - 1784.0f) < 2500.0f)
			{
				const bool pulse = (static_cast<long long>(now_seconds() * 3.0) & 1) != 0;
				const float red[4] = {1.0f, 0.05f, 0.05f, 1.0f};
				const float yel[4] = {1.0f, 1.0f, 0.0f, 1.0f};
				const float gr2[4] = {0.1f, 1.0f, 0.1f, 1.0f};
				const float wht[4] = {1.0f, 1.0f, 1.0f, 1.0f};
				// v0.89: if the camp scene already started (bad roll), touching this box makes Cormack walk to it,
				// which lets the camp scene finish early (run 173300) - so then it is a GOOD box.
				const bool rescue = g_v_time > 0.0 && g_f1_time > g_v_time && !(g_fe_time > g_v_time) && !(g_s_time > g_v_time);
				const float* col = rescue ? (pulse ? gr2 : wht) : (pulse ? red : yel);
				vec3 c[8];
				for (int i = 0; i < 8; ++i)
				{
					c[i].x = (i & 1) ? x1 : x0;
					c[i].y = (i & 2) ? y1 : y0;
					c[i].z = (i & 4) ? z1 : z0;
				}
				g_line_override = 9.0f;
				for (int i = 0; i < 8; ++i)
					for (int bit = 1; bit <= 4; bit <<= 1)
						if ((i & bit) == 0) edge(c[i], c[i | bit], col);
				// X across floor and ceiling, and posts along the two long sides
				edge(c[0], c[3], col); edge(c[1], c[2], col);
				edge(c[4], c[7], col); edge(c[5], c[6], col);
				for (int k = 1; k < 8; ++k)
				{
					const float x = x0 + (x1 - x0) * static_cast<float>(k) / 8.0f;
					edge({x, y0, z0}, {x, y0, z1}, col);
					edge({x, y1, z0}, {x, y1, z1}, col);
				}
				g_line_override = 0.0f;
				const bool inside = dx == 0.0f && dy == 0.0f && pz > z0 - 70.0f && pz < z1;
				const float sc = g_h >= 1200.0f ? 6.0f : 4.0f;
				if (rescue && false)
				{
					if (inside)
						draw_text(g_w * 0.5f - 13.0f * 3.0f * sc, g_h * 0.12f, "TOUCHED - GOOD", sc, gr2);
					else if (hd < 1500.0f)
						draw_text(g_w * 0.5f - 18.0f * 3.0f * sc, g_h * 0.12f, "TOUCH THIS BOX - SAVES WILL", sc, gr2);
				}
				else if (inside && false)
					draw_text(g_w * 0.5f - 15.0f * 3.0f * sc, g_h * 0.12f, "NO WILL BOX - BACK OUT", sc, red);
				else if (false && hd < 250.0f && pz > z0 - 70.0f && pz < z1)
					draw_text(g_w * 0.5f - 11.0f * 3.0f * sc, g_h * 0.12f, "CLOSE TO WILL BOX", sc, yel);
			}
		}

		// v0.94: speedometer + strafe helper. Reads your own speed (playerState velocity, checked against
		// how fast your position actually changes) and learns, from your own jumps, which angle between
		// your movement keys and your direction of travel gains speed in the air at each speed.
		if (g_speedo_mode > 0)
		{
			static unsigned int s_gt = 0;
			static float s_pos[3] = {};
			static float s_hs = 0.0f, s_disp = 0.0f, s_trend = 0.0f;
			static int s_good = 0, s_bad = 0;
			static bool s_air = false;
			static float s_take[6] = {};
			static int s_ntake = 0;
			static float s_take_speed = 0.0f, s_last_gain = 0.0f;
			static double s_land_time = 0.0;
			static float s_sum[16][46] = {}, s_w[16][46] = {};
			static float s_angle = -1.0f;
			float p[3] = {}, v[3] = {}, ryaw = 0.0f;
			unsigned int gt = 0;
			const uintptr_t cl = static_cast<uintptr_t>(pointer);
			const bool ok = read_bytes(cl + OFF_CAM_ORIGIN, p, sizeof(p)) && read_bytes(cl + OFF_CAM_ORIGIN + 12, v, sizeof(v)) &&
				read_bytes(cl + OFF_CAM_YAW, &ryaw, sizeof(ryaw)) && read_bytes(cl + 0x4C, &gt, sizeof(gt));
			if (ok && gt != s_gt)
			{
				const float dt = static_cast<float>(static_cast<int>(gt - s_gt)) / 1000.0f;
				const float dx = p[0] - s_pos[0], dy = p[1] - s_pos[1], dz = p[2] - s_pos[2];
				const bool cont = s_gt != 0 && dt > 0.0f && dt < 0.3f && dx * dx + dy * dy < 400.0f * 400.0f;
				const float pos_speed = cont ? std::sqrt(dx * dx + dy * dy) / dt : 0.0f;
				const bool vel_sane = std::fabs(v[0]) < 20000.0f && std::fabs(v[1]) < 20000.0f && std::fabs(v[2]) < 20000.0f;
				const float vel_speed = vel_sane ? std::sqrt(v[0] * v[0] + v[1] * v[1]) : 0.0f;
				if (cont && pos_speed > 80.0f && s_good + s_bad < 400)
				{
					if (std::fabs(vel_speed - pos_speed) < 0.3f * pos_speed) ++s_good; else ++s_bad;
				}
				const bool use_vel = vel_sane && s_good >= s_bad;
				const float hs = use_vel ? vel_speed : pos_speed;
				const bool ground = use_vel ? (v[2] == 0.0f) : (cont && std::fabs(dz) < 0.01f);
				float vyaw = use_vel ? std::atan2(v[1], v[0]) : std::atan2(dy, dx);
				vyaw *= 57.29578f;
				if (cont)
				{
					// take-off / landing
					if (!ground && !s_air)
					{
						s_air = true;
						s_take_speed = s_hs;
						for (int i = 5; i > 0; --i) s_take[i] = s_take[i - 1];
						s_take[0] = s_hs;
						if (s_ntake < 6) ++s_ntake;
					}
					else if (ground && s_air)
					{
						s_air = false;
						s_last_gain = s_hs - s_take_speed;
						s_land_time = now_seconds();
					}
					// which way do the movement keys push?
					const int fwd = ((GetAsyncKeyState('W') & 0x8000) ? 1 : 0) - ((GetAsyncKeyState('S') & 0x8000) ? 1 : 0);
					const int side = ((GetAsyncKeyState('D') & 0x8000) ? 1 : 0) - ((GetAsyncKeyState('A') & 0x8000) ? 1 : 0);
					s_angle = -1.0f;
					if ((fwd != 0 || side != 0) && hs > 100.0f)
					{
						const float wish = ryaw + std::atan2(static_cast<float>(-side), static_cast<float>(fwd)) * 57.29578f;
						float a = std::fmod(wish - vyaw + 540.0f, 360.0f) - 180.0f;
						a = std::fabs(a);
						s_angle = a;
						const float gain = (hs - s_hs) / dt;
						if (s_air && !ground && a <= 90.0f && std::fabs(gain) < 2500.0f)
						{
							const int band = hs >= 1500.0f ? 15 : static_cast<int>(hs / 100.0f);
							const int b = static_cast<int>(a / 2.0f);
							s_sum[band][b] = s_sum[band][b] * 0.97f + gain;
							s_w[band][b] = s_w[band][b] * 0.97f + 1.0f;
						}
					}
				}
				s_hs = hs;
				s_gt = gt;
				s_pos[0] = p[0]; s_pos[1] = p[1]; s_pos[2] = p[2];
			}
			// display
			const float prev = s_disp;
			s_disp += (s_hs - s_disp) * 0.25f;
			s_trend = s_trend * 0.9f + (s_disp - prev) * 0.1f;
			const float s1 = g_h >= 1200.0f ? 1.5f : 1.0f;
			const float big = 4.0f * s1;
			const float cx = g_w * 0.5f, top = g_h * 0.62f;
			const float wht[4] = {1.0f, 1.0f, 1.0f, 0.9f};
			const float grn[4] = {0.3f, 1.0f, 0.3f, 0.95f};
			const float red[4] = {1.0f, 0.3f, 0.25f, 0.95f};
			const float dim[4] = {0.8f, 0.8f, 0.8f, 0.75f};
			char buf[96];
			snprintf(buf, sizeof(buf), "%d", static_cast<int>(s_disp + 0.5f));
			const float* sc = s_trend > 0.15f ? grn : (s_trend < -0.15f ? red : wht);
			draw_text(cx - static_cast<float>(strlen(buf)) * 3.0f * big, top, buf, big, sc);
			draw_text(cx - 9.0f * s1 * 1.0f, top + 8.0f * big, "UPS", s1, dim);
			if (s_good + s_bad >= 20 && s_bad > s_good)
				draw_text(cx + static_cast<float>(strlen(buf)) * 3.0f * big + 6.0f, top, "(POS)", s1, dim);
			if (g_speedo_mode == 2)
			{
				float y = top + 8.0f * big + 14.0f * s1;
				// last jump result (shown for 2.5 s after landing)
				if (now_seconds() - s_land_time < 2.5)
				{
					snprintf(buf, sizeof(buf), "LAST JUMP %+d", static_cast<int>(s_last_gain));
					draw_text(cx - static_cast<float>(strlen(buf)) * 3.0f * s1 * 1.5f, y, buf, s1 * 1.5f, s_last_gain >= 0.0f ? grn : red);
				}
				y += 14.0f * s1;
				// take-off speeds of the last hops, oldest first
				if (s_ntake > 1)
				{
					float x = cx - 3.0f * 5.0f * 6.0f * s1;
					draw_text(x, y, "HOPS", s1, dim);
					x += 30.0f * s1;
					for (int i = s_ntake - 1; i >= 0; --i)
					{
						snprintf(buf, sizeof(buf), "%d", static_cast<int>(s_take[i]));
						const bool up = i < s_ntake - 1 && s_take[i] >= s_take[i + 1];
						draw_text(x, y, buf, s1, i == s_ntake - 1 ? dim : (up ? grn : red));
						x += static_cast<float>(strlen(buf) + 1) * 6.0f * s1;
					}
				}
				y += 14.0f * s1;
				// learned angle bar: 0 to 90 degrees between where your keys push and where you are going
				const int band = s_hs >= 1500.0f ? 15 : static_cast<int>(s_hs / 100.0f);
				float best_avg = 0.0f;
				int best = -1;
				for (int b = 0; b < 46; ++b)
				{
					if (s_w[band][b] >= 3.0f)
					{
						const float avg = s_sum[band][b] / s_w[band][b];
						if (best < 0 || avg > best_avg) { best_avg = avg; best = b; }
					}
				}
				const float bw = 4.0f * s1, bh = 10.0f * s1, bx = cx - 23.0f * bw;
				const float back[4] = {0.0f, 0.0f, 0.0f, 0.35f};
				add_rect(bx - 2.0f, y - 2.0f, 46.0f * bw + 4.0f, bh + 4.0f, back);
				for (int b = 0; b < 46; ++b)
				{
					float c[4] = {0.5f, 0.5f, 0.5f, 0.25f};
					if (s_w[band][b] >= 3.0f)
					{
						const float avg = s_sum[band][b] / s_w[band][b];
						const float k = best_avg > 0.0f ? std::fmin(1.0f, std::fmax(0.0f, avg / best_avg)) : 0.0f;
						if (avg > 0.0f) { c[0] = 0.15f; c[1] = 0.4f + 0.6f * k; c[2] = 0.15f; c[3] = 0.35f + 0.55f * k; }
						else { c[0] = 0.8f; c[1] = 0.15f; c[2] = 0.15f; c[3] = 0.5f; }
					}
					add_rect(bx + static_cast<float>(b) * bw, y, bw - 1.0f, bh, c);
				}
				if (best >= 0 && best_avg > 0.0f)
				{
					const float gold[4] = {1.0f, 0.85f, 0.2f, 1.0f};
					add_rect(bx + static_cast<float>(best) * bw, y - 3.0f * s1, bw - 1.0f, bh + 6.0f * s1, gold);
				}
				if (s_angle >= 0.0f && s_angle <= 92.0f)
				{
					add_rect(bx + std::fmin(s_angle, 91.0f) / 2.0f * bw - 1.0f, y - 5.0f * s1, 3.0f, bh + 10.0f * s1, wht);
				}
				draw_text(bx - 4.0f * 6.0f * s1, y + 2.0f * s1, "0", s1, dim);
				draw_text(bx + 46.0f * bw + 6.0f, y + 2.0f * s1, "90", s1, dim);
				y += bh + 8.0f * s1;
				if (s_angle >= 0.0f)
					snprintf(buf, sizeof(buf), best >= 0 && best_avg > 0.0f ? "ANGLE %d   BEST %d AT THIS SPEED" : "ANGLE %d   LEARNING...", static_cast<int>(s_angle), best * 2 + 1);
				else
					snprintf(buf, sizeof(buf), best >= 0 && best_avg > 0.0f ? "NO KEYS   BEST %d AT THIS SPEED" : "HOLD W + A OR D IN THE AIR", best * 2 + 1);
				draw_text(cx - static_cast<float>(strlen(buf)) * 3.0f * s1, y, buf, s1, dim);
			}
		}

		// v0.81: tells you early whether Will will come (fast "camp scene" start = bad roll, restart now)
		if (g_v_time > 0.0)
		{
			const double now = now_seconds();
			const double age = now - g_v_time;
			const float sc = g_h >= 1200.0f ? 4.0f : 3.0f;
			const float red[4] = {1.0f, 0.1f, 0.1f, 1.0f};
			const float grn[4] = {0.2f, 1.0f, 0.2f, 1.0f};
			const float gry[4] = {0.8f, 0.8f, 0.8f, 1.0f};
			// v1.03: race check from what actually happened (replaces the v0.88 box countdown, which was unreliable).
			// Good: Cormack's "allies to camp" signal (T) fires at the moment Will is warped to the van (after the turret).
			// Bad: T fired earlier (Cormack reached his door spot on his own) -> Will is sent back to the camp at the end.
			{
				const double vt = g_will_van_time;
				if (vt > g_bbt_time && now - vt < 10.0)
				{
					const bool t_before = g_t_time > g_bbt_time && g_t_time < vt - 2.0;
					const bool t_with = g_t_time > g_bbt_time && std::fabs(g_t_time - vt) <= 2.0;
					if (t_before)
						draw_text(g_w * 0.5f - 23.0f * 3.0f * sc, g_h * 0.30f, "CORMACK WON - WILL GOES TO CAMP - RESTART", sc, red);
					else if (t_with)
						draw_text(g_w * 0.5f - 14.0f * 3.0f * sc, g_h * 0.30f, "RACE WON - WILL OK SO FAR", sc, grn);
				}
				const double lt = g_will_left_van_time;
				if (lt > g_bbt_time && now - lt < 8.0 && std::fabs(g_t_time - vt) <= 2.0)
					draw_text(g_w * 0.5f - 25.0f * 3.0f * sc, g_h * 0.30f + 28.0f * sc, "WILL LEFT THE VAN - DONT TOUCH COMBAT_06", sc, red);
				const double ct = g_will_cc_time;
				const bool race_won = vt > g_bbt_time && g_t_time > g_bbt_time && std::fabs(g_t_time - vt) <= 2.0;
				if (ct > g_bbt_time && now - ct < 8.0 && race_won)
					draw_text(g_w * 0.5f - 22.0f * 3.0f * sc, g_h * 0.30f + 14.0f * sc, "WILL REACHED HIS CENTRE SPOT - LIKELY CAMP", sc, red);
			}

			if (g_v_time > 0.0 && g_s_time < g_v_time && now - g_v_time < 900.0)
			{
				// v0.87: camp scene stage before the binoculars. Started-but-not-finished = Will gets grabbed.
				if (false)
					draw_text(g_w * 0.5f - 22.0f * 3.0f * sc, g_h * 0.38f, "CAMP SCENE DONE - BINOCULARS OK", sc, grn);
				else if (false)
					draw_text(g_w * 0.5f - 28.0f * 3.0f * sc, g_h * 0.38f, "CAMP SCENE MID-WAY - BINOCULARS NOW = NO WILL", sc, red);
				else if (false)
					draw_text(g_w * 0.5f - 24.0f * 3.0f * sc, g_h * 0.38f, "CAMP SCENE NOT STARTED - BINOCULARS OK", sc, grn);
			}

			if (false && g_s_time > g_v_time && now - g_s_time < 90.0)   // v1.05: old head-start readout retired
			{
				char gb[96];
				if (g_f2_time > g_v_time)
				{
					const double gap = g_f2_time - g_s_time;
					snprintf(gb, sizeof(gb), "WILL HEAD START %.1f S  (NEED 3+)", gap);
					draw_text(g_w * 0.5f - 18.0f * 3.0f * sc, g_h * 0.38f, gb, sc, gap >= 3.0 ? grn : red);
				}
				else
				{
					snprintf(gb, sizeof(gb), "WILL SENT %.1f S AGO - CAMP DIALOGUE NOT YET", now - g_s_time);
					draw_text(g_w * 0.5f - 22.0f * 3.0f * sc, g_h * 0.38f, gb, sc, grn);
				}
			}

			if (g_t_time > g_v_time)
			{
				const double delta = g_t_time - g_v_time;
				if (now - g_t_time < 400.0)
				{
					(void)delta;
					if (!(g_box_time > g_v_time) || g_t_time - g_box_time < 5.0)
						draw_text(g_w * 0.5f - 20.0f * 3.0f * sc, g_h * 0.22f, "BAD ROLL - CAMP SCENE EARLY", sc, red);
					else
						draw_text(g_w * 0.5f - 10.0f * 3.0f * sc, g_h * 0.22f, "GOOD ROLL - WILL OK", sc, grn);
				}
			}
			else if (age < 120.0)
			{
				draw_text(g_w * 0.5f - 15.0f * 3.0f * sc, g_h * 0.22f, "CHECKING WILL ROLL...", sc, gry);
			}
		}

		g_feet = {origin[0], origin[1], origin[2] - (eye_height == 0.0f ? EYE_HEIGHT : 0.0f)};
		{
			const float cs = g_h >= 1200.0f ? 2.0f : 1.0f;
			char coords[96];
			snprintf(coords, sizeof(coords), "X %.0f  Y %.0f  Z %.0f", g_feet.x, g_feet.y, g_feet.z);
			const float cw = static_cast<float>(strlen(coords)) * 6.0f * cs;
			const float cback[4] = {0.0f, 0.0f, 0.0f, 0.6f};
			const float cwhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};
			add_rect(g_w - cw - 18.0f, 4.0f, cw + 8.0f, 7.0f * cs + 8.0f, cback);
			draw_text(g_w - cw - 14.0f, 8.0f, coords, cs, cwhite);
			if (strcmp(g_rec_map, "recovery") == 0)
			{
				const float ex = std::fabs(g_feet.x - END_CUBE_X) - END_CUBE_HX;
				const float ey = std::fabs(g_feet.y - END_CUBE_Y) - END_CUBE_HY;
				const float side = std::sqrt((ex > 0 ? ex * ex : 0.0f) + (ey > 0 ? ey * ey : 0.0f));
				const float below = g_feet.z - (END_CUBE_Z + END_CUBE_HZ);
				char eb[96];
				const bool over = ex <= 0.0f && ey <= 0.0f;
				if (over) snprintf(eb, sizeof(eb), "OVER THE END CUBE - %.0f ABOVE IT", below > 0 ? below : 0.0f);
				else snprintf(eb, sizeof(eb), "END CUBE: %.0f TO THE SIDE, %.0f BELOW", side, below);
				const float gcol[4] = {0.2f, 1.0f, 0.3f, 1.0f};
				const float ccol[4] = {0.0f, 1.0f, 1.0f, 1.0f};
				const float ew = static_cast<float>(strlen(eb)) * 6.0f * cs;
				add_rect(g_w - ew - 18.0f, 8.0f + 2.0f * (7.0f * cs + 10.0f) - 4.0f, ew + 8.0f, 7.0f * cs + 8.0f, cback);
				draw_text(g_w - ew - 14.0f, 8.0f + 2.0f * (7.0f * cs + 10.0f), eb, cs, over ? gcol : ccol);
			}

			if (g_collision_mode == 1 || g_collision_mode == 2)
			{
				static const char* const kRangeNames[4] = {"WALLS: 15 FT", "WALLS: 40 FT", "WALLS: 100 FT", "WALLS: 250 FT"};
				const char* rn = kRangeNames[g_collision_range];
				const float rw = static_cast<float>(strlen(rn)) * 6.0f * cs;
				draw_text(g_w - rw - 14.0f, 8.0f + 7.0f * cs + 10.0f, rn, cs, cwhite);
			}
		}

		if (g_collision_mode == 1 || g_collision_mode == 2)
		{
			draw_collision_brushes({origin[0], origin[1], origin[2] + 30.0f - (eye_height == 0.0f ? EYE_HEIGHT : 0.0f)});
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
				"Num5 ghost replay (ghost.txt + route.txt)", "Num6 pause  Num7/8 speed  Num9 restart", "Num0 whole path  Num+/- jump 10s", "Num/ ghost only (hide triggers)", "Num. ghost follows my game clock", "Delete collision: off / invisible walls / + all solid / old blocks", "PageDown wall distance (15/40/100/250 ft)", "' save collision pieces around me", "PageUp save collision data", "\\ route guide lines (Seoul)", "; speedometer / strafe helper", "End this help (or console: overlay_help)"};
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
				// v1.29: keep a copy per mission (map_ents_0.txt is overwritten by the next mission)
				if (a == 0 && g_rec_map[0]) { char keep[MAX_PATH]; snprintf(keep, sizeof(keep), "%sspeedrun_data\\map_ents_%s.txt", g_dir, g_rec_map); char kd[MAX_PATH]; snprintf(kd, sizeof(kd), "%sspeedrun_data", g_dir); CreateDirectoryA(kd, nullptr); CopyFileA(path, keep, FALSE); }
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

		// v0.90: also save the blocks so a top-down route map can be drawn outside the game
		{
			char name[128];
			snprintf(name, sizeof(name), "collision_%s.txt", g_rec_map[0] ? g_rec_map : "map");
			const char* old_sub = g_subdir;
			g_subdir = "speedrun_data\\";
			char path[MAX_PATH];
			make_path(path, name);
			g_subdir = old_sub;
			FILE* f = nullptr;
			if (fopen_s(&f, path, "w") == 0 && f)
			{
				fprintf(f, "# collision leaf boxes: mid x y z  half x y z  (tool version %s)\n", ADDON_VERSION);
				for (long long i = 0; i < written; ++i)
					fprintf(f, "%.1f %.1f %.1f %.1f %.1f %.1f\n", out[i].mid[0], out[i].mid[1], out[i].mid[2], out[i].half[0], out[i].half[1], out[i].half[2]);
				fclose(f);
				log_line("collision: saved %lld blocks to %s", written, path);
			}
		}
	}


	// v1.17: reads the level's collision brushes (clipMap: +0x10 planes, +0x20 materials, +0x70 sides,
	// +0x90 brush count, +0x98 brushes, +0xA0 bounds, +0xA8 contents) and works out each brush's shape.
	bool material_is_invisible(const char* name)
	{
		char low[96];
		size_t i = 0;
		for (; name[i] && i + 1 < sizeof(low); ++i) low[i] = static_cast<char>(tolower(static_cast<unsigned char>(name[i])));
		low[i] = 0;
		return strstr(low, "clip") || strstr(low, "caulk") || strstr(low, "nodraw") || strstr(low, "mantle") || strstr(low, "skip") ||
			strstr(low, "hint") || strstr(low, "trigger") || strstr(low, "physics") || strstr(low, "invisible") ||
			strstr(low, "vista_treeline") || strstr(low, "cinematic_3d");   // v1.22: background tree cards and the hologram stage block you too
	}

	template <typename T>
	T* copy_array(const uintptr_t address, const size_t count, const size_t stride)
	{
		if (address < 0x10000 || count == 0 || count > 8000000)
		{
			return nullptr;
		}

		const size_t bytes = count * stride;
		unsigned char* out = static_cast<unsigned char*>(malloc(bytes));
		if (!out)
		{
			return nullptr;
		}

		for (size_t done = 0; done < bytes; done += 65536)
		{
			const size_t n = bytes - done < 65536 ? bytes - done : 65536;
			if (!read_bytes(address + done, out + done, n))
			{
				free(out);
				return nullptr;
			}
		}

		return reinterpret_cast<T*>(out);
	}

	bool enum_clipmap_safe();
	void refresh_brushes()
	{
		if (!enum_clipmap_safe())
		{
			log_line("brushes: no level collision data found");
			return;
		}

		static uintptr_t last_header = 0;
		static char last_map[64] = {};
		const uintptr_t header = reinterpret_cast<uintptr_t>(g_found_assets[0]);
		// v1.30: the clip map lives at the same address for every mission, so also check the map name -
		// before this, only the first mission after starting the game got its full collision saved
		if (header == last_header && g_bbrushes && strcmp(last_map, g_rec_map) == 0)
		{
			return;
		}
		strcpy_s(last_map, sizeof(last_map), g_rec_map);

		unsigned int plane_count = 0, material_count = 0, side_count = 0, brush_count = 0;
		unsigned long long planes_p = 0, materials_p = 0, sides_p = 0, brushes_p = 0, bounds_p = 0, contents_p = 0;
		if (!read_bytes(header + 0x10, &plane_count, 4) || !read_bytes(header + 0x18, &planes_p, 8) ||
			!read_bytes(header + 0x20, &material_count, 4) || !read_bytes(header + 0x28, &materials_p, 8) ||
			!read_bytes(header + 0x70, &side_count, 4) || !read_bytes(header + 0x78, &sides_p, 8) ||
			!read_bytes(header + 0x90, &brush_count, 4) || !read_bytes(header + 0x98, &brushes_p, 8) ||
			!read_bytes(header + 0xA0, &bounds_p, 8) || !read_bytes(header + 0xA8, &contents_p, 8))
		{
			log_line("brushes: could not read the collision header");
			return;
		}

		if (brush_count > 400000) brush_count &= 0xFFFF;
		log_line("brushes: %u planes, %u materials, %u sides, %u brushes", plane_count, material_count, side_count, brush_count);
		if (plane_count == 0 || plane_count > 4000000 || material_count == 0 || material_count > 20000 || side_count > 8000000 ||
			brush_count == 0)
		{
			log_line("brushes: numbers look wrong, giving up");
			return;
		}

		float* planes = copy_array<float>(static_cast<uintptr_t>(planes_p), plane_count, 20);
		unsigned char* materials = copy_array<unsigned char>(static_cast<uintptr_t>(materials_p), material_count, 16);
		unsigned char* sides = copy_array<unsigned char>(static_cast<uintptr_t>(sides_p), side_count ? side_count : 1, 8);
		unsigned char* brushes = copy_array<unsigned char>(static_cast<uintptr_t>(brushes_p), brush_count, 48);
		float* bounds = copy_array<float>(static_cast<uintptr_t>(bounds_p), brush_count, 24);
		unsigned int* contents = copy_array<unsigned int>(static_cast<uintptr_t>(contents_p), brush_count, 4);
		bool* invisible = static_cast<bool*>(calloc(material_count, 1));
		char (*mat_names)[64] = static_cast<char (*)[64]>(calloc(material_count, 64));
		if (!planes || !materials || !brushes || !bounds || !contents || !invisible || !mat_names || (side_count && !sides))
		{
			log_line("brushes: could not copy the collision arrays");
			free(planes); free(materials); free(sides); free(brushes); free(bounds); free(contents); free(invisible); free(mat_names);
			return;
		}

		for (unsigned int m = 0; m < material_count; ++m)
		{
			unsigned long long name_p = 0;
			memcpy(&name_p, materials + m * 16, 8);
			if (!safe_copy_string(reinterpret_cast<const char*>(name_p), mat_names[m], 64))
			{
				strcpy_s(mat_names[m], 64, "?");
			}

			unsigned int mc = 0;
			memcpy(&mc, materials + m * 16 + 12, 4);
			// v1.19: also count anything that blocks the player only by "player clip" and is not ordinary solid
			invisible[m] = material_is_invisible(mat_names[m]) || ((mc & 0x10000u) != 0 && (mc & 1u) == 0);
		}

		// v1.18: most of the level's collision is not brushes but triangle groups ("partitions"), found through the
		// box tree at +0x50/+0x58 (leaf = material + partition). Clip-material triangles are invisible walls too.
		unsigned int tree_count = 0, vert_count = 0, tri_count = 0, part_count = 0;
		unsigned long long tree_p = 0, verts_p = 0, idx_p = 0, parts_p = 0;
		read_bytes(header + 0x50, &tree_count, 4); read_bytes(header + 0x58, &tree_p, 8);
		read_bytes(header + 0xB0, &vert_count, 4); read_bytes(header + 0xB8, &verts_p, 8);
		read_bytes(header + 0xC0, &tri_count, 4); read_bytes(header + 0xC8, &idx_p, 8);
		read_bytes(header + 0xE8, &part_count, 4); read_bytes(header + 0xF0, &parts_p, 8);
		log_line("brushes: triangle collision: %u tree boxes, %u corners, %u triangles, %u groups", tree_count, vert_count, tri_count, part_count);
		unsigned char* tree = nullptr; float* tverts = nullptr; unsigned short* tidx = nullptr; unsigned char* parts = nullptr;
		if (tree_count > 0 && tree_count < 4000000 && vert_count > 0 && vert_count < 8000000 && tri_count > 0 && tri_count < 8000000 &&
			part_count > 0 && part_count < 4000000)
		{
			tree = copy_array<unsigned char>(static_cast<uintptr_t>(tree_p), tree_count, 32);
			tverts = copy_array<float>(static_cast<uintptr_t>(verts_p), vert_count, 12);
			tidx = copy_array<unsigned short>(static_cast<uintptr_t>(idx_p), tri_count, 6);
			parts = copy_array<unsigned char>(static_cast<uintptr_t>(parts_p), part_count, 16);
			if (!tree || !tverts || !tidx || !parts)
			{
				log_line("brushes: could not copy the triangle collision");
				free(tree); free(tverts); free(tidx); free(parts);
				tree = nullptr; tverts = nullptr; tidx = nullptr; parts = nullptr;
			}
		}
		const unsigned int tri_room = tree ? tri_count : 0;
		const unsigned int extra = tree ? tree_count : 0;

		const int max_edges = static_cast<int>(brush_count) * 18 + 1024 + static_cast<int>(tri_room) * 3;
		const int max_points = static_cast<int>(brush_count) * 30 + 1024 + static_cast<int>(tri_room) * 3;
		cbrush_draw* out = static_cast<cbrush_draw*>(calloc(brush_count + extra, sizeof(cbrush_draw)));
		vec3* edges = static_cast<vec3*>(malloc(sizeof(vec3) * 2 * static_cast<size_t>(max_edges)));
		const int max_faces = static_cast<int>(brush_count) * 8 + 1024 + static_cast<int>(tri_room);
		cbrush_face* faces = static_cast<cbrush_face*>(malloc(sizeof(cbrush_face) * static_cast<size_t>(max_faces)));
		vec3* points = static_cast<vec3*>(malloc(sizeof(vec3) * static_cast<size_t>(max_points)));
		if (!out || !edges || !faces || !points)
		{
			log_line("brushes: out of memory");
			free(planes); free(materials); free(sides); free(brushes); free(bounds); free(contents); free(invisible); free(mat_names);
			free(out); free(edges); free(faces); free(points);
			return;
		}

		// v1.24: full collision dump for route planning (every solid brush face and every collision triangle, world positions)
		FILE* dump = nullptr;
		{
			char name[128];
			snprintf(name, sizeof(name), "collision_full_%s.bin", g_rec_map[0] ? g_rec_map : "map");
			const char* old_sub = g_subdir;
			g_subdir = "speedrun_data\\";
			char path[MAX_PATH];
			make_path(path, name);
			g_subdir = old_sub;
			if (fopen_s(&dump, path, "wb") != 0) dump = nullptr;
			if (dump)
			{
				fwrite("AWCOL1\0\0", 1, 8, dump);
				fwrite(&material_count, 4, 1, dump);
				for (unsigned int m = 0; m < material_count; ++m)
				{
					unsigned int sf = 0, mc = 0;
					memcpy(&sf, materials + m * 16 + 8, 4);
					memcpy(&mc, materials + m * 16 + 12, 4);
					fwrite(&mc, 4, 1, dump); fwrite(&sf, 4, 1, dump); fwrite(mat_names[m], 1, 64, dump);
				}
			}
		}
		int dump_brushes = 0, dump_tris = 0;

		int edge_total = 0, face_total = 0, point_total = 0;
		int class_count[4] = {};
		int material_use[4096] = {};
		for (unsigned int bi = 0; bi < brush_count; ++bi)
		{
			cbrush_draw& d = out[bi];
			d.model = -1;
			const float* bb = bounds + bi * 6;
			for (int k = 0; k < 3; ++k)
			{
				d.lo[k] = bb[k] - bb[3 + k];
				d.hi[k] = bb[k] + bb[3 + k];
			}

			d.contents = contents[bi];
			const unsigned char* br = brushes + static_cast<size_t>(bi) * 48;
			unsigned short numsides = 0;
			unsigned long long side_ptr = 0;
			short axial[6];
			memcpy(&numsides, br, 2);
			memcpy(&side_ptr, br + 8, 8);
			memcpy(axial, br + 24, 12);
			if (!(bb[3] >= 0.0f && bb[4] >= 0.0f && bb[5] >= 0.0f && bb[3] < 1.0e5f && bb[4] < 1.0e5f && bb[5] < 1.0e5f &&
				std::fabs(bb[0]) < 1.0e6f && std::fabs(bb[1]) < 1.0e6f && std::fabs(bb[2]) < 1.0e6f))
			{
				continue;
			}

			// half-spaces n.x <= dist : 6 axial first, then the extra sides
			float pn[64][3];
			float pd[64];
			int pm[64];
			int np = 0;
			for (int k = 0; k < 3; ++k)
			{
				pn[np][0] = pn[np][1] = pn[np][2] = 0.0f; pn[np][k] = -1.0f; pd[np] = -d.lo[k]; pm[np] = axial[k]; ++np;
				pn[np][0] = pn[np][1] = pn[np][2] = 0.0f; pn[np][k] = 1.0f; pd[np] = d.hi[k]; pm[np] = axial[3 + k]; ++np;
			}

			long long first_side = -1;
			if (numsides > 0 && side_count > 0 && side_ptr >= sides_p)
			{
				first_side = static_cast<long long>((side_ptr - sides_p) / 8);
			}

			for (int s = 0; s < numsides && np < 64 && first_side >= 0 && first_side + s < static_cast<long long>(side_count); ++s)
			{
				unsigned int plane_index = 0;
				unsigned short mat = 0;
				memcpy(&plane_index, sides + (first_side + s) * 8, 4);
				memcpy(&mat, sides + (first_side + s) * 8 + 4, 2);
				if (plane_index >= plane_count)
				{
					continue;
				}

				const float* pl = planes + static_cast<size_t>(plane_index) * 5;
				pn[np][0] = pl[0]; pn[np][1] = pl[1]; pn[np][2] = pl[2]; pd[np] = pl[3]; pm[np] = mat; ++np;
			}

			// class from materials and contents
			bool all_invisible = true, any_mantle = false;
			for (int p = 0; p < np; ++p)
			{
				if (pm[p] < 0 || pm[p] >= static_cast<int>(material_count)) continue;
				if (!invisible[pm[p]]) all_invisible = false;
				char low[64];
				size_t c = 0;
				for (; mat_names[pm[p]][c] && c + 1 < sizeof(low); ++c) low[c] = static_cast<char>(tolower(static_cast<unsigned char>(mat_names[pm[p]][c])));
				low[c] = 0;
				if (strstr(low, "mantle")) any_mantle = true;
			}

			const bool blocks_player = (d.contents & 0x10001u) != 0;
			d.cls = any_mantle ? 2 : (blocks_player ? (all_invisible ? 1 : 3) : 0);
			++class_count[d.cls];
			if (d.cls == 1)
			{
				for (int p = 0; p < np; ++p)
					if (pm[p] >= 0 && pm[p] < 4096) ++material_use[pm[p]];
			}

			if (d.cls == 0)
			{
				continue;
			}

			// corners: every three planes that meet at a point inside all the others
			vec3 verts[256];
			unsigned long long on[256];
			int nv = 0;
			for (int a = 0; a < np && nv < 256; ++a)
			for (int b = a + 1; b < np && nv < 256; ++b)
			for (int c = b + 1; c < np && nv < 256; ++c)
			{
				const float* n1 = pn[a]; const float* n2 = pn[b]; const float* n3 = pn[c];
				const float cx23[3] = {n2[1] * n3[2] - n2[2] * n3[1], n2[2] * n3[0] - n2[0] * n3[2], n2[0] * n3[1] - n2[1] * n3[0]};
				const float det = n1[0] * cx23[0] + n1[1] * cx23[1] + n1[2] * cx23[2];
				if (std::fabs(det) < 1.0e-5f) continue;
				const float cx31[3] = {n3[1] * n1[2] - n3[2] * n1[1], n3[2] * n1[0] - n3[0] * n1[2], n3[0] * n1[1] - n3[1] * n1[0]};
				const float cx12[3] = {n1[1] * n2[2] - n1[2] * n2[1], n1[2] * n2[0] - n1[0] * n2[2], n1[0] * n2[1] - n1[1] * n2[0]};
				vec3 v;
				v.x = (pd[a] * cx23[0] + pd[b] * cx31[0] + pd[c] * cx12[0]) / det;
				v.y = (pd[a] * cx23[1] + pd[b] * cx31[1] + pd[c] * cx12[1]) / det;
				v.z = (pd[a] * cx23[2] + pd[b] * cx31[2] + pd[c] * cx12[2]) / det;
				bool inside = true;
				unsigned long long mask = 0;
				for (int p = 0; p < np; ++p)
				{
					const float s = pn[p][0] * v.x + pn[p][1] * v.y + pn[p][2] * v.z - pd[p];
					if (s > 0.1f) { inside = false; break; }
					if (s > -0.1f) mask |= 1ull << p;
				}

				if (!inside) continue;
				int same = -1;
				for (int q = 0; q < nv; ++q)
				{
					if (std::fabs(verts[q].x - v.x) < 0.2f && std::fabs(verts[q].y - v.y) < 0.2f && std::fabs(verts[q].z - v.z) < 0.2f) { same = q; break; }
				}

				if (same >= 0) { on[same] |= mask; continue; }
				verts[nv] = v;
				on[nv] = mask;
				++nv;
			}

			if (nv < 4)
			{
				d.cls = 0;
				continue;
			}

			// edges: two corners that share two faces
			d.first_edge = edge_total;
			for (int i = 0; i < nv; ++i)
			for (int j = i + 1; j < nv; ++j)
			{
				const unsigned long long shared = on[i] & on[j];
				if ((shared & (shared - 1)) == 0) continue;   // fewer than two
				if (edge_total >= max_edges) break;
				edges[edge_total * 2] = verts[i];
				edges[edge_total * 2 + 1] = verts[j];
				++edge_total;
			}

			d.edge_count = edge_total - d.first_edge;

			if (dump)
			{
				// record: 'B', contents, face count, then per face: material, corner count, corners
				int faces_here = 0;
				for (int p = 0; p < np; ++p)
				{
					int k = 0;
					for (int i = 0; i < nv; ++i) if (on[i] & (1ull << p)) ++k;
					if (k >= 3) ++faces_here;
				}
				const unsigned char tag = 'B';
				const unsigned short nf = static_cast<unsigned short>(faces_here);
				fwrite(&tag, 1, 1, dump); fwrite(&d.contents, 4, 1, dump); fwrite(&nf, 2, 1, dump);
				for (int p = 0; p < np; ++p)
				{
					int idx[64];
					int k = 0;
					float cx = 0, cy = 0, cz = 0;
					for (int i = 0; i < nv && k < 64; ++i)
						if (on[i] & (1ull << p)) { idx[k++] = i; cx += verts[i].x; cy += verts[i].y; cz += verts[i].z; }
					if (k < 3) continue;
					cx /= k; cy /= k; cz /= k;
					const float* n = pn[p];
					float ux, uy, uz;
					if (std::fabs(n[2]) < 0.9f) { ux = -n[1]; uy = n[0]; uz = 0.0f; } else { ux = 0.0f; uy = -n[2]; uz = n[1]; }
					const float vx = n[1] * uz - n[2] * uy, vy = n[2] * ux - n[0] * uz, vz = n[0] * uy - n[1] * ux;
					float ang[64];
					for (int i = 0; i < k; ++i)
					{
						const float dx = verts[idx[i]].x - cx, dy = verts[idx[i]].y - cy, dz = verts[idx[i]].z - cz;
						ang[i] = std::atan2(dx * vx + dy * vy + dz * vz, dx * ux + dy * uy + dz * uz);
					}
					for (int i = 1; i < k; ++i)
						for (int j = i; j > 0 && ang[j - 1] > ang[j]; --j)
						{
							const float ta = ang[j]; ang[j] = ang[j - 1]; ang[j - 1] = ta;
							const int ti = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = ti;
						}
					const unsigned short mat16 = static_cast<unsigned short>(pm[p] < 0 ? 0xFFFF : pm[p]);
					const unsigned char kc = static_cast<unsigned char>(k);
					fwrite(&mat16, 2, 1, dump); fwrite(&kc, 1, 1, dump);
					for (int i = 0; i < k; ++i) fwrite(&verts[idx[i]], 12, 1, dump);
				}
				++dump_brushes;
			}

			// faces (only kept for invisible walls, used for the see-through fill and the saved file)
			d.first_face = face_total;
			if (d.cls == 1)
			{
				for (int p = 0; p < np && face_total < max_faces; ++p)
				{
					int idx[64];
					int k = 0;
					float cx = 0, cy = 0, cz = 0;
					for (int i = 0; i < nv && k < 64; ++i)
						if (on[i] & (1ull << p)) { idx[k++] = i; cx += verts[i].x; cy += verts[i].y; cz += verts[i].z; }
					if (k < 3 || point_total + k > max_points) continue;
					cx /= k; cy /= k; cz /= k;
					// sort the corners around the face centre
					const float* n = pn[p];
					float ux, uy, uz;
					if (std::fabs(n[2]) < 0.9f) { ux = -n[1]; uy = n[0]; uz = 0.0f; } else { ux = 0.0f; uy = -n[2]; uz = n[1]; }
					const float vx = n[1] * uz - n[2] * uy, vy = n[2] * ux - n[0] * uz, vz = n[0] * uy - n[1] * ux;
					float ang[64];
					for (int i = 0; i < k; ++i)
					{
						const float dx = verts[idx[i]].x - cx, dy = verts[idx[i]].y - cy, dz = verts[idx[i]].z - cz;
						ang[i] = std::atan2(dx * vx + dy * vy + dz * vz, dx * ux + dy * uy + dz * uz);
					}

					for (int i = 1; i < k; ++i)
						for (int j = i; j > 0 && ang[j - 1] > ang[j]; --j)
						{
							const float ta = ang[j]; ang[j] = ang[j - 1]; ang[j - 1] = ta;
							const int ti = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = ti;
						}

					faces[face_total].first_point = point_total;
					faces[face_total].point_count = k;
					for (int i = 0; i < k; ++i) points[point_total++] = verts[idx[i]];
					++face_total;
				}
			}

			d.face_count = face_total - d.first_face;
		}

		log_line("brushes: %d invisible walls, %d mantle, %d visible solid, %d skipped (%d edges, %d faces)", class_count[1], class_count[2],
		         class_count[3], class_count[0], edge_total, face_total);

		// triangle groups with a clip material
		unsigned int total = brush_count;
		int tri_walls = 0, tri_drawn = 0, skip_range = 0, skip_verts = 0, verts_in = 0, verts_out = 0, searched = 0, shown_bases = 0;
		long long last_base = -1;
		// v1.23: which tree boxes belong to brush models (cmodels at +0x130 count, +0x138 list, 88 bytes each)
		short* owner = tree ? static_cast<short*>(malloc(sizeof(short) * tree_count)) : nullptr;
		int model_count_found = 0, model_leaves = 0;
		if (owner)
		{
			for (unsigned int q = 0; q < tree_count; ++q) owner[q] = -1;
			unsigned int cm_count = 0;
			unsigned long long cm_p = 0;
			read_bytes(header + 0x130, &cm_count, 4);
			read_bytes(header + 0x138, &cm_p, 8);
			unsigned char* cms = (cm_count > 1 && cm_count < MAX_MODELS) ? copy_array<unsigned char>(static_cast<uintptr_t>(cm_p), cm_count, 88) : nullptr;
			unsigned long long info0 = 0;
			if (cms) memcpy(&info0, cms + 0x20, 8);
			log_line("brushes: %u brush models (list %llX, first info %llX, expected %llX)", cm_count, cm_p, info0, static_cast<unsigned long long>(header + 0x10));
			for (int m = 0; m < MAX_MODELS; ++m) g_model_slot[m] = -1;
			for (unsigned int m = 1; cms && m < cm_count; ++m)
			{
				const unsigned char* cm = cms + static_cast<size_t>(m) * 88;
				float mid[3], half[3];
				memcpy(mid, cm, 12); memcpy(half, cm + 12, 12);
				g_model_local_mid[m][0] = mid[0]; g_model_local_mid[m][1] = mid[1]; g_model_local_mid[m][2] = mid[2];
				// try two layouts for the leaf's first box / box count
				for (int layout = 0; layout < 2; ++layout)
				{
					unsigned int first = 0, cnt = 0;
					if (layout == 0) { memcpy(&first, cm + 0x28, 4); memcpy(&cnt, cm + 0x2C, 4); }
					else { unsigned short a = 0, b2 = 0; memcpy(&a, cm + 0x28, 2); memcpy(&b2, cm + 0x2A, 2); first = a; cnt = b2; }
					if (cnt == 0 || cnt > 4096 || first >= tree_count || first + cnt > tree_count) continue;
					// walk the boxes, collect leaves, check they sit inside the model's own box
					static unsigned int stack[65536];
					int sp = 0, inside = 0, outside = 0;
					for (unsigned int r = 0; r < cnt && sp < 65536; ++r) stack[sp++] = first + r;
					static unsigned int found[65536];
					int nf = 0;
					while (sp > 0 && nf < 65536)
					{
						const unsigned int ni = stack[--sp];
						if (ni >= tree_count) { ++outside; continue; }
						const unsigned char* nd = tree + static_cast<size_t>(ni) * 32;
						unsigned short cc = 0; unsigned int u = 0;
						memcpy(&cc, nd + 14, 2); memcpy(&u, nd + 28, 4);
						if (cc == 0) { found[nf++] = ni; continue; }
						for (unsigned int c = 0; c < cc && sp < 65536; ++c) stack[sp++] = u + c;
					}
					for (int q = 0; q < nf; ++q)
					{
						float nm[3], nh[3];
						memcpy(nm, tree + static_cast<size_t>(found[q]) * 32, 12);
						memcpy(nh, tree + static_cast<size_t>(found[q]) * 32 + 16, 12);
						bool in = true;
						for (int k = 0; k < 3; ++k) if (nm[k] - nh[k] < mid[k] - half[k] - 8.0f || nm[k] + nh[k] > mid[k] + half[k] + 8.0f) in = false;
						if (in) ++inside; else ++outside;
					}
					if (nf == 0 || inside < nf * 8 / 10) continue;
					for (int q = 0; q < nf; ++q) owner[found[q]] = static_cast<short>(m);
					model_leaves += nf;
					++model_count_found;
					break;
				}
			}

			// match each model to its entity: same size, and entity box centre = entity origin + model's own centre
			int matched = 0;
			struct ent_box { int slot; float wb[6]; float o[3]; };
			ent_box* ents = static_cast<ent_box*>(malloc(sizeof(ent_box) * MAX_ENTITIES));
			int ne = 0;
			for (int slot = 0; ents && slot < MAX_ENTITIES; ++slot)
			{
				unsigned char eb[0x1C0];
				if (!read_bytes(SP_G_ENTITIES + static_cast<uintptr_t>(slot) * g_stride, eb, sizeof(eb))) break;
				unsigned short cid = 0;
				memcpy(&cid, eb + OFF_CLASSNAME, 2);
				if (cid == 0) continue;
				ents[ne].slot = slot;
				memcpy(ents[ne].wb, eb + OFF_BOUNDS, 24);
				memcpy(ents[ne].o, eb + OFF_ORIGIN, 12);
				++ne;
			}
			for (unsigned int m = 1; cms && ents && m < cm_count; ++m)
			{
				const unsigned char* cm = cms + static_cast<size_t>(m) * 88;
				float mid[3], half[3];
				memcpy(mid, cm, 12); memcpy(half, cm + 12, 12);
				for (int e = 0; e < ne; ++e)
				{
					const int slot = ents[e].slot;
					const float* wb = ents[e].wb;
					const float* o = ents[e].o;
					bool same = true;
					for (int k = 0; k < 3 && same; ++k)
						if (std::fabs(wb[3 + k] - half[k]) > 2.5f || std::fabs(wb[k] - (o[k] + mid[k])) > 2.5f) same = false;
					if (!same) continue;
					bool taken = false;
					for (unsigned int q = 1; q < m; ++q) if (g_model_slot[q] == slot) taken = true;
					if (taken) continue;
					g_model_slot[m] = slot;
					++matched;
					break;
				}
			}

			log_line("brushes: %d brush models have triangle collision (%d boxes), %d matched to objects in the level", model_count_found, model_leaves, matched);
			free(ents);
			free(cms);
		}

		if (tree)
		{
			unsigned char* used = static_cast<unsigned char*>(calloc(part_count, 1));
			unsigned char* dumped = static_cast<unsigned char*>(calloc(part_count, 1));
			for (unsigned int ti = 0; ti < tree_count && used && dumped; ++ti)
			{
				const unsigned char* node = tree + static_cast<size_t>(ti) * 32;
				unsigned short mat = 0, child_count = 0;
				unsigned int part = 0;
				memcpy(&mat, node + 12, 2);
				memcpy(&child_count, node + 14, 2);
				memcpy(&part, node + 28, 4);
				if (child_count != 0 || part >= part_count || mat >= material_count || used[part]) continue;
				unsigned int mat_contents = 0;
				memcpy(&mat_contents, materials + static_cast<size_t>(mat) * 16 + 12, 4);
				const short mdl = owner ? owner[ti] : static_cast<short>(-1);
				if ((mat_contents & 0x10001u) == 0) continue;
				if (dump && !dumped[part])
				{
					dumped[part] = 1;
					const unsigned char* dp = parts + static_cast<size_t>(part) * 16;
					const int dcount = dp[0];
					int dfirst = 0;
					memcpy(&dfirst, dp + 4, 4);
					float nm1[3], nh1[3];
					memcpy(nm1, node, 12); memcpy(nh1, node + 16, 12);
					const unsigned short* dgi = tidx + static_cast<size_t>(dfirst) * 3;
					long long dbase = -1;
					if (dcount > 0 && dfirst >= 0 && static_cast<unsigned int>(dfirst) + dcount <= tri_count)
					{
						const long long cand[2] = {static_cast<long long>(dp[2]) << 10, static_cast<long long>(dp[2]) << 16};
						for (int ci = 0; ci < 2 && dbase < 0; ++ci)
						{
							bool okb = true;
							for (int q = 0; q < dcount * 3 && okb; ++q)
							{
								const unsigned long long vi = static_cast<unsigned long long>(cand[ci]) + dgi[q];
								if (vi >= vert_count) { okb = false; break; }
								const float* v = tverts + vi * 3;
								if (!(std::fabs(v[0] - nm1[0]) <= nh1[0] + 2.0f && std::fabs(v[1] - nm1[1]) <= nh1[1] + 2.0f && std::fabs(v[2] - nm1[2]) <= nh1[2] + 2.0f)) okb = false;
							}
							if (okb) dbase = cand[ci];
						}
					}
					float sh[3] = {0.0f, 0.0f, 0.0f};
					bool place = true;
					if (mdl >= 0)
					{
						const int slot = g_model_slot[mdl];
						place = slot >= 0 && read_bytes(SP_G_ENTITIES + static_cast<uintptr_t>(slot) * g_stride + OFF_ORIGIN, sh, sizeof(sh));
					}
					if (dbase >= 0 && place)
					{
						for (int t = 0; t < dcount; ++t)
						{
							const unsigned char tag = 'T';
							const short m16 = mdl;
							fwrite(&tag, 1, 1, dump); fwrite(&mat, 2, 1, dump); fwrite(&mat_contents, 4, 1, dump); fwrite(&m16, 2, 1, dump);
							for (int k = 0; k < 3; ++k)
							{
								const float* v = tverts + (static_cast<unsigned long long>(dbase) + dgi[t * 3 + k]) * 3;
								const float w[3] = {v[0] + sh[0], v[1] + sh[1], v[2] + sh[2]};
								fwrite(w, 12, 1, dump);
							}
							++dump_tris;
						}
					}
				}
				if (mdl < 0 && (!invisible[mat] || strstr(mat_names[mat], "caulk"))) continue;   // level: only invisible walls; brush models: everything solid
				used[part] = 1;
				const unsigned char* pp = parts + static_cast<size_t>(part) * 16;
				const int count = pp[0];
				const unsigned int seg = pp[2];
				int first = 0;
				memcpy(&first, pp + 4, 4);
				if (count <= 0 || first < 0 || static_cast<unsigned int>(first) + count > tri_count) { ++skip_range; continue; }
				if (face_total + count > max_faces || point_total + count * 3 > max_points || edge_total + count * 3 > max_edges) break;

				cbrush_draw& d = out[total];
				d.cls = 1;
				d.model = mdl;
				d.contents = mat_contents;
				d.lo[0] = d.lo[1] = d.lo[2] = 1.0e30f;
				d.hi[0] = d.hi[1] = d.hi[2] = -1.0e30f;
				d.first_face = face_total;
				d.first_edge = edge_total;
				vec3 tri_pts[256 * 3];
				int good = 0;
				// v1.21: where this group's corners start. The 3rd header byte is not always right, so check the corners
				// land inside the group's box, and if not, search for the starting point that makes them fit.
				float nm0[3], nh0[3];
				memcpy(nm0, node, 12); memcpy(nh0, node + 16, 12);
				const unsigned short* gi = tidx + static_cast<size_t>(first) * 3;
				unsigned int max_i = 0;
				for (int q = 0; q < count * 3; ++q) if (gi[q] > max_i) max_i = gi[q];
				auto fits = [&](unsigned long long b) -> bool {
					if (b + max_i >= vert_count) return false;
					for (int q = 0; q < count * 3; ++q)
					{
						const float* v = tverts + (b + gi[q]) * 3;
						if (!(std::fabs(v[0] - nm0[0]) <= nh0[0] + 2.0f && std::fabs(v[1] - nm0[1]) <= nh0[1] + 2.0f && std::fabs(v[2] - nm0[2]) <= nh0[2] + 2.0f)) return false;
					}
					return true;
				};
				long long base = -1;
				if (fits(static_cast<unsigned long long>(seg) << 10)) base = static_cast<long long>(seg) << 10;   // v1.22: corners start at byte*1024
				else if (fits(static_cast<unsigned long long>(seg) << 16)) base = static_cast<long long>(seg) << 16;
				else if (last_base >= 0 && fits(static_cast<unsigned long long>(last_base))) base = last_base;
				else
				{
					const unsigned int first_i = gi[0];
					for (unsigned long long b = 0; b + max_i < vert_count; ++b)
					{
						const float* v = tverts + (b + first_i) * 3;
						if (std::fabs(v[0] - nm0[0]) > nh0[0] + 2.0f || std::fabs(v[1] - nm0[1]) > nh0[1] + 2.0f || std::fabs(v[2] - nm0[2]) > nh0[2] + 2.0f) continue;
						if (fits(b)) { base = static_cast<long long>(b); ++searched; break; }
					}
				}

				if (base >= 0) last_base = base;
				if (base >= 0 && base != (static_cast<long long>(seg) << 10) && shown_bases < 12)
				{
					++shown_bases;
					log_line("brushes: group %u header %02X %02X %02X %02X first %d -> corners start at %lld (seg byte %u)", part, pp[0], pp[1], pp[2], pp[3], first, base, seg);
				}

				for (int t = 0; t < count && base >= 0; ++t)
				{
					bool ok_tri = true;
					for (int k = 0; k < 3; ++k)
					{
						const unsigned int vi = static_cast<unsigned int>(base) + tidx[(static_cast<size_t>(first) + t) * 3 + k];
						if (vi >= vert_count) { ok_tri = false; break; }
						const float* v = tverts + static_cast<size_t>(vi) * 3;
						tri_pts[good * 3 + k] = {v[0], v[1], v[2]};
						float nm[3], nh[3];
						memcpy(nm, node, 12); memcpy(nh, node + 16, 12);
						if (std::fabs(v[0] - nm[0]) <= nh[0] + 2.0f && std::fabs(v[1] - nm[1]) <= nh[1] + 2.0f && std::fabs(v[2] - nm[2]) <= nh[2] + 2.0f) ++verts_in; else ++verts_out;
					}

					if (!ok_tri) continue;
					faces[face_total].first_point = point_total;
					faces[face_total].point_count = 3;
					for (int k = 0; k < 3; ++k)
					{
						const vec3& v = tri_pts[good * 3 + k];
						points[point_total++] = v;
						const float c[3] = {v.x, v.y, v.z};
						for (int a = 0; a < 3; ++a) { if (c[a] < d.lo[a]) d.lo[a] = c[a]; if (c[a] > d.hi[a]) d.hi[a] = c[a]; }
					}

					++face_total;
					++good;
				}

				// outline: triangle sides that no other triangle in the group shares
				for (int t = 0; t < good; ++t)
					for (int k = 0; k < 3; ++k)
					{
						const vec3& a = tri_pts[t * 3 + k];
						const vec3& b = tri_pts[t * 3 + (k + 1) % 3];
						bool shared = false;
						for (int u = 0; u < good && !shared; ++u)
						{
							if (u == t) continue;
							for (int m = 0; m < 3; ++m)
							{
								const vec3& c = tri_pts[u * 3 + m];
								const vec3& e = tri_pts[u * 3 + (m + 1) % 3];
								if ((std::fabs(c.x - b.x) + std::fabs(c.y - b.y) + std::fabs(c.z - b.z) < 0.5f &&
									 std::fabs(e.x - a.x) + std::fabs(e.y - a.y) + std::fabs(e.z - a.z) < 0.5f) ||
									(std::fabs(c.x - a.x) + std::fabs(c.y - a.y) + std::fabs(c.z - a.z) < 0.5f &&
									 std::fabs(e.x - b.x) + std::fabs(e.y - b.y) + std::fabs(e.z - b.z) < 0.5f))
								{
									shared = true;
									break;
								}
							}
						}

						if (!shared && edge_total < max_edges)
						{
							edges[edge_total * 2] = a;
							edges[edge_total * 2 + 1] = b;
							++edge_total;
						}
					}

				d.face_count = face_total - d.first_face;
				d.edge_count = edge_total - d.first_edge;
				if (good == 0) { d.cls = 0; ++skip_verts; continue; }
				if (mat < 4096) ++material_use[mat];
				tri_drawn += good;
				++tri_walls;
				++total;
			}

			free(used);
			free(dumped);
			free(tree); free(tverts); free(tidx); free(parts);
			free(owner);
		}

		if (dump)
		{
			const unsigned char tag = 'E';
			fwrite(&tag, 1, 1, dump);
			fclose(dump);
			log_line("brushes: full collision saved for route planning (%d brushes, %d triangles)", dump_brushes, dump_tris);
			// movement settings that matter for route planning
			char name[128];
			snprintf(name, sizeof(name), "movement_%s.txt", g_rec_map[0] ? g_rec_map : "map");
			const char* old_sub = g_subdir;
			g_subdir = "speedrun_data\\";
			char path[MAX_PATH];
			make_path(path, name);
			g_subdir = old_sub;
			FILE* mf = nullptr;
			if (fopen_s(&mf, path, "w") == 0 && mf)
			{
				static const char* const kNames[] = {"g_speed", "g_gravity", "jump_height", "jump_stepSize", "jump_slowdownEnable", "jump_ladderPushVel",
					"player_sprintSpeedScale", "player_sprintUnlimited", "player_sprintTime", "player_backSpeedScale", "player_strafeSpeedScale",
					"bg_fallDamageMinHeight", "bg_fallDamageMaxHeight", "player_view_pitch_up", "player_view_pitch_down", "g_friction", "g_stopspeed",
					"player_moveThreshhold", "jump_spreadAdd", "player_meleeRange", "mantle_enable", "mantle_check_range", "mantle_check_radius",
					"mantle_check_angle", "mantle_view_yawcap", "player_standingViewHeight", "player_crouchViewHeight", "player_proneViewHeight",
					"g_maxHeightForUpwardJump", "player_slideTime", "player_sustainAmmo", "bg_ladder_yawcap"};
				for (const char* nm : kNames)
				{
					float v = 0.0f;
					if (read_dvar_float(nm, &v))
					{
						int iv = 0; memcpy(&iv, &v, 4);
						fprintf(mf, "%s float %g int %d\n", nm, v, iv);
					}
					else fprintf(mf, "%s missing\n", nm);
				}
				fclose(mf);
			}
		}

		log_line("brushes: %d invisible triangle groups (%d triangles); skipped %d (range) %d (corners); corners inside their box %d, outside %d; %d found by search",
		         tri_walls, tri_drawn, skip_range, skip_verts, verts_in, verts_out, searched);

		// save for the 3D map: invisible walls and mantle brushes as faces / edges
		{
			char name[128];
			snprintf(name, sizeof(name), "brushes_%s.txt", g_rec_map[0] ? g_rec_map : "map");
			const char* old_sub = g_subdir;
			g_subdir = "speedrun_data\\";
			char path[MAX_PATH];
			make_path(path, name);
			g_subdir = old_sub;
			FILE* f = nullptr;
			if (fopen_s(&f, path, "w") == 0 && f)
			{
				fprintf(f, "# collision brushes (tool version %s). B class contents lo hi; F x y z ... (face corners); E x y z x y z\n", ADDON_VERSION);
				fprintf(f, "# class 1 = invisible wall, 2 = mantle, 3 = visible solid, 4 = brush model (object) collision, world position at save time\n");
				for (unsigned int m = 0; m < material_count && m < 4096; ++m)
					if (material_use[m] > 0) fprintf(f, "# wall material %u %s used %d\n", m, mat_names[m], material_use[m]);
				for (unsigned int m = 0; m < material_count; ++m)
				{
					unsigned int sf = 0, mc = 0;
					memcpy(&sf, materials + m * 16 + 8, 4);
					memcpy(&mc, materials + m * 16 + 12, 4);
					fprintf(f, "# material %u %s flags %08X contents %08X%s\n", m, mat_names[m], sf, mc, invisible[m] ? " INVISIBLE" : "");
				}
				for (unsigned int bi = 0; bi < total; ++bi)
				{
					const cbrush_draw& d = out[bi];
					if (d.cls == 0) continue;
					float sh[3] = {0.0f, 0.0f, 0.0f};
					if (d.model >= 0)
					{
						const int slot = g_model_slot[d.model];
						if (slot < 0 || !read_bytes(SP_G_ENTITIES + static_cast<uintptr_t>(slot) * g_stride + OFF_ORIGIN, sh, sizeof(sh))) continue;
					}
					fprintf(f, "B %d %08X %.1f %.1f %.1f %.1f %.1f %.1f%s\n", d.model >= 0 ? 4 : d.cls, d.contents, d.lo[0] + sh[0], d.lo[1] + sh[1], d.lo[2] + sh[2],
					        d.hi[0] + sh[0], d.hi[1] + sh[1], d.hi[2] + sh[2], d.model >= 0 ? " model" : "");
					if (d.cls == 1)
					{
						for (int fi = 0; fi < d.face_count; ++fi)
						{
							const cbrush_face& face = faces[d.first_face + fi];
							fprintf(f, "F");
							for (int i = 0; i < face.point_count; ++i)
								fprintf(f, " %.1f %.1f %.1f", points[face.first_point + i].x + sh[0], points[face.first_point + i].y + sh[1], points[face.first_point + i].z + sh[2]);
							fprintf(f, "\n");
						}
					}
					else if (d.cls == 2)
					{
						for (int e = 0; e < d.edge_count; ++e)
						{
							const vec3& a = edges[(d.first_edge + e) * 2];
							const vec3& b = edges[(d.first_edge + e) * 2 + 1];
							fprintf(f, "E %.1f %.1f %.1f %.1f %.1f %.1f\n", a.x, a.y, a.z, b.x, b.y, b.z);
						}
					}
				}

				fclose(f);
				log_line("brushes: saved to %s", path);
			}
		}

		free(planes); free(materials); free(sides); free(brushes); free(bounds); free(contents); free(invisible); free(mat_names);
		// publish (old arrays are not freed - the picture thread may still be reading them)
		g_bbrush_count = 0;
		g_bedges = edges;
		g_bfaces = faces;
		g_bpoints = points;
		g_bbrushes = out;
		g_bbrush_count = static_cast<int>(total);
		last_header = header;
	}


	// v1.19: ' key - writes every collision piece (brushes and triangle groups, any material) near the player
	unsigned int g_dbg_tri_count = 0, g_dbg_vert_count = 0;
	unsigned long long g_dbg_idx_p = 0, g_dbg_verts_p = 0;

	bool enum_clipmap_safe()
	{
		g_found_count = 0;
		using enum_t = void (*)(int, void (*)(void*, void*), void*, bool);
		__try
		{
			reinterpret_cast<enum_t>(SP_DB_ENUM_XASSETS)(ASSET_TYPE_CLIPMAP_INDEX, &found_asset_callback, nullptr, false);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}

		return g_found_count > 0;
	}

	void dump_collision_here()
	{
		const vec3 at = g_feet;
		const float R = 400.0f;
		if (!enum_clipmap_safe()) return;
		const uintptr_t header = reinterpret_cast<uintptr_t>(g_found_assets[0]);
		unsigned int material_count = 0, side_count = 0, brush_count = 0, tree_count = 0, part_count = 0;
		unsigned long long materials_p = 0, sides_p = 0, brushes_p = 0, bounds_p = 0, contents_p = 0, tree_p = 0, parts_p = 0;
		read_bytes(header + 0x20, &material_count, 4); read_bytes(header + 0x28, &materials_p, 8);
		read_bytes(header + 0x70, &side_count, 4); read_bytes(header + 0x78, &sides_p, 8);
		read_bytes(header + 0x90, &brush_count, 4); read_bytes(header + 0x98, &brushes_p, 8);
		read_bytes(header + 0xA0, &bounds_p, 8); read_bytes(header + 0xA8, &contents_p, 8);
		read_bytes(header + 0x50, &tree_count, 4); read_bytes(header + 0x58, &tree_p, 8);
		read_bytes(header + 0xE8, &part_count, 4); read_bytes(header + 0xF0, &parts_p, 8);
		read_bytes(header + 0xB0, &g_dbg_vert_count, 4); read_bytes(header + 0xB8, &g_dbg_verts_p, 8);
		read_bytes(header + 0xC0, &g_dbg_tri_count, 4); read_bytes(header + 0xC8, &g_dbg_idx_p, 8);
		if (brush_count > 400000) brush_count &= 0xFFFF;
		if (material_count == 0 || material_count > 20000) return;

		char path[MAX_PATH];
		const char* old_sub = g_subdir;
		g_subdir = "speedrun_data\\";
		make_path(path, "collision_here.txt");
		g_subdir = old_sub;
		FILE* f = nullptr;
		if (fopen_s(&f, path, "a") != 0 || !f) return;
		fprintf(f, "\n=== HERE map %s  X %.0f Y %.0f Z %.0f (tool %s) ===\n", g_rec_map[0] ? g_rec_map : "?", at.x, at.y, at.z, ADDON_VERSION);

		auto mat_name = [&](unsigned int m, char* out, unsigned int* contents_out) {
			strcpy_s(out, 64, "?");
			*contents_out = 0;
			if (m >= material_count) return;
			unsigned long long np = 0;
			read_bytes(static_cast<uintptr_t>(materials_p) + m * 16ull, &np, 8);
			read_bytes(static_cast<uintptr_t>(materials_p) + m * 16ull + 12, contents_out, 4);
			safe_copy_string(reinterpret_cast<const char*>(np), out, 64);
		};

		int nb = 0;
		float* bounds = copy_array<float>(static_cast<uintptr_t>(bounds_p), brush_count ? brush_count : 1, 24);
		for (unsigned int bi = 0; bounds && bi < brush_count; ++bi)
		{
			const float* bb = bounds + bi * 6;
			const float dx = std::fabs(at.x - bb[0]) - bb[3], dy = std::fabs(at.y - bb[1]) - bb[4], dz = std::fabs(at.z - bb[2]) - bb[5];
			if (dx > R || dy > R || dz > R) continue;
			unsigned int contents = 0;
			read_bytes(static_cast<uintptr_t>(contents_p) + bi * 4ull, &contents, 4);
			unsigned char br[48];
			if (!read_bytes(static_cast<uintptr_t>(brushes_p) + bi * 48ull, br, 48)) continue;
			unsigned short numsides = 0; unsigned long long side_ptr = 0; short axial[6];
			memcpy(&numsides, br, 2); memcpy(&side_ptr, br + 8, 8); memcpy(axial, br + 24, 12);
			fprintf(f, "BRUSH %u contents %08X  lo %.0f %.0f %.0f  hi %.0f %.0f %.0f  materials:", bi, contents, bb[0] - bb[3], bb[1] - bb[4],
			        bb[2] - bb[5], bb[0] + bb[3], bb[1] + bb[4], bb[2] + bb[5]);
			unsigned int seen[70]; int ns = 0;
			for (int s = 0; s < 6 + numsides && s < 70; ++s)
			{
				unsigned int m = 0;
				if (s < 6) m = static_cast<unsigned short>(axial[s]);
				else
				{
					unsigned short mm = 0;
					read_bytes(static_cast<uintptr_t>(side_ptr) + (s - 6) * 8ull + 4, &mm, 2);
					m = mm;
				}
				bool dup = false;
				for (int q = 0; q < ns; ++q) if (seen[q] == m) dup = true;
				if (dup) continue;
				seen[ns++] = m;
				char name[64]; unsigned int mc = 0;
				mat_name(m, name, &mc);
				fprintf(f, " %s", name);
			}
			fprintf(f, "\n");
			++nb;
		}
		free(bounds);

		int nt = 0;
		unsigned char* tree = copy_array<unsigned char>(static_cast<uintptr_t>(tree_p), tree_count ? tree_count : 1, 32);
		for (unsigned int ti = 0; tree && ti < tree_count; ++ti)
		{
			const unsigned char* node = tree + static_cast<size_t>(ti) * 32;
			float mid[3], half[3];
			unsigned short mat = 0, child_count = 0;
			unsigned int part = 0;
			memcpy(mid, node, 12); memcpy(&mat, node + 12, 2); memcpy(&child_count, node + 14, 2); memcpy(half, node + 16, 12); memcpy(&part, node + 28, 4);
			if (child_count != 0) continue;
			static unsigned int last_parts[64]; static int lp = 0;
			bool again = false;
			for (int q = 0; q < 64; ++q) if (last_parts[q] == part + 1) again = true;
			if (again) continue;
			if (std::fabs(at.x - mid[0]) - half[0] > R || std::fabs(at.y - mid[1]) - half[1] > R || std::fabs(at.z - mid[2]) - half[2] > R) continue;
			unsigned char tris = 0;
			if (part < part_count) read_bytes(static_cast<uintptr_t>(parts_p) + part * 16ull, &tris, 1);
			last_parts[lp] = part + 1; lp = (lp + 1) % 64;
			char name[64]; unsigned int mc = 0;
			mat_name(mat, name, &mc);
			fprintf(f, "TRIS group %u  %d triangles  material %s contents %08X  lo %.0f %.0f %.0f  hi %.0f %.0f %.0f\n", part, tris, name, mc,
			        mid[0] - half[0], mid[1] - half[1], mid[2] - half[2], mid[0] + half[0], mid[1] + half[1], mid[2] + half[2]);
			// v1.20 diagnostics: raw group header and its first triangles, for close-by groups
			if (part < part_count && std::fabs(at.x - mid[0]) - half[0] < 80.0f && std::fabs(at.y - mid[1]) - half[1] < 80.0f &&
				std::fabs(at.z - mid[2]) - half[2] < 80.0f && !(mc & 0x800u))
			{
				unsigned char pr[16] = {};
				read_bytes(static_cast<uintptr_t>(parts_p) + part * 16ull, pr, 16);
				fprintf(f, "   raw:");
				for (int i = 0; i < 16; ++i) fprintf(f, " %02X", pr[i]);
				int first = 0; memcpy(&first, pr + 4, 4);
				fprintf(f, "  (tri count %u, vert count %u, index ptr %llX, vert ptr %llX)\n", g_dbg_tri_count, g_dbg_vert_count, g_dbg_idx_p, g_dbg_verts_p);
				for (int t = 0; t < tris && t < 3; ++t)
				{
					unsigned short id[3] = {};
					read_bytes(static_cast<uintptr_t>(g_dbg_idx_p) + (static_cast<unsigned long long>(first) + t) * 6ull, id, 6);
					fprintf(f, "   tri %d idx %u %u %u :", t, id[0], id[1], id[2]);
					for (int k = 0; k < 3; ++k)
					{
						for (unsigned int sg = 0; sg < 4; ++sg)
						{
							float v[3] = {};
							const unsigned long long vi = sg * 65536ull + id[k];
							if (vi >= g_dbg_vert_count) break;
							read_bytes(static_cast<uintptr_t>(g_dbg_verts_p) + vi * 12ull, v, 12);
							fprintf(f, " [s%u %.0f %.0f %.0f]", sg, v[0], v[1], v[2]);
						}
						fprintf(f, " |");
					}
					fprintf(f, "\n");
				}
			}
			++nt;
		}
		free(tree);
		fclose(f);
		log_line("': wrote %d brushes and %d triangle groups near you to collision_here.txt", nb, nt);
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


	// Finds the game's list of console commands (our own command was just put at the head of it, so the
	// pointer to it is the list head) and writes every command name to console_commands.txt.
	void dump_console_commands()
	{
		const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
		unsigned char header[0x400];
		if (!read_bytes(base, header, sizeof(header)))
		{
			return;
		}

		unsigned int nt = 0, image_size = 0;
		memcpy(&nt, header + 0x3C, 4);
		if (nt == 0 || nt > 0x300)
		{
			return;
		}

		memcpy(&image_size, header + nt + 24 + 56, 4);
		const uintptr_t mine = reinterpret_cast<uintptr_t>(&g_help_command);
		uintptr_t head_address = 0;
		static unsigned char chunk[0x10000];
		for (uintptr_t offset = 0; offset < image_size && !head_address; offset += sizeof(chunk))
		{
			if (!read_bytes(base + offset, chunk, sizeof(chunk)))
			{
				continue;
			}

			for (size_t i = 0; i + 8 <= sizeof(chunk); i += 8)
			{
				uintptr_t value;
				memcpy(&value, chunk + i, 8);
				if (value == mine)
				{
					head_address = base + offset + i;
					break;
				}
			}
		}

		char path[MAX_PATH];
		make_path(path, "console_commands.txt");
		FILE* f = nullptr;
		if (fopen_s(&f, path, "w") != 0 || !f)
		{
			return;
		}

		fprintf(f, "list head variable at %p (module base %p)\n", reinterpret_cast<void*>(head_address), reinterpret_cast<void*>(base));
		// our node's own fields show where the game keeps the name and the function
		{
			unsigned char* own = reinterpret_cast<unsigned char*>(&g_help_command);
			fprintf(f, "our node as the game filled it:");
			for (int i = 0; i < 8; ++i)
			{
				uintptr_t v;
				memcpy(&v, own + i * 8, 8);
				fprintf(f, " %p", reinterpret_cast<void*>(v));
			}
			fprintf(f, "\n");
		}

		int shown = 0;
		if (head_address)
		{
			uintptr_t node = 0;
			read_bytes(head_address, &node, 8);
			while (node && shown < 3000)
			{
				unsigned char data[0x40];
				if (!read_bytes(node, data, sizeof(data)))
				{
					break;
				}

				uintptr_t q[8];
				memcpy(q, data, sizeof(q));
				char name[96] = {};
				for (int field = 1; field < 4; ++field)
				{
					char tmp[96] = {};
					if (q[field] > 0x10000 && read_bytes(q[field], tmp, 64) && tmp[0] >= 'A' && tmp[0] <= 'z')
					{
						tmp[63] = 0;
						strcpy_s(name, sizeof(name), tmp);
						break;
					}
				}

				fprintf(f, "%s | node %p fn %p %p %p\n", name, reinterpret_cast<void*>(node), reinterpret_cast<void*>(q[2]),
				        reinterpret_cast<void*>(q[3]), reinterpret_cast<void*>(q[4]));
				++shown;
				node = q[0];
			}
		}

		fprintf(f, "%d commands\n", shown);
		fclose(f);
		log_line("console commands: %d written to console_commands.txt", shown);

		// copy the game's unpacked code and read-only data to a file so its save functions can be studied
		{
			char dump_path[MAX_PATH], info_path[MAX_PATH];
			make_path(dump_path, "module_dump.bin");
			make_path(info_path, "module_sections.txt");
			FILE* probe = nullptr;
			if (fopen_s(&probe, dump_path, "rb") == 0 && probe)
			{
				fclose(probe);   // already done once
			}
			else
			{
				unsigned short section_count = 0, optional_size = 0;
				memcpy(&section_count, header + nt + 6, 2);
				memcpy(&optional_size, header + nt + 20, 2);
				FILE* dump = nullptr;
				FILE* info = nullptr;
				if (section_count > 0 && section_count < 40 && fopen_s(&dump, dump_path, "wb") == 0 && dump &&
				    fopen_s(&info, info_path, "w") == 0 && info)
				{
					unsigned long long file_offset = 0;
					for (int i = 0; i < section_count; ++i)
					{
						const unsigned char* sec = header + nt + 24 + optional_size + i * 40;
						char name[9] = {};
						memcpy(name, sec, 8);
						unsigned int vsize = 0, vaddr = 0;
						memcpy(&vsize, sec + 8, 4);
						memcpy(&vaddr, sec + 12, 4);
						const bool wanted = strcmp(name, ".text") == 0 || strcmp(name, ".rdata") == 0;
						fprintf(info, "%s virtual_address 0x%X size 0x%X file_offset %llu %s\n", name, vaddr, vsize, file_offset, wanted ? "DUMPED" : "skipped");
						if (!wanted)
						{
							continue;
						}

						static unsigned char buf[0x10000];
						for (unsigned int off = 0; off < vsize; off += sizeof(buf))
						{
							const unsigned int n = vsize - off < sizeof(buf) ? vsize - off : static_cast<unsigned int>(sizeof(buf));
							memset(buf, 0, n);
							read_bytes(base + vaddr + off, buf, n);
							fwrite(buf, 1, n, dump);
							file_offset += n;
						}
					}
				}

				if (dump) fclose(dump);
				if (info) fclose(info);
				log_line("module dump written");
			}
		}
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
		if (ok)
		{
			dump_console_commands();
		}
	}

	bool g_notify_hooked = false;

	// Tries to hook the script "notify" function. Can be called again and again until it works.
	// MinHook needs a free spot within 1 GB of the function it patches. Once the game is running those
	// spots can all be taken, so we reserve a few early and give them back right before hooking.
	void* g_reserved[64];
	int g_reserved_count = 0;

	void reserve_hook_memory()
	{
		// below the game program, then above it, so MinHook always has somewhere to put its code
		for (uintptr_t address = 0x13F000000ull; address > 0x110000000ull && g_reserved_count < 32; address -= 0x400000ull)
		{
			void* block = VirtualAlloc(reinterpret_cast<void*>(address), 0x10000, MEM_RESERVE, PAGE_NOACCESS);
			if (block)
			{
				g_reserved[g_reserved_count++] = block;
			}
		}

		for (uintptr_t address = 0x14C000000ull; address < 0x170000000ull && g_reserved_count < 64; address += 0x400000ull)
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

		if (!g_is_mp)
		{
			static const unsigned char vm_sig[1] = {0};
			(void)vm_sig;
			void* vm = reinterpret_cast<void*>(SP_VM_NOTIFY);
			unsigned char vp[8];
			if (read_bytes(SP_VM_NOTIFY, vp, sizeof(vp)))
			{
				MH_STATUS c2 = MH_CreateHook(vm, reinterpret_cast<LPVOID>(&hooked_vm_notify), reinterpret_cast<LPVOID*>(&g_original_vm_notify));
				if (c2 == MH_OK && MH_EnableHook(vm) == MH_OK)
				{
					log_line("hook: script-made events (flags) installed");
				}
				else
				{
					log_line("hook: script-made events not installed (%s)", MH_StatusToString(c2));
				}
			}
		}
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

	// v0.88: track the real (moving) Cormack and how far he is from his door spot
	void corm_track(const int slot, const char* name, const float* pos)
	{
		if (!strstr(name, "cormack_marine") || strstr(name, "damaged") || strstr(name, "seointro") || strstr(name, "outro")) return;
		if (slot < 0 || slot >= MAX_ENTITIES) return;
		static float last_x[MAX_ENTITIES], last_y[MAX_ENTITIES];
		static double last_t[MAX_ENTITIES], moved_t[MAX_ENTITIES];
		static int best = -1;
		const double now = now_seconds();
		const float dx = pos[0] - last_x[slot], dy = pos[1] - last_y[slot];
		const double dt = now - last_t[slot];
		if (dt < 0.2 && last_t[slot] > 0.0) return;
		const double moved = sqrt(dx * dx + dy * dy);
		if (last_t[slot] > 0.0 && moved > 2.0) moved_t[slot] = now;
		if (best < 0 || best >= MAX_ENTITIES || now - moved_t[best] > 3.0 || moved_t[slot] >= moved_t[best]) best = slot;
		if (best == slot)
		{
			const double ddx = pos[0] - 34529.1, ddy = pos[1] - 8378.7;
			g_corm_dist = sqrt(ddx * ddx + ddy * ddy);
			if (last_t[slot] > 0.0 && dt < 2.0 && moved < 1000.0)
			{
				const double v = moved / dt;
				g_corm_speed = g_corm_speed * 0.6 + v * 0.4;
			}
			g_corm_seen = now;
		}
		last_x[slot] = pos[0]; last_y[slot] = pos[1]; last_t[slot] = now;
	}

	void rec_snotify(const char* name, const unsigned int owner)
	{
		if (g_v_time > 0.0 && g_l_time < g_v_time && strcmp(name, "i_have_landed") == 0)
		{
			const double dt = now_seconds() - g_v_time;
			if (dt >= 10.0 && dt <= 30.0) g_l_time = now_seconds();
		}

		if (g_v_time > 0.0 && g_t_time < g_v_time && strcmp(name, "trigger_allies_to_fob") == 0)
		{
			g_t_time = now_seconds();
		}

		if (g_v_time > 0.0 && strcmp(name, "canal_strategy_scene_complete") == 0)
		{
			g_s_time = now_seconds();
			if (g_rec) rec_line("WILLGAP S (Will sent) at %.2f  F2=%.2f", g_s_time, g_f2_time);
		}
		if (strcmp(name, "begin_building_traversal") == 0)
		{
			g_bbt_time = now_seconds(); g_enter2_time = 0.0; g_enter1_time = 0.0; g_enter_count = 0; g_will_msg = 0; g_will_msg_time = 0.0;
		}
		if (strcmp(name, "first_landassist_enter") == 0 && g_bbt_time > 0.0)
		{
			const double t = now_seconds();
			++g_enter_count;
			if (g_enter_count == 1) g_enter1_time = t;
			// v1.12: this notify comes from Cormack AND Will, in either order, so it no longer drives Will's messages (log only)
			if (g_rec) rec_line("LEDGEANIM ledge enter #%d at %.1f s", g_enter_count, t - g_bbt_time);
		}
		if (strcmp(name, "fob_cormack_wave") == 0) { g_wave_time = now_seconds(); if (g_rec) rec_line("PLANTWIN wave at %.2f (S+%.2f)", g_wave_time, g_wave_time - g_s_time); }
		if (strcmp(name, "canal_bomb_plant_trigger_on") == 0) { g_prompt_time = now_seconds(); if (g_rec) rec_line("PLANTWIN prompt at S+%.2f", g_prompt_time - g_s_time); }
		if (strcmp(name, "bomb_plant_start") == 0) { g_press_time = now_seconds(); if (g_rec) rec_line("PLANTWIN pressed at S+%.2f (wave %s)", g_press_time - g_s_time, g_wave_time > g_s_time && g_s_time > 0.0 ? "already happened" : "not yet"); }
		if (strcmp(name, "begin_building_traversal") == 0) { g_wave_time = 0.0; g_press_time = 0.0; g_prompt_time = 0.0; g_will_van_time = 0.0; g_will_cc_time = 0.0; g_will_camp_time = 0.0; g_will_left_van_time = 0.0; }
		if (g_v_time > 0.0 && strcmp(name, "fob_talk_scene_start") == 0) g_f1_time = now_seconds();
		if (g_v_time > 0.0 && strcmp(name, "fob_talk_scene_end") == 0) g_fe_time = now_seconds();
		if (g_v_time > 0.0 && strcmp(name, "dialogue_start_fob_meetup") == 0)
		{
			g_f2_time = now_seconds();
			if (g_rec) rec_line("WILLGAP F2 (camp dialogue) at %.2f  S=%.2f  gap F2-S=%.2f", g_f2_time, g_s_time, g_f2_time - g_s_time);
		}

		if (g_rec)
		{
			rec_line("SNOTIFY %s ent=%u", name, owner);
		}
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
	unsigned char g_hero_cache[65536];   // 0 unknown, 1 no, 2 hero (string ids are renumbered by a reload, so this is cleared then)
	volatile int g_rec_gen = 0;
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
		{
			static int seen_gen = 0;
			if (seen_gen != g_rec_gen)
			{
				seen_gen = g_rec_gen;
				memset(g_rec_class_cache, 0, sizeof(g_rec_class_cache));
				memset(g_hero_cache, 0, sizeof(g_hero_cache));
			}
		}
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
					unsigned char* const hero_cache = g_hero_cache;
					static double hero_time[MAX_ENTITIES];
					unsigned int type_id = 0;
					memcpy(&type_id, buffer + OFF_TYPE, 4);
					if (type_id > 1 && type_id < 0x1000000)
					{
						if (type_id >= 0x10000)
						{
							char big[64] = {};
							class_name(type_id, big, sizeof(big));
							corm_track(slot, big, r.pos);
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
						if (hero_cache[type_id] == 2)
						{
							char cname[64] = {};
							class_name(type_id, cname, sizeof(cname));
							corm_track(slot, cname, r.pos);
						}
						if (hero_cache[type_id] == 2 && hnow - hero_time[slot] >= 0.5)
						{
							hero_time[slot] = hnow;
							char tname[64] = {};
							class_name(type_id, tname, sizeof(tname));
							rec_line("HERO #%d %s at %.0f %.0f %.0f", slot, tname, r.pos[0], r.pos[1], r.pos[2]);
						}
					}
				}

				// v0.79: Cormack's raw entity bytes - log every changed 4-byte word, to catch what makes him start walking
				{
					static bool corm_slot[MAX_ENTITIES];
					static unsigned char corm_prev[MAX_ENTITIES][0x2F8];
					static bool corm_have[MAX_ENTITIES];
					if (!corm_slot[slot])
					{
						unsigned int tid = 0;
						memcpy(&tid, buffer + OFF_TYPE, 4);
						if (tid > 1 && tid < 0x1000000)
						{
							char tn[64] = {};
							class_name(tid, tn, sizeof(tn));
							if (strstr(tn, "cormack") && strstr(tn, "hero"))
							{
								corm_slot[slot] = true;
							}
						}
					}

					if (corm_slot[slot])
					{
						unsigned char full[0x2F8];
						if (read_fast(SP_G_ENTITIES + static_cast<uintptr_t>(slot) * g_stride, full, sizeof(full)))
						{
							if (corm_have[slot])
							{
								int logged = 0;
								for (size_t off = 0; off + 4 <= sizeof(full) && logged < 24; off += 4)
								{
									if (off >= OFF_ORIGIN && off < OFF_ORIGIN + 12) continue;
									if (memcmp(full + off, corm_prev[slot] + off, 4) != 0)
									{
										unsigned int a = 0, b = 0;
										memcpy(&a, corm_prev[slot] + off, 4);
										memcpy(&b, full + off, 4);
										rec_line("CORM #%d +0x%zX %08X -> %08X", slot, off, a, b);
										++logged;
									}
								}
							}
							memcpy(corm_prev[slot], full, sizeof(full));
							corm_have[slot] = true;
						}
					}
				}

				// v0.97: Will / Cormack memory snapshots for the first 30 s after the level starts (twice a second),
				// to find where the game keeps their walking destination. Written to speedrun_data\heroes_<map>_<time>.bin
				{
					static signed char hero_kind[MAX_ENTITIES];   // 0 unknown, 1 will, 2 cormack, -1 other
					if (hero_kind[slot] == 0)
					{
						unsigned int tid = 0;
						memcpy(&tid, buffer + OFF_TYPE, 4);
						hero_kind[slot] = -1;
						if (tid > 1 && tid < 0x1000000)
						{
							char tn[64] = {};
							class_name(tid, tn, sizeof(tn));
							if (strstr(tn, "hero") && strstr(tn, "will")) hero_kind[slot] = 1;
							else if (strstr(tn, "hero") && strstr(tn, "cormack")) hero_kind[slot] = 2;
							else if (strstr(tn, "enemy")) hero_kind[slot] = 3;
						}
					}
					// v0.98: Will's / Cormack's destination (actor +0x15A8) - logged whenever it changes, plus fast positions early on
					// v1.01: enemies near the start doorway, 10 times a second for 30 s after the start
					if (hero_kind[slot] == 3 && g_bbt_time > 0.0)
					{
						static double epos_time[MAX_ENTITIES];
						const double s3 = now_seconds() - g_bbt_time;
						const float ex = r.pos[0] - 35440.0f, ey = r.pos[1] - 9790.0f;
						const float qx = r.pos[0] - 25300.0f, qy = r.pos[1] - 10700.0f;
						const bool centre = s3 > 40.0 && s3 <= 260.0 && qx * qx + qy * qy < 1800.0f * 1800.0f && r.pos[2] > 1100.0f && r.pos[2] < 2500.0f && now_seconds() - epos_time[slot] >= 0.5;
						{
							const float bx = r.pos[0] - 35466.0f, by = r.pos[1] - 9792.0f;
							if (s3 >= 8.0 && s3 <= 14.0 && bx * bx + by * by < 24.0f * 24.0f && std::fabs(r.pos[2] - 4344.0f) < 40.0f)
							{
								if (now_seconds() - g_blocker_time > 2.0) rec_line("BLOCKER #%d on Will's doorway spot at %.1f s", slot, s3);
								g_blocker_time = now_seconds();
							}
						}
						if (centre || (s3 >= -2.0 && s3 <= 30.0 && ex * ex + ey * ey < 1500.0f * 1500.0f && std::fabs(r.pos[2] - 4350.0f) < 400.0f && now_seconds() - epos_time[slot] >= 0.1))
						{
							epos_time[slot] = now_seconds();
							rec_line("EPOS #%d %.1f %.1f %.1f", slot, r.pos[0], r.pos[1], r.pos[2]);
						}
					}
					if (hero_kind[slot] == 1 || hero_kind[slot] == 2)
					{
						unsigned long long actor = 0;
						memcpy(&actor, buffer + 0x120, 8);
						static float gprev[MAX_ENTITIES][12];
						static double wpos_time[MAX_ENTITIES];
						float gv[12] = {};
						if (actor >= 0x140000000ull && actor < 0x150000000ull && read_fast(static_cast<uintptr_t>(actor) + 0x15A8, gv, sizeof(gv)))
						{
							if (memcmp(gv, gprev[slot], sizeof(gv)) != 0 && std::fabs(gv[0]) < 200000.0f)
							{
								rec_line("GOAL #%d %s goal %.0f %.0f %.0f | %.3f %.3f %.0f | end %.0f %.0f %.0f | %.0f | me-to-goal %.0f", slot, hero_kind[slot] == 1 ? "will" : "cormack",
									gv[0], gv[1], gv[2], gv[3], gv[4], gv[5], gv[7], gv[8], gv[9], gv[10],
									std::sqrt((r.pos[0] - gv[0]) * (r.pos[0] - gv[0]) + (r.pos[1] - gv[1]) * (r.pos[1] - gv[1])));
								memcpy(gprev[slot], gv, sizeof(gv));
							}
							if (hero_kind[slot] == 2)
							{
								g_wc_pos[0] = r.pos[0]; g_wc_pos[1] = r.pos[1]; g_wc_pos[2] = r.pos[2];
								g_wc_time = now_seconds();
							}
							if (hero_kind[slot] == 1 && g_bbt_time > 0.0)
							{
								// v1.12: Will's opening jump, judged from where Will actually is
								{
									const double tn = now_seconds(), since = tn - g_bbt_time;
									const float lx = r.pos[0] - 35169.0f, ly = r.pos[1] - 9573.0f;
									if (g_will_ledge_time < g_bbt_time && lx * lx + ly * ly < 60.0f * 60.0f && r.pos[2] > 4300.0f)
									{
										g_will_ledge_time = tn;
										if (g_s_time > g_bbt_time) { g_will_msg = 4; g_will_msg_time = tn; }
										else if (g_will_msg == 2) { g_will_msg = 3; g_will_msg_time = tn; }
										rec_line("WILLJUMP Will reached the ledge at %.1f s (msg %d)", since, g_will_msg);
									}
									const float jx = r.pos[0] - 35150.0f, jy = r.pos[1] - 9200.0f;
									if (g_enter2_time < g_bbt_time && jx * jx + jy * jy < 500.0f * 500.0f && r.pos[2] < 3000.0f)
									{
										g_enter2_time = tn;
										if (g_will_msg == 0) { g_will_msg = 1; g_will_msg_time = tn; }
										rec_line("WILLJUMP Will jumped down at %.1f s (msg %d)", since, g_will_msg);
									}
									if (g_will_msg == 0 && g_will_ledge_time < g_bbt_time && g_enter2_time < g_bbt_time && g_s_time < g_bbt_time &&
										since > 32.0 && since < 120.0 && r.pos[2] > 4300.0f &&
										(r.pos[0] - 35458.0f) * (r.pos[0] - 35458.0f) + (r.pos[1] - 9700.0f) * (r.pos[1] - 9700.0f) < 300.0f * 300.0f)
									{
										g_will_msg = 2; g_will_msg_time = tn;
										rec_line("WILLJUMP Will still stuck in the top building, no jump (missed) at %.1f s (msg 2)", since);
									}
								}
								const float vx = r.pos[0] - 28100.0f, vy = r.pos[1] - 7970.0f;
								if (vx * vx + vy * vy < 80.0f * 80.0f && std::fabs(r.pos[2] - 1451.0f) < 40.0f && g_will_van_time < g_bbt_time)
								{
									g_will_van_time = now_seconds();
									rec_line("WILLRACE Will at the van; T %s (T-van %.2f s)", g_t_time > g_bbt_time ? "already fired" : "not yet", g_t_time > g_bbt_time ? g_t_time - g_will_van_time : 0.0);
								}
								if (g_will_van_time > g_bbt_time && g_will_left_van_time < g_will_van_time && g_s_time < g_bbt_time &&
									vx * vx + vy * vy > 400.0f * 400.0f && r.pos[2] < 1700.0f && now_seconds() - g_will_van_time > 3.0)
								{
									g_will_left_van_time = now_seconds();
									rec_line("WILLRACE Will left the van (sent into the convention centre)");
								}
								const float cx = r.pos[0] - 24788.0f, cy = r.pos[1] - 12114.0f;
								if (cx * cx + cy * cy < 40.0f * 40.0f && g_will_cc_time < g_bbt_time)
								{
									g_will_cc_time = now_seconds();
									rec_line("WILLRACE Will reached his convention-centre spot (bad sign)");
								}
								const float kx = r.pos[0] - 33087.0f, ky = r.pos[1] - 6831.0f;
								if (g_s_time > g_bbt_time && now_seconds() - g_s_time < 40.0 && kx * kx + ky * ky < 600.0f * 600.0f && g_will_camp_time < g_s_time)
								{
									g_will_camp_time = now_seconds();
									rec_line("WILLRACE Will sent back to the camp %.1f s after the binoculars", g_will_camp_time - g_s_time);
								}
							}
							if (hero_kind[slot] == 1)
							{
								const double tn = now_seconds();
								const float mv = std::fabs(r.pos[0] - g_wg_pos[0]) + std::fabs(r.pos[1] - g_wg_pos[1]);
								if (mv > 2.0f || g_wg_still_since == 0.0) g_wg_still_since = tn;
								g_wg_goal[0] = gv[0]; g_wg_goal[1] = gv[1]; g_wg_goal[2] = gv[2];
								g_wg_pos[0] = r.pos[0]; g_wg_pos[1] = r.pos[1]; g_wg_pos[2] = r.pos[2];
								g_wg_time = tn;
							}
						}
						const double since0 = now_seconds() - g_bbt_time;
						if (g_bbt_time > 0.0 && since0 >= 0.0 && since0 <= 30.0 && now_seconds() - wpos_time[slot] >= 0.1)
						{
							wpos_time[slot] = now_seconds();
							rec_line("HPOS #%d %s %.1f %.1f %.1f", slot, hero_kind[slot] == 1 ? "will" : "cormack", r.pos[0], r.pos[1], r.pos[2]);
						}
					}
					static FILE* hf = nullptr;
					static double hf_bbt = -1.0;
					static double last_snap[MAX_ENTITIES];
					const double hnow2 = now_seconds();
					const double since = hnow2 - g_bbt_time;
					if ((hero_kind[slot] == 1 || hero_kind[slot] == 2) && g_bbt_time > 0.0 && since >= 0.0 && since <= 30.0 && hnow2 - last_snap[slot] >= 0.5)
					{
						last_snap[slot] = hnow2;
						if (hf_bbt != g_bbt_time)
						{
							if (hf) { fclose(hf); hf = nullptr; }
							hf_bbt = g_bbt_time;
							char nm[96];
							SYSTEMTIME stt; GetLocalTime(&stt);
							snprintf(nm, sizeof(nm), "heroes_%s_%04d%02d%02d_%02d%02d%02d.bin", g_rec_map[0] ? g_rec_map : "map", stt.wYear, stt.wMonth, stt.wDay, stt.wHour, stt.wMinute, stt.wSecond);
							const char* old_sub = g_subdir;
							g_subdir = "speedrun_data\\";
							char path[MAX_PATH];
							make_path(path, nm);
							g_subdir = old_sub;
							if (fopen_s(&hf, path, "wb") != 0) hf = nullptr;
							rec_line("HEROSNAP file %s", nm);
						}
						if (hf)
						{
							unsigned char full[0x2F8];
							if (read_fast(SP_G_ENTITIES + static_cast<uintptr_t>(slot) * g_stride, full, sizeof(full)))
							{
								// record: "HSNP", float time since start, int slot, int kind, entity bytes, then up to 8 pointer targets
								const float tf = static_cast<float>(since);
								const int sl = slot, kd = hero_kind[slot];
								fwrite("HSNP", 1, 4, hf); fwrite(&tf, 4, 1, hf); fwrite(&sl, 4, 1, hf); fwrite(&kd, 4, 1, hf);
								const unsigned int esz = sizeof(full);
								fwrite(&esz, 4, 1, hf); fwrite(full, 1, sizeof(full), hf);
								static unsigned char tgt[0x1800];
								int npt = 0;
								for (unsigned int off = 0; off + 8 <= sizeof(full) && npt < 8; off += 8)
								{
									unsigned long long v = 0;
									memcpy(&v, full + off, 8);
									if (v >= 0x140000000ull && v < 0x150000000ull && (v & 3) == 0)
									{
										if (read_fast(static_cast<uintptr_t>(v), tgt, sizeof(tgt)))
										{
											const unsigned int tsz = sizeof(tgt);
											fwrite("PTR_", 1, 4, hf); fwrite(&off, 4, 1, hf); fwrite(&v, 8, 1, hf); fwrite(&tsz, 4, 1, hf); fwrite(tgt, 1, sizeof(tgt), hf);
											++npt;
										}
									}
								}
								fwrite("END_", 1, 4, hf);
								fflush(hf);
							}
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
						if (inside && (strstr(text, "begin_fob_combat_vignette") || strstr(text, "at 34672 8432 1488")))
						{
							g_v_time = now_seconds();
							g_t_time = 0.0;
							g_l_time = 0.0;
							g_in_time = 0.0; g_s_time = 0.0; g_f2_time = 0.0; g_f1_time = 0.0; g_fe_time = 0.0; g_box_time = 0.0;
						}

						if (inside && g_v_time > 0.0 && g_box_time < g_v_time && (strstr(text, "player_leaving_map") || strstr(text, "at 27731 7141 2006")))
						{
							g_box_time = now_seconds();
							rec_line("BOXRACE box entered, Cormack %.0f from door, speed %.0f, T so far %.2f", g_corm_dist, g_corm_speed, g_t_time);
						}

						if (inside && g_l_time > g_v_time && g_in_time < g_l_time && (strstr(text, "player_leaving_map") || strstr(text, "at 27731 7141 2006")))
						{
							g_in_time = now_seconds();
						}

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
					g_rec_gen = g_rec_gen + 1;
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
						g_rec_gen = g_rec_gen + 1;
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

		// 100 ms per tick. v1.13: no 30-minute limit any more (it switched off Home/End position save/load mid-session).
		for (int tick = 1; tick < 2000000000; ++tick)
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

			if (tick % 10 == 0)
			{
				lab_poll();
			}

			// v1.25: every new mission, save everything about the map automatically (full collision, invisible walls,
			// hidden objects, movement settings) 20 s after it loads - no key needed.
			{
				static char dumped_map[64] = {};
				static int seen_tick = 0;
				if (g_rec_map[0] && strcmp(g_rec_map, dumped_map) != 0)
				{
					if (seen_tick == 0) seen_tick = tick;
					else if (tick - seen_tick >= 200)
					{
						log_line("map data: saving everything for %s", g_rec_map);
						refresh_brushes();
						strcpy_s(dumped_map, sizeof(dumped_map), g_rec_map);
						seen_tick = 0;
					}
				}
			}

			if (g_here_requested)
			{
				g_here_requested = false;
				dump_collision_here();
			}

			if ((g_collision_mode == 1 || g_collision_mode == 2) && (g_collision_refresh || tick % 150 == 0))
			{
				g_collision_refresh = false;
				refresh_brushes();
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

			{
				static bool n7_was = false, n9_was = false;
				static LONG shown = 0;
				const bool n7 = (GetAsyncKeyState(0x24) & 0x8000) != 0;
				const bool n9 = (GetAsyncKeyState(0x23) & 0x8000) != 0;
				if (n7 && !n7_was)
				{
					log_line("bookmark: Home pressed");
					InterlockedExchange(&g_dev_save_req, 1);
				}
				if (n9 && !n9_was)
				{
					log_line("bookmark: End pressed");
					InterlockedExchange(&g_dev_load_req, 1);
				}
				n7_was = n7;
				n9_was = n9;
				if (g_dev_result != 0)
				{
					const LONG r = InterlockedExchange(&g_dev_result, 0);
					if (r == 1) { Beep(1200, 120); }
					else if (r == 2) { Beep(900, 120); }
					else { Beep(300, 400); log_line("bookmark: failed (nothing remembered yet, or player not found)"); }
					(void)shown;
				}
			}

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

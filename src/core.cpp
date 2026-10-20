// S1x debug overlay - version 0.9 (draws with Direct3D just before the picture is shown)
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
	// Drawing: we paint our own lines on top of the finished picture, using the graphics
	// system (Direct3D 11) directly, just before the picture is shown on screen.
	// ------------------------------------------------------------------------------
	constexpr uintptr_t SP_DVAR_FIND_VAR = 0x140370860;

	constexpr size_t OFF_CAM_ORIGIN = 0x80;  // 3 floats, found in your snapshots
	constexpr size_t OFF_CAM_PITCH = 0x10C;  // degrees, positive = looking down
	constexpr size_t OFF_CAM_YAW = 0x110;    // degrees
	constexpr size_t DVAR_CURRENT_OFFSET = 0x18;

	constexpr float EYE_HEIGHT = 60.0f;
	constexpr int MAX_BOXES = 64;
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
		unsigned char kind;
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
	volatile int g_fov_style = 1;
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

	float kColors[6][4] = {
		{0.1f, 1.0f, 0.2f, 1.0f},   // trigger_multiple (green)
		{1.0f, 0.2f, 0.2f, 1.0f},   // hurt / damage (red)
		{1.0f, 0.9f, 0.1f, 1.0f},   // use (yellow)
		{1.0f, 0.55f, 0.1f, 1.0f},  // radius (orange)
		{0.2f, 0.8f, 1.0f, 1.0f},   // everything else (blue)
		{1.0f, 1.0f, 1.0f, 1.0f},   // you are inside this one (white)
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
		add_line(x0, y0, x1, y1, color, kThickness[g_thick_level]);
	}

	void draw_box(const hit& h)
	{
		const float* color = h.inside ? kColors[5] : kColors[h.kind > 1 && h.kind < 7 ? h.kind - 2 : 4];

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

	// Reads the F-keys. Runs once per picture, whether or not anything is being drawn.
	void poll_keys()
	{
		static bool was_f5, was_f6, was_f7, was_f8;
		const bool f5 = key_pressed(VK_F5, &was_f5);
		const bool f6 = key_pressed(VK_F6, &was_f6);
		const bool f7 = key_pressed(VK_F7, &was_f7);
		const bool f8 = key_pressed(VK_F8, &was_f8);

		if (f8)
		{
			g_overlay_on = !g_overlay_on;
			log_line("F8: overlay %s", g_overlay_on ? "ON" : "OFF");
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

	// Fills g_verts with everything to draw this picture. Returns false if there is nothing to draw.
	bool build_picture(const float width, const float height)
	{
		g_w = width;
		g_h = height;
		g_vert_count = 0;

		unsigned long long pointer = 0;
		if (!read_bytes(SP_G_ENTITIES + OFF_CLIENT, &pointer, sizeof(pointer)) || pointer < 0x140000000ull ||
			pointer > 0x7FFFFFFE0000ull)
		{
			return false;
		}

		float origin[3];
		float pitch = 0.0f;
		float yaw = 0.0f;
		if (!read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_ORIGIN, origin, sizeof(origin)) ||
			!read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_PITCH, &pitch, sizeof(pitch)) ||
			!read_bytes(static_cast<uintptr_t>(pointer) + OFF_CAM_YAW, &yaw, sizeof(yaw)))
		{
			return false;
		}

		if (!(std::fabs(origin[0]) < 500000.0f && std::fabs(origin[1]) < 500000.0f && std::fabs(origin[2]) < 500000.0f &&
			std::fabs(pitch) <= 91.0f && std::fabs(yaw) <= 720.0f))
		{
			return false;
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
		g_eye = {origin[0], origin[1], origin[2] + EYE_HEIGHT};
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

		hit hits[MAX_BOXES];
		const int count = find_triggers({origin[0], origin[1], origin[2] + 30.0f}, kDistances[g_dist_level], hits, MAX_BOXES);

		for (int i = count - 1; i >= 0; --i)
		{
			draw_box(hits[i]);
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

		log_line("---- add-on loaded (version 0.9, draws with Direct3D just before the picture is shown) ----");
		log_line("host program: %s", exe);
		log_line("F9 = save entity table | F8 on/off | F7 distance | F6 thickness | F5 fov style");

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

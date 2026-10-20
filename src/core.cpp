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
		unsigned int name_id;
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

	// ---- tiny built-in text (5x7 letters drawn as little rectangles) ------------------------
	volatile bool g_labels_on = true;   // F2

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

	void draw_label(const hit& h, const float* color)
	{
		const vec3 c = to_camera(h.mid);
		if (c.z < 8.0f)
		{
			return;
		}

		float sx, sy;
		to_screen(c, &sx, &sy);
		if (sx < 0.0f || sy < 0.0f || sx > g_w || sy > g_h)
		{
			return;
		}

		char name[48];
		if (!class_name(h.class_id, name, sizeof(name)))
		{
			return;
		}

		const char* shown = strncmp(name, "trigger_", 8) == 0 ? name + 8 : name;
		char text[96];
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

		if (given[0])
		{
			snprintf(text, sizeof(text), "%s #%d %dm [%s]", shown, h.slot, static_cast<int>(h.dist / 39.37f), given);
		}
		else
		{
			snprintf(text, sizeof(text), "%s #%d %dm", shown, h.slot, static_cast<int>(h.dist / 39.37f));
		}

		const float scale = g_h >= 1200.0f ? 2.0f : 1.0f;
		const float width = static_cast<float>(strlen(text)) * 6.0f * scale;
		const float height = 7.0f * scale;
		const float x = sx - width * 0.5f;
		const float y = sy - height * 0.5f;
		const float back[4] = {0.0f, 0.0f, 0.0f, 0.65f};
		add_rect(x - 3.0f, y - 3.0f, width + 5.0f, height + 6.0f, back);
		draw_text(x, y, text, scale, color);
	}

	// ---- script events (what the map scripts are being told) -------------------------------
	// Hooks the game's "notify" function. Every time a script sends a message such as "trigger" or
	// "death", we remember its name and show the newest ones in the top-right corner.
	constexpr uintptr_t SP_SCR_NOTIFY_ID = 0x14031CB80;
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
			unsigned char buffer[0x170];
			if (!read_bytes(SP_G_ENTITIES + static_cast<uintptr_t>(index) * ENTITY_SIZE, buffer, sizeof(buffer)))
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

	void record_event(const unsigned int entity, const unsigned int string_id)
	{
		char name[44];
		if (!class_name(string_id, name, sizeof(name)))
		{
			return;
		}

		const double now = now_seconds();
		count_event(name, entity, now);
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
			if (string_id != 0 && string_id < 0x10000)
			{
				record_event(entity, string_id);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}

		g_original_notify(entity, string_id, count);
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
		unsigned char buffer[0x170];

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
			memcpy(&h.name_id, buffer + 0x160, sizeof(h.name_id));
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
	const camera_source kSources[SOURCE_COUNT] = {
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
			if (!read_bytes(SP_G_ENTITIES + OFF_CLIENT, &pointer, sizeof(pointer)) || pointer < 0x140000000ull)
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

	void poll_keys()
	{
		static bool was_f11, was_f2, was_f3, was_f4, was_f5, was_f6, was_f7, was_f8;
		const bool f11 = key_pressed(VK_F11, &was_f11);
		const bool f2 = key_pressed(VK_F2, &was_f2);
		const bool f3 = key_pressed(VK_F3, &was_f3);
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
		if (!read_bytes(SP_G_ENTITIES + OFF_CLIENT, &pointer, sizeof(pointer)) || pointer < 0x140000000ull ||
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

		if (source_index == 0)
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

		hit hits[MAX_BOXES];
		const int count = find_triggers({origin[0], origin[1], origin[2] + 30.0f - (eye_height == 0.0f ? EYE_HEIGHT : 0.0f)}, kDistances[g_dist_level], hits, MAX_BOXES);

		for (int i = count - 1; i >= 0; --i)
		{
			draw_box(hits[i]);
		}

		if (g_events_on)
		{
			draw_events();
		}

		if (g_labels_on)
		{
			const int label_count = count < 20 ? count : 20;
			for (int i = label_count - 1; i >= 0; --i)
			{
				draw_label(hits[i], hits[i].inside ? kColors[5] : kColors[hits[i].kind > 1 && hits[i].kind < 7 ? hits[i].kind - 2 : 4]);
			}
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
			unsigned char buffer[OFF_CLASSNAME + 2];
			if (!read_bytes(SP_G_ENTITIES + static_cast<uintptr_t>(index) * ENTITY_SIZE, buffer, sizeof(buffer)))
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

	bool g_notify_hooked = false;

	// Tries to hook the script "notify" function. Can be called again and again until it works.
	void try_hook_notify(const int attempt)
	{
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

		return strstr(exe, "s1_sp64_ship") != nullptr;
	}

	DWORD WINAPI worker(LPVOID)
	{
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exe, MAX_PATH);

		log_line("---- add-on loaded (version 0.20, script events (player animations hidden)) ----");
		log_line("host program: %s", exe);
		log_line("F8 on/off | F2 labels | F11 events | F7 distance | F6 thickness | F5 fov style | F4 event filter on/off | Insert geometry search | F3 camera source");

		bool hook_done = false;
		int notify_tries = 0;

		int tables = 0;
		int seconds_with_data = 0;
		bool auto_done = false;
		bool f9_was_down = false;
		bool insert_was_down = false;

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

			if (hook_done && !g_notify_hooked && notify_tries < 24 && tick % 50 == 0)
			{
				++notify_tries;
				try_hook_notify(notify_tries);
			}

			if (tick % 10 == 0)
			{
				seconds_with_data = entity_list_has_data() ? seconds_with_data + 1 : 0;
			}

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
				write_event_summary();
				write_trigger_names();
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

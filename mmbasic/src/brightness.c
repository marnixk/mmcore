/*
 * Display brightness helper.
 *
 * The interpreter's MM.BRIGHTNESS% variable reads and writes the panel
 * backlight percentage on native Linux (including the Chromebook ISO). The
 * device-directory helpers live here so the percentage maths can be unit
 * tested on any host; the get/set wrappers discover the sysfs device on Linux
 * and fall back to 100 (read) / no-op (write) everywhere else.
 */
#include "mmbasic.h"
#include "mmb_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#if defined(__linux__)
#include <dirent.h>
#endif

#define MMB_BRIGHTNESS_DEFAULT 100

static int read_int_file(const char *path)
{
	FILE *f;
	char buf[32];
	char *end;
	long v;

	f = fopen(path, "r");
	if (!f)
		return INT_MIN;
	if (!fgets(buf, (int)sizeof(buf), f))
	{
		fclose(f);
		return INT_MIN;
	}
	fclose(f);
	v = strtol(buf, &end, 10);
	if (end == buf)
		return INT_MIN;
	if (v < 0)
		v = 0;
	if (v > INT_MAX)
		v = INT_MAX;
	return (int)v;
}

static int join_path(char *out, int outsz, const char *dir, const char *name)
{
	int n = snprintf(out, (size_t)outsz, "%s/%s", dir, name);

	return n > 0 && n < outsz;
}

int mmb_brightness_read(const char *devdir, int fallback)
{
	char path[512];
	int max, cur;
	long pct;

	if (!devdir)
		return fallback;
	if (!join_path(path, (int)sizeof(path), devdir, "max_brightness"))
		return fallback;
	max = read_int_file(path);
	if (max <= 0)
		return fallback;
	if (!join_path(path, (int)sizeof(path), devdir, "brightness"))
		return fallback;
	cur = read_int_file(path);
	if (cur == INT_MIN)
	{
		if (!join_path(path, (int)sizeof(path), devdir, "actual_brightness"))
			return fallback;
		cur = read_int_file(path);
	}
	if (cur == INT_MIN)
		return fallback;
	pct = ((long)cur * 100 + max / 2) / max;
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	return (int)pct;
}

int mmb_brightness_write(const char *devdir, int pct)
{
	char path[512];
	int max;
	long val;
	FILE *f;

	if (!devdir)
		return -1;
	if (!join_path(path, (int)sizeof(path), devdir, "max_brightness"))
		return -1;
	max = read_int_file(path);
	if (max <= 0)
		return -1;
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	val = ((long)pct * max + 50) / 100;
	if (val < 0)
		val = 0;
	if (val > max)
		val = max;
	if (!join_path(path, (int)sizeof(path), devdir, "brightness"))
		return -1;
	f = fopen(path, "w");
	if (!f)
		return -1;
	if (fprintf(f, "%ld\n", val) < 0)
	{
		fclose(f);
		return -1;
	}
	if (fclose(f) != 0)
		return -1;
	return 0;
}

static int usable_dir(const char *devdir)
{
	char path[512];
	FILE *f;

	if (!join_path(path, (int)sizeof(path), devdir, "brightness"))
		return 0;
	f = fopen(path, "r");
	if (!f)
		return 0;
	fclose(f);
	if (!join_path(path, (int)sizeof(path), devdir, "max_brightness"))
		return 0;
	f = fopen(path, "r");
	if (!f)
		return 0;
	fclose(f);
	return 1;
}

int mmb_backlight_find(char *out, int outsz)
{
	/* Escape hatch for tests and unusual installs: point straight at a
	 * backlight device directory. */
	const char *override = getenv("MMB_BACKLIGHT");

	if (!out || outsz <= 0)
		return 0;
	if (override && *override && usable_dir(override))
	{
		snprintf(out, (size_t)outsz, "%s", override);
		return 1;
	}
#if defined(__linux__)
	{
		/* Prefer the real Intel/AMD panel backlight over the ACPI video
		 * shim, which often reports brightness without changing the panel. */
		static const char *const pref[] = {
			"intel_backlight", "amdgpu_bl0", "amdgpu_bl1",
			"radeon_bl0", "acpi_video0", NULL
		};
		const char *base = "/sys/class/backlight";
		char cand[512];
		int i;
		DIR *d;
		struct dirent *e;

		for (i = 0; pref[i]; i++)
		{
			if (snprintf(cand, sizeof(cand), "%s/%s", base, pref[i]) >=
			    (int)sizeof(cand))
				continue;
			if (usable_dir(cand))
			{
				snprintf(out, (size_t)outsz, "%s", cand);
				return 1;
			}
		}
		d = opendir(base);
		if (!d)
			return 0;
		while ((e = readdir(d)))
		{
			if (e->d_name[0] == '.')
				continue;
			if (snprintf(cand, sizeof(cand), "%s/%s", base,
				     e->d_name) >= (int)sizeof(cand))
				continue;
			if (usable_dir(cand))
			{
				snprintf(out, (size_t)outsz, "%s", cand);
				closedir(d);
				return 1;
			}
		}
		closedir(d);
	}
#endif
	return 0;
}

int mmb_brightness_get(void)
{
	char devdir[512];

	if (mmb_backlight_find(devdir, (int)sizeof(devdir)))
		return mmb_brightness_read(devdir, MMB_BRIGHTNESS_DEFAULT);
	return MMB_BRIGHTNESS_DEFAULT;
}

int mmb_brightness_set(int pct)
{
	char devdir[512];

	if (mmb_backlight_find(devdir, (int)sizeof(devdir)))
		return mmb_brightness_write(devdir, pct);
	return -1;
}

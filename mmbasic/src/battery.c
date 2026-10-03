/*
 * Battery capacity helper (#857).
 *
 * The interpreter's BATTERY%() function reads the Linux sysfs battery
 * capacity on the native build and returns 100 everywhere else. The read
 * itself lives behind this path-taking helper so it can be unit tested on
 * any host without a real battery.
 */
#include "mmbasic.h"
#include "mmb_priv.h"

#include <stdio.h>
#include <stdlib.h>

int mmb_battery_capacity_read(const char *path, int fallback)
{
	FILE *f;
	char buf[32];
	char *end;
	long v;

	if (!path)
		return fallback;
	f = fopen(path, "r");
	if (!f)
		return fallback;
	if (!fgets(buf, (int)sizeof(buf), f))
	{
		fclose(f);
		return fallback;
	}
	fclose(f);

	v = strtol(buf, &end, 10);
	if (end == buf)
		return fallback;
	if (v < 0)
		v = 0;
	if (v > 100)
		v = 100;
	return (int)v;
}

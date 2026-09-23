/* Host unit test for the bare-metal sprintf formatter core (#549).
 *
 * Native builds take sprintf() from libc, so this compiles libc_shims.c with
 * MMB_PLATFORM_POSIX and exercises the shared mmb_vsnprintf/mmb_snprintf used
 * by the bare-metal shim. The old shim skipped width/precision flags, so
 * %02d, %5s and %.2f all came out un-padded. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int mmb_vsnprintf(char *str, unsigned long size, const char *fmt, va_list ap);
int mmb_snprintf(char *str, unsigned long size, const char *fmt, ...);

static int fails;

static void check(const char *want, const char *fmt, ...)
{
	char got[128];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = mmb_vsnprintf(got, sizeof got, fmt, ap);
	va_end(ap);
	if (strcmp(got, want) != 0 || n != (int)strlen(want))
	{
		fprintf(stderr, "FAIL %-10s want [%s] (%d) got [%s] (%d)\n", fmt,
			want, (int)strlen(want), got, n);
		fails++;
	}
}

static void check_snprintf(void)
{
	char buf[8];
	int n;

	n = mmb_snprintf(buf, sizeof buf, "%05d", 42);
	if (strcmp(buf, "00042") != 0 || n != 5)
	{
		fprintf(stderr, "FAIL mmb_snprintf padded: got [%s] n=%d\n", buf, n);
		fails++;
	}
	/* Truncation must stay NUL-terminated and still return the full length. */
	n = mmb_snprintf(buf, 4, "%d", 12345);
	if (strcmp(buf, "123") != 0 || n != 5)
	{
		fprintf(stderr, "FAIL mmb_snprintf truncation: got [%s] n=%d\n", buf, n);
		fails++;
	}
}

int main(void)
{
	/* Width and zero padding. */
	check("03", "%02d", 3);
	check("   42", "%5d", 42);
	check("42   ", "%-5d", 42);
	check("-0042", "%05d", -42);
	check("+42", "%+d", 42);
	check("007", "%.3d", 7);
	check("001f", "%04x", 0x1f);
	check("0x001f", "%#06x", 0x1f);
	check("AB", "%X", 0xab);

	/* String width and precision. */
	check("   hi", "%5s", "hi");
	check("hi   ", "%-5s", "hi");
	check("he", "%.2s", "hello");
	check("  he", "%4.2s", "hello");

	/* Float precision (the bare-metal code serialises with %1.15g). */
	check("3.14", "%.2f", 3.14159);
	check("003.14", "%06.2f", 3.14159);
	check("12.50", "%.2f", 12.5);
	check("12.5", "%1.15g", 12.5);
	check("100", "%g", 100.0);
	check("1.5e+00", "%.1e", 1.5);

	check_snprintf();

	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}

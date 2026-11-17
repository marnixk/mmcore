/* Host test for the PACKAGE.INF parser and version compare (#1523).
 *
 * Compiles the real mmbasic/src/pkg_info.c + app_category.c against a tiny
 * allocator shim and checks the tolerant line parser (missing/empty values,
 * CRLF, unknown keys, duplicates, '=' in values, long summaries), the exact
 * build text, and the dotted-version table from the issue.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmb_priv.h"
#include "pkg_info.h"

mmb_globals_t G;

static int fails;

static void check(int cond, const char *what)
{
	if (!cond)
	{
		printf("FAIL: %s\n", what);
		fails++;
	}
}

static void check_str(const char *got, const char *want, const char *what)
{
	if (!got)
		got = "(null)";
	if (!want)
		want = "(null)";
	if (strcmp(got, want) != 0)
	{
		printf("FAIL: %s: got '%s', want '%s'\n", what, got, want);
		fails++;
	}
}

static void *host_alloc(unsigned n)
{
	return malloc(n ? n : 1);
}

static void host_free(void *p)
{
	free(p);
}

static mmb_plat_t s_plat = { host_alloc, host_free };

static void parse_text(const char *text, struct mmb_pkg_info *pi)
{
	check(mmb_pkg_info_parse(text, (unsigned)strlen(text), pi) == 0,
	      "parse rc");
}

static void test_parse_basic(void)
{
	struct mmb_pkg_info pi;
	const char *text =
		"name=APP.APP\n"
		"main=MAIN.BAS\n"
		"title=My Game\n"
		"author=Me\n"
		"category=games\n"
		"version=1.2.3\n"
		"summary=Fly high\n"
		"minver=0.312.0\n";

	parse_text(text, &pi);
	check_str(pi.name, "APP.APP", "name");
	check_str(pi.title, "My Game", "title");
	check_str(pi.author, "Me", "author");
	check_str(pi.category, "GAMES", "category uppercase");
	check_str(pi.version, "1.2.3", "version");
	check_str(pi.summary, "Fly high", "summary");
	check_str(pi.minver, "0.312.0", "minver");
	check(pi.cat_index == 0, "category index GAMES");
	mmb_pkg_info_free(&pi);
}

static void test_parse_missing_and_empty(void)
{
	struct mmb_pkg_info pi;

	parse_text("", &pi);
	check(pi.name == 0 && pi.category == 0, "empty text fields");
	check(pi.cat_index == MMB_APP_CATEGORY_OTHER, "empty category OTHER");
	mmb_pkg_info_free(&pi);

	parse_text("name=\ntitle=\ncategory=\nversion=\n", &pi);
	check_str(pi.name, "", "empty name");
	check_str(pi.title, "", "empty title");
	check_str(pi.category, "", "empty category string");
	check(pi.cat_index == MMB_APP_CATEGORY_OTHER, "empty category OTHER 2");
	mmb_pkg_info_free(&pi);
}

static void test_parse_crlf(void)
{
	struct mmb_pkg_info pi;

	parse_text("name=A.APP\r\ntitle=CRLF App\r\nsummary=Line\r\n", &pi);
	check_str(pi.name, "A.APP", "crlf name");
	check_str(pi.title, "CRLF App", "crlf title");
	check_str(pi.summary, "Line", "crlf summary");
	mmb_pkg_info_free(&pi);
}

static void test_parse_unknown_category_and_keys(void)
{
	struct mmb_pkg_info pi;

	parse_text("category=wibble\ntags=x\nwebsite=y\ndescription=z\n", &pi);
	check_str(pi.category, "WIBBLE", "unknown category preserved uppercase");
	check(pi.cat_index == MMB_APP_CATEGORY_OTHER, "unknown category OTHER");
	check(pi.name == 0 && pi.title == 0, "unknown keys ignored");
	mmb_pkg_info_free(&pi);

	parse_text("category=OtHeR\n", &pi);
	check_str(pi.category, "OTHER", "category case-insensitive");
	check(pi.cat_index == MMB_APP_CATEGORY_OTHER, "other index");
	mmb_pkg_info_free(&pi);
}

static void test_parse_duplicates_and_values(void)
{
	struct mmb_pkg_info pi;

	parse_text("title=First\ntitle=Second\n", &pi);
	check_str(pi.title, "Second", "duplicate last wins");
	mmb_pkg_info_free(&pi);

	parse_text("  title \t=  Hello = World  \n", &pi);
	check_str(pi.title, "Hello = World", "value with '=' and spaces");
	mmb_pkg_info_free(&pi);
}

static void test_parse_malformed_and_bounds(void)
{
	struct mmb_pkg_info pi;
	const char *text = "garbage\n=noname\nname\n\nname=OK\n";

	parse_text(text, &pi);
	check_str(pi.name, "OK", "malformed lines ignored");
	mmb_pkg_info_free(&pi);

	/* The length is authoritative: data past n is never read. */
	check(mmb_pkg_info_parse("name=ABC\nZZZ=no", 9, &pi) == 0, "n bound rc");
	check_str(pi.name, "ABC", "n bound name");
	check(pi.title == 0 && pi.summary == 0, "n bound ignored tail");
	mmb_pkg_info_free(&pi);

	check(mmb_pkg_info_parse("name=X\n", 7, 0) != 0, "NULL out rejected");
}

static void test_parse_long_summary(void)
{
	struct mmb_pkg_info pi;
	char text[256];
	char longsum[121];

	memset(longsum, 'x', 120);
	longsum[120] = 0;
	snprintf(text, sizeof(text), "summary=%s\n", longsum);
	parse_text(text, &pi);
	check(pi.summary != 0 && strlen(pi.summary) == 120,
	      "long summary preserved by parser");
	mmb_pkg_info_free(&pi);
}

static void test_build(void)
{
	struct mmb_pkg_info pi;
	char longsum[121];
	char *out = 0;
	unsigned n = 0;

	mmb_pkg_info_init(&pi);
	mmb_pkg_info_set(&pi, "name", "T.APP");
	mmb_pkg_info_set(&pi, "title", "Title");
	mmb_pkg_info_set(&pi, "author", "Auth");
	mmb_pkg_info_set(&pi, "category", "games");
	mmb_pkg_info_set(&pi, "version", "1.10");
	memset(longsum, 'y', 120);
	longsum[120] = 0;
	mmb_pkg_info_set(&pi, "summary", longsum);
	mmb_pkg_info_set(&pi, "minver", "0.312.0");
	check(mmb_pkg_info_build(&pi, &out, &n) == 0, "build rc");
	check(out != 0 && n == strlen(out), "build length");
	check(out && strncmp(out, "name=T.APP\nmain=MAIN.BAS\n", 24) == 0,
	      "build starts with name/main");
	check(out && strstr(out, "category=GAMES\n") != 0, "build category");
	check(out && strstr(out, "version=1.10\n") != 0, "build version");
	check(out && strstr(out, "minver=0.312.0\n") != 0, "build minver");
	check(out && strstr(out, "summary=") != 0, "build summary present");
	mmb_pkg_info_free(&pi);

	if (out)
	{
		struct mmb_pkg_info back;
		parse_text(out, &back);
		check_str(back.name, "T.APP", "round-trip name");
		check_str(back.category, "GAMES", "round-trip category");
		check(back.cat_index == 0, "round-trip index");
		check(back.summary != 0 && strlen(back.summary) == 80,
		      "build truncates summary to 80");
		mmb_pkg_info_free(&back);
		G.plat->free(out);
	}

	/* Empty optional fields are omitted. */
	mmb_pkg_info_init(&pi);
	mmb_pkg_info_set(&pi, "name", "E.APP");
	out = 0;
	check(mmb_pkg_info_build(&pi, &out, &n) == 0, "build minimal rc");
	check_str(out, "name=E.APP\nmain=MAIN.BAS\n", "build minimal text");
	if (out)
		G.plat->free(out);
	mmb_pkg_info_free(&pi);

	check(mmb_pkg_info_build(0, 0, 0) != 0, "build NULL out rejected");
}

static void vcheck(const char *a, const char *b, int want)
{
	int got = mmb_version_cmp(a, b);
	if ((want < 0 && got >= 0) || (want == 0 && got != 0) ||
	    (want > 0 && got <= 0))
	{
		printf("FAIL: version_cmp('%s','%s') got %d want sign %d\n",
		       a ? a : "(null)", b ? b : "(null)", got, want);
		fails++;
	}
	got = mmb_version_cmp(b, a);
	if ((want < 0 && got <= 0) || (want == 0 && got != 0) ||
	    (want > 0 && got >= 0))
	{
		printf("FAIL: version_cmp('%s','%s') reverse got %d\n",
		       b ? b : "(null)", a ? a : "(null)", got);
		fails++;
	}
}

static void test_version_cmp(void)
{
	vcheck("1.2", "1.10", -1);
	vcheck("1.10", "1.9", 1);
	vcheck("0.312.0", "0.311.9", 1);
	vcheck("v0.312.0-5-gabc", "0.312.0", 0);
	vcheck("", "0.1", -1);
	vcheck("0", "", 0);
	vcheck("1", "1.0.0", 0);
	vcheck("V2.3", "2.3", 0);
	vcheck("1.2a", "1.2", 0);
	vcheck("2", "10", -1);
	vcheck("1.0.1", "1.0", 1);
	vcheck(0, "0", 0);
}

int main(void)
{
	G.plat = &s_plat;
	test_parse_basic();
	test_parse_missing_and_empty();
	test_parse_crlf();
	test_parse_unknown_category_and_keys();
	test_parse_duplicates_and_values();
	test_parse_malformed_and_bounds();
	test_parse_long_summary();
	test_build();
	test_version_cmp();
	if (fails)
	{
		printf("%d check(s) failed\n", fails);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}

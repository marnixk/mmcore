"""#987: the keyword name->id table is a hash lookup, not a linear scan.

Compiles the real ``mmbasic/src/tok.c`` with ``MMB_KW_STATS`` (the counters are
compiled out of production builds) and checks:

* every id round-trips through ``mmb_tok_expand`` and back to ``mmb_kw_id``;
* ids are contiguous ``1..N`` (so ``fun_tab``/``tab``/``emit_tok`` stay valid);
* a keyword resolves with a small, O(1)-ish number of string comparisons;
* unknown names return 0.

A regression back to the linear ``lookup_kw`` scan would push the comparisons
per lookup from ~1 to ~180 and fail the budget below.
"""

import os
import shutil
import subprocess
import sys

import pytest

DRIVER_C = r"""
#include "mmb_priv.h"
#include <stdio.h>
#include <string.h>

mmb g_state;
mmb *g_cur = &g_state;

extern unsigned long mmb_kw_cmp_total;
extern unsigned long mmb_kw_lookup_calls;

static int fails;

static void check(int cond, const char *what)
{
	if (!cond)
	{
		printf("FAIL %s\n", what);
		fails++;
	}
}

int main(void)
{
	char tok[3];
	char name[MMB_MAX_NAME];
	char lower[MMB_MAX_NAME];
	int id, n = 0, i;
	unsigned long cmps, calls;

	/* Discover the number of keywords and round-trip each id. */
	for (id = 1; id < 600; id++)
	{
		tok[0] = (char)0x80;
		tok[1] = (char)(id & 0xff);
		tok[2] = (char)((id >> 8) & 0xff);
		G.p = tok;
		if (!mmb_tok_expand(name, (int)sizeof(name)))
			break;
		if (mmb_kw_id(name) != id)
		{
			printf("FAIL roundtrip id=%d name=%s\n", id, name);
			fails++;
		}
		for (i = 0; name[i]; i++)
			lower[i] = (name[i] >= 'A' && name[i] <= 'Z')
				? (char)(name[i] + 32) : name[i];
		lower[i] = 0;
		if (mmb_kw_id(lower) != id)
		{
			printf("FAIL lowercase id=%d name=%s\n", id, lower);
			fails++;
		}
		n = id;
	}
	check(n > 0 && n <= 511, "count_in_range");
	check(mmb_kw_id("") == 0, "empty_is_unknown");
	check(mmb_kw_id("ZZZ_NOT_A_KEYWORD") == 0, "unknown_zero");

	/* Comparisons per lookup for a full sweep of every keyword. */
	mmb_kw_cmp_total = 0;
	mmb_kw_lookup_calls = 0;
	for (id = 1; id <= n; id++)
	{
		tok[0] = (char)0x80;
		tok[1] = (char)(id & 0xff);
		tok[2] = (char)((id >> 8) & 0xff);
		G.p = tok;
		check(mmb_tok_expand(name, (int)sizeof(name)) == 1, "expand");
		check(mmb_kw_id(name) == id, "sweep_roundtrip");
	}
	cmps = mmb_kw_cmp_total;
	calls = mmb_kw_lookup_calls;
	/* Linear lookup averages ~180 comparisons; a hash stays near 1. Budget
	 * 8 gives headroom for collisions without ever accepting the old scan. */
	check(calls == (unsigned long)n, "lookup_calls");
	check(cmps <= calls * 8, "comparisons_bounded");

	printf("KW n=%d cmps=%lu calls=%lu avg=%.2f\n", n, cmps, calls,
	       calls ? (double)cmps / (double)calls : 0.0);
	printf("%s\n", fails ? "FAILURES" : "ALL OK");
	return fails ? 1 : 0;
}
"""


@pytest.fixture(scope="module")
def kw_host_run(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tmp = tmp_path_factory.mktemp("tok_keywords")
    (tmp / "mmb_version.h").write_text('#define MMB_VERSION "test"\n')
    (tmp / "driver.c").write_text(DRIVER_C)
    exe = tmp / "driver"
    gcflags = ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra",
            "-DMMB_PLATFORM_POSIX", "-DMMB_KW_STATS",
            "-ffunction-sections", "-fdata-sections",
            "-I", str(tmp),
            "-I", os.path.join(repo, "mmbasic", "include"),
            "-o", str(exe),
            str(tmp / "driver.c"),
            os.path.join(repo, "mmbasic", "src", "tok.c"),
            *gcflags,
        ],
        check=True,
        cwd=repo,
    )
    return subprocess.run([str(exe)], check=False, capture_output=True, text=True)


def test_keyword_hash_table(kw_host_run):
    out = kw_host_run.stdout + kw_host_run.stderr
    assert kw_host_run.returncode == 0, out
    assert "ALL OK" in out, out
    assert "FAIL" not in out, out

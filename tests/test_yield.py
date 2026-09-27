"""Host test: cooperative background yield registry (#858).

``mmbasic/src/yield.c`` is deliberately interpreter-free so the register /
unregister / rate-limit rules can be exercised on the host. The callbacks the
apps register (JUKE queue, TERM/CONNECT socket drain) are driven from
``mmb_poll()`` via ``mmb_yield_run()``.
"""

import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PROG = r"""
#include "yield.h"
#include <stdio.h>

static int a_calls, b_calls, a_console = -1;
static int a_ctx, b_ctx;

static void cb_a(int console, void *ctx) { a_calls++; a_console = console; a_ctx = *(int *)ctx; }
static void cb_b(int console, void *ctx) { (void)console; b_calls++; b_ctx = *(int *)ctx; }

int main(void)
{
	int fails = 0;
	int want_a = 11, want_b = 22;

	/* A callback receives its owning console and its context. */
	mmb_yield_add(1, cb_a, &want_a, 100);
	mmb_yield_add(2, cb_b, &want_b, 0);
	if (mmb_yield_count() != 2) {
		printf("FAIL: count=%d\n", mmb_yield_count());
		fails++;
	}
	mmb_yield_run(1000);  /* first run is always due */
	if (a_calls != 1 || b_calls != 1) {
		printf("FAIL: first run a=%d b=%d\n", a_calls, b_calls);
		fails++;
	}
	if (a_console != 1 || a_ctx != want_a || b_ctx != want_b) {
		printf("FAIL: console/ctx not delivered\n");
		fails++;
	}

	/* Rate limit: cb_a waits 100 ms, cb_b has no limit. */
	mmb_yield_run(1030);
	if (a_calls != 1) { printf("FAIL: cb_a ran inside its interval\n"); fails++; }
	if (b_calls != 2) { printf("FAIL: cb_b did not run\n"); fails++; }
	mmb_yield_run(1099);
	if (a_calls != 1) { printf("FAIL: cb_a ran one ms early\n"); fails++; }
	mmb_yield_run(1100);
	if (a_calls != 2) { printf("FAIL: cb_a did not run at its interval\n"); fails++; }

	/* Re-adding the same callback replaces its slot, it does not stack. */
	mmb_yield_add(1, cb_a, &want_a, 100);
	if (mmb_yield_count() != 2) {
		printf("FAIL: re-add leaked (%d)\n", mmb_yield_count());
		fails++;
	}

	/* Removing by callback clears every console's registration of it. */
	{
		int a0 = a_calls, b0 = b_calls;
		mmb_yield_remove(cb_a);
		mmb_yield_run(3000);
		if (a_calls != a0) {
			printf("FAIL: removed callback still ran\n");
			fails++;
		}
		/* cb_b (another console) is untouched. */
		if (b_calls == b0) {
			printf("FAIL: removing cb_a also stopped cb_b\n");
			fails++;
		}
	}

	/* Removing by console leaves other consoles' callbacks alone. */
	{
		int a0 = a_calls, b0 = b_calls;
		mmb_yield_remove_console(2);
		mmb_yield_run(4000);
		if (a_calls != a0 || b_calls != b0) {
			printf("FAIL: wrong console removed\n");
			fails++;
		}
	}
	if (mmb_yield_count() != 0) {
		printf("FAIL: remove_console count=%d\n", mmb_yield_count());
		fails++;
	}

	/* clear() drops everything (warm reset path). */
	mmb_yield_add(0, cb_a, &want_a, 0);
	mmb_yield_add(1, cb_b, &want_b, 0);
	mmb_yield_clear();
	if (mmb_yield_count() != 0) { printf("FAIL: clear left entries\n"); fails++; }

	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
"""


def test_yield_registry_register_unregister_rate_limit(tmp_path):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    src = os.path.join(tmp_path, "yield_host.c")
    exe = os.path.join(tmp_path, "yield_host")
    with open(src, "w", encoding="utf-8") as fh:
        fh.write(PROG)
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
            "-I", os.path.join(REPO, "mmbasic", "include"),
            "-o", exe,
            src, os.path.join(REPO, "mmbasic", "src", "yield.c"),
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], check=True, capture_output=True, text=True)
    assert "all checks passed" in out.stdout, out.stdout + out.stderr

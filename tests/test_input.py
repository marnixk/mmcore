"""Console INPUT reads a typed line into variables."""

import os
import shutil
import subprocess
import sys
import time

import pytest

from harness import prompt_at_buffer_end


def _wait_contains(con, token: bytes, timeout: float = 5.0) -> bytes:
    deadline = time.time() + timeout
    seen = b""
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            seen += chunk
            if token in seen:
                return seen
        else:
            time.sleep(0.05)
    return seen


def _until_prompt(con, timeout: float = 5.0) -> str:
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            buf += chunk
            if prompt_at_buffer_end(buf, con.prompt):
                break
        else:
            time.sleep(0.05)
    return buf.decode(errors="replace")


def test_input_console_string(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line("10 INPUT a$")
    c.send_line('20 PRINT "[" + a$ + "]"')
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    assert b"?" in _wait_contains(c, b"?")
    c._ser.sendall(b"hello\r")
    out = _until_prompt(c)
    assert "[hello]" in out


def test_input_console_prompt_and_csv(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line('10 INPUT "Name"; n$')
    c.send_line("20 PRINT n$")
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    first = _wait_contains(c, b"Name?")
    assert b"Name?" in first
    c._ser.sendall(b"Ada\r")
    out = _until_prompt(c)
    assert "Ada" in out

    c.send_line("NEW")
    c.send_line("10 INPUT a, b$")
    c.send_line('20 PRINT STR$(a) + "/" + b$')
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    assert b"?" in _wait_contains(c, b"?")
    c._ser.sendall(b"3,xyz\r")
    out = _until_prompt(c)
    assert "3/xyz" in out


def test_print_semicolon_then_input_shows_print_first(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line('10 PRINT "what is happening?";')
    c.send_line("20 INPUT a$")
    c.send_line('30 PRINT "Oh interesting, I never thought of"; a$')
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    first = _wait_contains(c, b"what is happening??", timeout=8.0)
    assert b"what is happening?" in first, first
    c._ser.sendall(b"bananas\r")
    out = _until_prompt(c, timeout=8.0)
    assert "Oh interesting, I never thought ofbananas" in out.replace("\r", "")


def test_print_then_line_input_shows_print_first(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line('10 PRINT "ahead";')
    c.send_line('20 LINE INPUT "Go"; s$')
    c.send_line("30 PRINT s$")
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    first = _wait_contains(c, b"aheadGo", timeout=8.0)
    assert b"aheadGo" in first, first
    assert first.find(b"ahead") <= first.find(b"Go")
    c._ser.sendall(b"xyz\r")
    out = _until_prompt(c, timeout=8.0)
    assert "xyz" in out


def test_line_input_console(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line('10 LINE INPUT "Go"; s$')
    c.send_line("20 PRINT s$")
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    assert b"Go" in _wait_contains(c, b"Go")
    c._ser.sendall(b"abc def\r")
    out = _until_prompt(c)
    assert "abc def" in out


from ihelp_util import dump_topic


def test_help_input_describes_console(console):
    out = dump_topic(console, "INPUT")
    assert "not implemented" not in out.lower()
    assert "prompt" in out.lower()
    assert "INPUT" in out


# ---- KEYDOWN contract (host unit test) ------------------------------------
#
# CMM2 contract (restored in #1250): KEYDOWN(0) is the held-key count and
# KEYDOWN(n>0) is the code of the nth held key. Compile the real
# mmbasic/src/util.c so the implementation, not a copy, stays pinned.

KEYDOWN_DRIVER_C = r"""
#include "mmb_priv.h"
#include <stdio.h>

mmb g_state;
mmb *g_cur = &g_state;

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
	int none[1] = { 0 };
	int left[1] = { 130 };
	int right_esc[2] = { 131, 27 };
	int i, paddle = 100, quit = 0;

	mmb_keydown_set(none, 0);
	check(mmb_keydown_get(0) == 0, "empty count");
	check(mmb_keydown_get(1) == 0, "empty index");

	/* sprites.bas style: iterate the held-key list from KEYDOWN(0). */
	mmb_keydown_set(left, 1);
	check(mmb_keydown_get(0) == 1, "left count");
	check(mmb_keydown_get(1) == 130, "left code");
	check(mmb_keydown_get(2) == 0, "list ends after one key");
	for (i = 1; i <= mmb_keydown_get(0); i++)
		if (mmb_keydown_get(i) == 130)
			paddle -= 3;
	check(paddle == 97, "paddle moved left");

	/* Holding right and Escape must move the paddle and quit. */
	mmb_keydown_set(right_esc, 2);
	check(mmb_keydown_get(0) == 2, "two held");
	check(mmb_keydown_get(1) == 131, "right code");
	check(mmb_keydown_get(2) == 27, "escape code");
	for (i = 1; i <= mmb_keydown_get(0); i++)
	{
		if (mmb_keydown_get(i) == 131)
			paddle += 3;
		if (mmb_keydown_get(i) == 27)
			quit = 1;
	}
	check(paddle == 100, "paddle moved right");
	check(quit == 1, "escape quits");

	printf("%s\n", fails ? "FAILURES" : "ALL OK");
	return fails ? 1 : 0;
}
"""


@pytest.fixture(scope="module")
def keydown_host_run(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tmp = tmp_path_factory.mktemp("keydown_contract")
    (tmp / "mmb_version.h").write_text('#define MMB_VERSION "test"\n')
    (tmp / "driver.c").write_text(KEYDOWN_DRIVER_C)
    exe = tmp / "driver"
    gcflags = ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra",
            "-DMMB_PLATFORM_POSIX",
            "-ffunction-sections", "-fdata-sections",
            "-I", str(tmp),
            "-I", os.path.join(repo, "mmbasic", "include"),
            "-o", str(exe),
            str(tmp / "driver.c"),
            os.path.join(repo, "mmbasic", "src", "util.c"),
            *gcflags,
        ],
        check=True,
        cwd=repo,
    )
    return subprocess.run([str(exe)], check=False, capture_output=True, text=True)


def test_keydown_list_contract(keydown_host_run):
    out = keydown_host_run.stdout + keydown_host_run.stderr
    assert keydown_host_run.returncode == 0, out
    assert "ALL OK" in out, out
    assert "FAIL" not in out, out

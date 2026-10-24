"""Growable program storage (#1303).

The per-console interpreter used to cap a program at MMB_MAX_LINES (2048)
lines, 64 labels, 128 SUBs and a 16 KB EDIT buffer. These tests generate
programs larger than every one of those limits in the guest and check that
they load, run, LIST, SAVE/LOAD and EDIT without truncation.

Large files are produced by small BASIC generator programs that run inside
the guest: typing thousands of lines over the serial socket would dominate
the test time.
"""

import time

from harness import MMBasicConsole


def _write_bas(console: MMBasicConsole, path: str, lines: list[str]) -> None:
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def test_10k_line_program_runs_with_goto_gosub_labels(console):
    """A 10,008-line program loads and runs; GOSUB to a label, RESTORE to a
    label, GOTO forward, and DATA/READ all land near the end of the store."""
    gen = [
        'OPEN "BIG10K.BAS" FOR OUTPUT AS #1',
        'PRINT #1, "10 S=0"',
        "FOR I=20 TO 99950 STEP 10",
        'PRINT #1, STR$(I)+" S=S+1"',
        "NEXT I",
        'PRINT #1, "99960 GOSUB ENDLABEL"',
        'PRINT #1, "99970 RESTORE DATALBL"',
        'PRINT #1, "99980 READ X"',
        'PRINT #1, "99990 PRINT ""DONE"";S+X"',
        'PRINT #1, "100000 GOTO FINISH"',
        'PRINT #1, "100010 ENDLABEL:"',
        'PRINT #1, "100020 S=S+1"',
        'PRINT #1, "100030 RETURN"',
        'PRINT #1, "100040 DATALBL:"',
        'PRINT #1, "100050 DATA 5"',
        'PRINT #1, "100060 FINISH:"',
        'PRINT #1, "100070 PRINT ""END"""',
        'PRINT #1, "100080 END"',
        "CLOSE #1",
    ]
    _write_bas(console, "GEN10K.BAS", gen)
    assert console.send_line('RUN "GEN10K.BAS"', timeout=120) == ""
    start = time.monotonic()
    out = console.send_line('RUN "BIG10K.BAS"', timeout=120)
    elapsed = time.monotonic() - start
    assert out == "DONE10000\nEND", out
    # A quadratic insert would take minutes at 10k lines; loading and running
    # the whole program must stay comfortably linear (a generous CI bound).
    assert elapsed < 60, f"10k-line load+run took {elapsed:.1f}s"


def test_include_files_over_2048_lines(console):
    """A main file plus 3 x 800-line #INCLUDE files (2,403 lines) runs."""
    gen = [
        "N=1",
        "WHILE N<=3",
        'P$="PART"+LTRIM$(STR$(N))+".INC"',
        "OPEN P$ FOR OUTPUT AS #2",
        "FOR I=1 TO 800",
        'PRINT #2, "S=S+1"',
        "NEXT I",
        "CLOSE #2",
        "N=N+1",
        "WEND",
    ]
    _write_bas(console, "GENINC.BAS", gen)
    assert console.send_line('RUN "GENINC.BAS"', timeout=120) == ""
    _write_bas(
        console,
        "MAIN.BAS",
        [
            "S=0",
            '#INCLUDE "PART1.INC"',
            '#INCLUDE "PART2.INC"',
            '#INCLUDE "PART3.INC"',
            'PRINT "TOTAL";S',
        ],
    )
    assert console.send_line('RUN "MAIN.BAS"', timeout=120) == "TOTAL2400"


def test_large_program_list_and_save_load_roundtrip(console):
    """LIST prints a 3,000-line program in full; SAVE then RUN of the saved
    copy produces the same result."""
    fillers = 2998  # lines 20..29990 step 10
    gen = [
        'OPEN "L3K.BAS" FOR OUTPUT AS #1',
        'PRINT #1, "10 S=0"',
        "FOR I=20 TO 29990 STEP 10",
        'PRINT #1, STR$(I)+" S=S+1"',
        "NEXT I",
        'PRINT #1, "30000 PRINT ""TAIL"";S"',
        "CLOSE #1",
    ]
    _write_bas(console, "GEN3K.BAS", gen)
    assert console.send_line('RUN "GEN3K.BAS"', timeout=120) == ""
    # RUN loads L3K.BAS into memory (and exercises the 3,000-line program).
    assert console.send_line('RUN "L3K.BAS"', timeout=120) == f"TAIL{fillers}"
    out = console.send_line("LIST", timeout=120)
    assert out.startswith("10 S=0"), out[:80]
    assert '30000 PRINT "TAIL";S' in out
    assert console.send_line('SAVE "L3K2.BAS"', timeout=120) == ""
    assert console.send_line("NEW") == ""
    assert console.send_line('RUN "L3K2.BAS"', timeout=120) == f"TAIL{fillers}"
    # The reloaded program also survives a re-run from memory.
    assert console.send_line("RUN", timeout=120) == f"TAIL{fillers}"


def test_over_64_labels(console):
    """100 labels register and GOTO finds the last one (no silent drop)."""
    lines = ["10 GOTO L100"]
    for i in range(1, 101):
        lines.append(f"{i * 10 + 100} L{i}: S=S+1")
    lines += ['1200 PRINT "LBL";S', "1210 END"]
    _write_bas(console, "LBL100.BAS", lines)
    assert console.send_line('RUN "LBL100.BAS"', timeout=60) == "LBL1"


def test_over_128_subs(console):
    """200 SUB definitions register and CALL reaches the last one."""
    gen = [
        'OPEN "SUBS.BAS" FOR OUTPUT AS #1',
        'PRINT #1, "10 CALL S200"',
        'PRINT #1, "20 PRINT ""SUBOK"""',
        'PRINT #1, "30 END"',
        "FOR I=1 TO 200",
        'PRINT #1, STR$(100+I*100)+" SUB S"+LTRIM$(STR$(I))+"()"',
        'PRINT #1, STR$(110+I*100)+" END SUB"',
        "NEXT I",
        "CLOSE #1",
    ]
    _write_bas(console, "GENSUBS.BAS", gen)
    assert console.send_line('RUN "GENSUBS.BAS"', timeout=120) == ""
    assert console.send_line('RUN "SUBS.BAS"', timeout=120) == "SUBOK"


def test_edit_open_save_over_16kb(console):
    """EDIT opens a >16 KB file, Ctrl+S saves it and the tail survives (the
    old fixed 16 KB buffer truncated the file on load)."""
    gen = [
        'OPEN "BIGED.BAS" FOR OUTPUT AS #1',
        "FOR I=1 TO 800",
        'PRINT #1, STR$(I*10)+" S=S+1 \'padding padding pad"',
        "NEXT I",
        'PRINT #1, "9999 ENDMARK"',
        "CLOSE #1",
    ]
    _write_bas(console, "GENED.BAS", gen)
    assert console.send_line('RUN "GENED.BAS"', timeout=120) == ""
    # Raw serial for the full-screen editor (Alt+X quits; Ctrl+S saves).
    console.drain(quiet=0.1)
    console._ser.sendall(b'EDIT "BIGED.BAS"\r')
    console.drain(quiet=0.8)
    console._ser.sendall(bytes([19]))  # Ctrl+S: save
    console.drain(quiet=0.5)
    console._ser.sendall(bytes([1]) + b"x")  # Alt+X: quit
    console.drain(quiet=0.8)
    # Read the file back: 801 lines, last line still "9999 ENDMARK".
    checker = [
        'OPEN "BIGED.BAS" FOR INPUT AS #1',
        "N=0",
        "DO",
        "LINE INPUT #1, A$",
        "N=N+1",
        "LOOP UNTIL EOF(1)",
        "CLOSE #1",
        'PRINT N; " "; A$',
    ]
    _write_bas(console, "CHKED.BAS", checker)
    assert console.send_line('RUN "CHKED.BAS"', timeout=60) == "801 9999 ENDMARK"


def test_small_program_no_regression(console):
    """A tiny program still loads and runs instantly (sanity check)."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 A=1") == ""
    assert console.send_line("20 FOR I=1 TO 3") == ""
    assert console.send_line("30 A=A*2") == ""
    assert console.send_line("40 NEXT I") == ""
    assert console.send_line('50 PRINT "OK";A') == ""
    start = time.monotonic()
    assert console.send_line("RUN") == "OK8"
    assert time.monotonic() - start < 10

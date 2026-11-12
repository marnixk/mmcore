"""#1418: the SUB/FUNCTION name index must match the old linear scan.

sub_find() now probes a case-insensitive hash table that is rebuilt as
definitions are registered and cleared with the SUB table on NEW/RUN.  These
tests pin the semantics the linear scan had: case-insensitive calls, variables
still missing, duplicate definitions rejected, a fresh hash after NEW/RUN, and
a table that grows past its initial capacity.
"""


def _run_program(console, program):
    assert console.send_line("NEW") == ""
    for line in program:
        assert console.send_line(line) == ""
    out = console.send_line("RUN")
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


def _load(console, program):
    """Enter a long program in serial chunks (send_line is ~0.25 s each)."""
    assert console.send_line("NEW") == ""
    chunk = []
    size = 0
    for line in program:
        cost = len(line) + 1
        if chunk and size + cost > 400:
            console._ser.sendall(("\r".join(chunk) + "\r").encode())
            console.drain(quiet=0.15, timeout=10.0)
            chunk, size = [], 0
        chunk.append(line)
        size += cost
    if chunk:
        console._ser.sendall(("\r".join(chunk) + "\r").encode())
    console.drain(quiet=0.4, timeout=20.0)


def test_sub_function_lookup_is_case_insensitive(console):
    lines = _run_program(console, [
        "10 FUNCTION AdD1(N)",
        "20 AdD1 = N + 1",
        "30 END FUNCTION",
        "40 SUB DoShow(X)",
        '50 PRINT "S";X',
        "60 END SUB",
        '70 PRINT "F";aDd1(4)',
        "80 dOsHoW(7)",
    ])
    assert lines == ["F5", "S7"], lines


def test_variable_names_still_miss_the_sub_index(console):
    """A variable primary must not resolve as a user FUNCTION/SUB."""
    lines = _run_program(console, [
        "10 X = 3",
        "20 PRINT X",
        "30 FUNCTION F(N)",
        "40 F = N * 2",
        "50 END FUNCTION",
        "60 PRINT f(4)",
    ])
    assert lines == ["3", "8"], lines


def test_duplicate_definition_across_case_still_conflicts(console):
    lines = _run_program(console, [
        "10 SUB Mix()",
        "20 END SUB",
        "30 SUB mIX()",
        "40 END SUB",
    ])
    assert any("NAME CONFLICT" in ln.upper() for ln in lines), lines


def test_new_and_run_rebuild_the_name_index(console):
    lines = _run_program(console, [
        "10 FUNCTION F(N)",
        "20 F = N * 3",
        "30 END FUNCTION",
        "40 PRINT f(2)",
    ])
    assert lines == ["6"], lines
    # A second RUN clears G.subs and rescans; the stale hash must not answer.
    out = console.send_line("RUN")
    assert [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()] == ["6"]
    # NEW drops the definition; a call afterwards must not find it.
    assert console.send_line("NEW") == ""
    console.send_line("x = f(2)")
    out = console.send_line('PRINT "ALIVE"')
    assert "ALIVE" in out, out


def test_index_grows_past_initial_sub_table(console):
    """129+ definitions grow G.subs; the index must be rebuilt, not stale."""
    n = 200
    program = ["10 G = 0"]
    for i in range(n):
        program.append(f"{20 + i} S{i:03d}")
    program += [f"{20 + n} PRINT G", f"{30 + n} END"]
    base = 1000
    for i in range(n):
        program += [
            f"{base} SUB S{i:03d}",
            f"{base + 1} G = G + 1",
            f"{base + 2} END SUB",
        ]
        base += 10
    _load(console, program)
    out = console.send_line("RUN", timeout=60)
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == [str(n)], lines

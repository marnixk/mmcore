"""PicoMite ARRAY SET / ADD / INSERT / SLICE."""

from ihelp_util import dump_topic


def test_array_set_numeric_and_string(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM A(4)") == ""
    assert console.send_line("ARRAY SET 7, A()") == ""
    assert console.send_line("PRINT A(0); A(1); A(2); A(3); A(4)") == "77777"
    assert console.send_line("DIM S$(2)") == ""
    assert console.send_line('ARRAY SET "x", S$()') == ""
    assert console.send_line("PRINT S$(0); S$(1); S$(2)") == "xxx"


def test_array_add_numeric(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM A(2), B(2)") == ""
    assert console.send_line("A(0) = 1 : A(1) = 2 : A(2) = 3") == ""
    assert console.send_line("ARRAY ADD A(), 10, B()") == ""
    assert console.send_line("PRINT B(0); B(1); B(2)") == "111213"
    assert console.send_line("ARRAY ADD A(), 0, B()") == ""
    assert console.send_line("PRINT B(0); B(1); B(2)") == "123"


def test_array_add_strings(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM S$(1), T$(1)") == ""
    assert console.send_line('S$(0) = "a" : S$(1) = "b"') == ""
    assert console.send_line('ARRAY ADD S$(), "!", T$()') == ""
    assert console.send_line("PRINT T$(0); T$(1)") == "a!b!"


def test_array_add_size_mismatch(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM A(2), C(3)") == ""
    err = console.send_line("ARRAY ADD A(), 1, C()")
    assert "SIZE MISMATCH" in err.upper()


def test_array_slice_last_dim(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM M(1, 2)") == ""
    assert console.send_line("M(0,0)=1 : M(0,1)=2 : M(0,2)=3") == ""
    assert console.send_line("M(1,0)=4 : M(1,1)=5 : M(1,2)=6") == ""
    assert console.send_line("DIM R(2)") == ""
    assert console.send_line("ARRAY SLICE M(), 0, , R()") == ""
    assert console.send_line("PRINT R(0); R(1); R(2)") == "123"
    assert console.send_line("ARRAY SLICE M(), 1, , R()") == ""
    assert console.send_line("PRINT R(0); R(1); R(2)") == "456"


def test_array_slice_first_dim(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM M(1, 2)") == ""
    assert console.send_line("M(0,1)=12 : M(1,1)=22") == ""
    assert console.send_line("DIM R(1)") == ""
    assert console.send_line("ARRAY SLICE M(), , 1, R()") == ""
    assert console.send_line("PRINT R(0); R(1)") == "1222"


def test_array_slice_strings(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM M$(1, 1)") == ""
    assert console.send_line('M$(0,0)="a" : M$(0,1)="b" : M$(1,0)="c" : M$(1,1)="d"') == ""
    assert console.send_line("DIM R$(1)") == ""
    assert console.send_line("ARRAY SLICE M$(), 1, , R$()") == ""
    assert console.send_line("PRINT R$(0); R$(1)") == "cd"


def test_array_insert(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM M(1, 2)") == ""
    assert console.send_line("DIM R(2)") == ""
    assert console.send_line("R(0)=7 : R(1)=8 : R(2)=9") == ""
    assert console.send_line("ARRAY INSERT M(), 1, , R()") == ""
    assert console.send_line("PRINT M(1,0); M(1,1); M(1,2)") == "789"
    assert console.send_line("PRINT M(0,0); M(0,1); M(0,2)") == "000"


def test_array_slice_requires_one_omitted(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM M(1, 2)") == ""
    assert console.send_line("DIM R(2)") == ""
    err = console.send_line("ARRAY SLICE M(), 0, 1, R()")
    assert "SLICE" in err.upper()


def test_help_array(console):
    out = dump_topic(console, "ARRAY")
    assert "ARRAY SET" in out
    assert "SLICE" in out
    assert "INSERT" in out

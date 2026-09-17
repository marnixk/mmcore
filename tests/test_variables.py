"""QuickBasic/PicoMite variable and type gaps: SWAP, REDIM [PRESERVE],
DIM SHARED, COMMON."""

from ihelp_util import dump_topic


def test_swap_numbers_and_strings(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("A = 1 : B = 2") == ""
    assert console.send_line("SWAP A, B") == ""
    assert console.send_line("PRINT A; B") == "21"
    assert console.send_line('S$ = "L" : T$ = "R"') == ""
    assert console.send_line("SWAP S$, T$") == ""
    assert console.send_line("PRINT S$; T$") == "RL"


def test_swap_array_elements(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM N$(1)") == ""
    assert console.send_line('N$(0) = "left"') == ""
    assert console.send_line('N$(1) = "right"') == ""
    assert console.send_line("SWAP N$(0), N$(1)") == ""
    assert console.send_line("PRINT N$(0); N$(1)") == "rightleft"


def test_swap_type_mismatch(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('A = 1 : B$ = "x"') == ""
    err = console.send_line("SWAP A, B$")
    assert "TYPE MISMATCH" in err.upper()


def test_redim_discards_without_preserve(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM A(2)") == ""
    assert console.send_line("A(0) = 5") == ""
    assert console.send_line("REDIM A(4)") == ""
    assert console.send_line("PRINT A(0)") == "0"
    assert console.send_line("PRINT A(4)") == "0"


def test_redim_creates_new_array(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("REDIM B(3)") == ""
    assert console.send_line("B(2) = 9") == ""
    assert console.send_line("PRINT B(2)") == "9"


def test_redim_preserve_numeric(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM A(2)") == ""
    assert console.send_line("A(0) = 1 : A(1) = 2 : A(2) = 3") == ""
    assert console.send_line("REDIM PRESERVE A(4)") == ""
    assert console.send_line("PRINT A(0) + A(1) + A(2)") == "6"
    assert console.send_line("PRINT A(3); A(4)") == "00"
    assert console.send_line("REDIM PRESERVE A(1)") == ""
    assert console.send_line("PRINT A(0); A(1)") == "12"
    assert console.send_line("PRINT BOUND(A())") == "1"


def test_redim_preserve_strings(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM S$(2)") == ""
    assert console.send_line('S$(0) = "a" : S$(1) = "bb" : S$(2) = "ccc"') == ""
    assert console.send_line("REDIM PRESERVE S$(4)") == ""
    assert console.send_line("PRINT S$(0); S$(1); S$(2)") == "abbccc"
    assert console.send_line('PRINT "["; S$(4); "]"') == "[]"


def test_redim_preserve_2d_last_dim(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM M(1, 2)") == ""
    assert console.send_line("M(0, 0) = 1 : M(0, 1) = 2 : M(0, 2) = 3") == ""
    assert console.send_line("M(1, 0) = 4 : M(1, 1) = 5 : M(1, 2) = 6") == ""
    assert console.send_line("REDIM PRESERVE M(1, 4)") == ""
    assert console.send_line("PRINT M(0, 0); M(0, 1); M(0, 2); M(1, 0); M(1, 3)") == "12340"


def test_redim_preserve_other_dim_errors(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM M(1, 2)") == ""
    err = console.send_line("REDIM PRESERVE M(3, 2)")
    assert "DIMENSION" in err.upper()


def test_dim_and_redim_shared(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM SHARED X(2)") == ""
    assert console.send_line("X(0) = 7") == ""
    assert console.send_line("PRINT X(0)") == "7"
    assert console.send_line("REDIM SHARED Y(3)") == ""
    assert console.send_line("Y(3) = 8") == ""
    assert console.send_line("PRINT Y(3)") == "8"


def test_common_declares_variables(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("COMMON FOO, BAR") == ""
    assert console.send_line("FOO = 5 : BAR$ = \"hi\"") == ""
    assert console.send_line("PRINT FOO") == "5"
    assert console.send_line("PRINT BAR$") == "hi"
    assert console.send_line("COMMON SHARED BAZ") == ""
    assert console.send_line("BAZ = 3") == ""
    assert console.send_line("PRINT BAZ") == "3"


def test_help_variable_gaps(console):
    out = dump_topic(console, "REDIM")
    assert "PRESERVE" in out
    out = dump_topic(console, "SWAP")
    assert "SWAP" in out
    out = dump_topic(console, "COMMON")
    assert "COMMON" in out
    dim = dump_topic(console, "DIM")
    assert "SHARED" in dim

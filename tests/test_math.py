"""CMM2 CINT, EVAL, MATH() and MATH command."""

from ihelp_util import dump_topic


def test_cint_round_half_away(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("PRINT CINT(45.47)") == "45"
    assert console.send_line("PRINT CINT(45.57)") == "46"
    assert console.send_line("PRINT CINT(-1.5)") == "-2"
    assert console.send_line("PRINT CINT(1.5)") == "2"
    assert console.send_line("PRINT INT(-1.5)") == "-2"
    assert console.send_line("PRINT FIX(-1.5)") == "-1"


def test_eval_expression_string(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("PRINT EVAL(\"1+2*3\")") == "7"
    assert console.send_line("S$=\"COS(0)*100\"") == ""
    assert console.send_line("PRINT EVAL(S$)") == "100"


def test_math_hyperbolic_log10_atan3(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("PRINT MATH(SINH 0)") == "0"
    assert console.send_line("PRINT MATH(COSH 0)") == "1"
    assert console.send_line("PRINT MATH(TANH 0)") == "0"
    assert console.send_line("PRINT MATH(LOG10 100)") == "2"
    assert console.send_line("OPTION ANGLE DEGREES") == ""
    assert console.send_line("PRINT CINT(MATH(ATAN3 0, 1))") == "90"
    assert console.send_line("PRINT CINT(MATH(ATAN3 1, 0))") == "0"


def test_math_stats_and_vector(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM A(2)") == ""
    assert console.send_line("A(0)=1") == ""
    assert console.send_line("A(1)=2") == ""
    assert console.send_line("A(2)=3") == ""
    assert console.send_line("PRINT MATH(SUM A())") == "6"
    assert console.send_line("PRINT MATH(MEAN A())") == "2"
    assert console.send_line("PRINT MATH(MAX A())") == "3"
    assert console.send_line("PRINT MATH(MIN A())") == "1"
    assert console.send_line("PRINT MATH(MEDIAN A())") == "2"
    assert console.send_line("PRINT MATH(SD A())") == "1"
    assert console.send_line("DIM B(3)") == ""
    assert console.send_line("B(0)=1") == ""
    assert console.send_line("B(1)=3") == ""
    assert console.send_line("B(2)=2") == ""
    assert console.send_line("B(3)=4") == ""
    assert console.send_line("PRINT MATH(MEDIAN B())") == "2.5"
    mag = console.send_line("PRINT CINT(MATH(MAGNITUDE A())*1000)")
    assert mag == "3742"
    assert console.send_line("DIM C(2)") == ""
    assert console.send_line("C(0)=4") == ""
    assert console.send_line("C(1)=5") == ""
    assert console.send_line("C(2)=6") == ""
    assert console.send_line("PRINT MATH(DOTPRODUCT A(), C())") == "32"


def test_math_set_scale_add_interpolate(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM A(2)") == ""
    assert console.send_line("MATH SET 3, A()") == ""
    assert console.send_line("PRINT A(0)+A(1)+A(2)") == "9"
    assert console.send_line("MATH SCALE A(), 2, A()") == ""
    assert console.send_line("PRINT A(1)") == "6"
    assert console.send_line("MATH ADD A(), 4, A()") == ""
    assert console.send_line("PRINT A(2)") == "10"
    assert console.send_line("DIM B(2)") == ""
    assert console.send_line("DIM C(2)") == ""
    assert console.send_line("MATH SET 0, B()") == ""
    assert console.send_line("MATH SET 10, C()") == ""
    assert console.send_line("DIM D(2)") == ""
    assert console.send_line("MATH INTERPOLATE B(), C(), 0.5, D()") == ""
    assert console.send_line("PRINT D(0)") == "5"


def test_math_matrix(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM M(1,1)") == ""
    assert console.send_line("M(0,0)=1") == ""
    assert console.send_line("M(1,0)=2") == ""
    assert console.send_line("M(0,1)=3") == ""
    assert console.send_line("M(1,1)=4") == ""
    assert console.send_line("PRINT MATH(M_DETERMINANT M())") == "-2"
    assert console.send_line("DIM T(1,1)") == ""
    assert console.send_line("MATH M_TRANSPOSE M(), T()") == ""
    assert console.send_line("PRINT T(0,1)") == "2"
    assert console.send_line("PRINT T(1,0)") == "3"
    assert console.send_line("DIM A(1,1)") == ""
    assert console.send_line("DIM B(1,1)") == ""
    assert console.send_line("DIM C(1,1)") == ""
    assert console.send_line("A(0,0)=1") == ""
    assert console.send_line("A(1,0)=2") == ""
    assert console.send_line("A(0,1)=3") == ""
    assert console.send_line("A(1,1)=4") == ""
    assert console.send_line("B(0,0)=5") == ""
    assert console.send_line("B(1,0)=6") == ""
    assert console.send_line("B(0,1)=7") == ""
    assert console.send_line("B(1,1)=8") == ""
    assert console.send_line("MATH M_MULT A(), B(), C()") == ""
    assert console.send_line("PRINT C(0,0)") == "19"
    assert console.send_line("PRINT C(1,0)") == "22"
    assert console.send_line("PRINT C(0,1)") == "43"
    assert console.send_line("PRINT C(1,1)") == "50"
    assert console.send_line("DIM I(1,1)") == ""
    assert console.send_line("DIM INV(1,1)") == ""
    assert console.send_line("I(0,0)=1") == ""
    assert console.send_line("I(1,0)=0") == ""
    assert console.send_line("I(0,1)=0") == ""
    assert console.send_line("I(1,1)=2") == ""
    assert console.send_line("MATH M_INVERSE I(), INV()") == ""
    assert console.send_line("PRINT INV(0,0)") == "1"
    assert console.send_line("PRINT INV(1,1)") == "0.5"


def test_math_vectors_slice_fft_quat(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM U(2)") == ""
    assert console.send_line("DIM V(2)") == ""
    assert console.send_line("DIM W(2)") == ""
    assert console.send_line("U(0)=1") == ""
    assert console.send_line("U(1)=0") == ""
    assert console.send_line("U(2)=0") == ""
    assert console.send_line("V(0)=0") == ""
    assert console.send_line("V(1)=1") == ""
    assert console.send_line("V(2)=0") == ""
    assert console.send_line("MATH V_CROSS U(), V(), W()") == ""
    assert console.send_line("PRINT W(0);W(1);W(2)") == "001"
    assert console.send_line("DIM N(2)") == ""
    assert console.send_line("N(0)=3") == ""
    assert console.send_line("N(1)=0") == ""
    assert console.send_line("N(2)=4") == ""
    assert console.send_line("MATH V_NORMALISE N(), N()") == ""
    assert console.send_line("PRINT CINT(N(0)*10)") == "6"
    assert console.send_line("PRINT CINT(N(2)*10)") == "8"
    assert console.send_line("DIM P(1,2)") == ""
    assert console.send_line("P(0,0)=10") == ""
    assert console.send_line("P(0,1)=20") == ""
    assert console.send_line("P(0,2)=30") == ""
    assert console.send_line("DIM S(2)") == ""
    assert console.send_line("MATH SLICE P(), 0, , S()") == ""
    assert console.send_line("PRINT S(0)+S(1)+S(2)") == "60"
    assert console.send_line("DIM Q(4)") == ""
    assert console.send_line("MATH Q_VECTOR 3, 0, 4, Q()") == ""
    assert console.send_line("PRINT Q(4)") == "5"
    assert console.send_line("PRINT CINT(Q(1)*10)") == "6"
    assert console.send_line("DIM X(3)") == ""
    assert console.send_line("DIM Y(3)") == ""
    assert console.send_line("MATH SET 1, X()") == ""
    assert console.send_line("MATH FFT MAGNITUDE X(), Y()") == ""
    assert console.send_line("PRINT Y(0)") == "4"


def test_math_chi_correl(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM T(1,1)") == ""
    assert console.send_line("T(0,0)=10") == ""
    assert console.send_line("T(1,0)=10") == ""
    assert console.send_line("T(0,1)=10") == ""
    assert console.send_line("T(1,1)=10") == ""
    chi = console.send_line("PRINT MATH(CHI T())")
    assert chi == "0"
    assert console.send_line("DIM X(2)") == ""
    assert console.send_line("DIM Y(2)") == ""
    assert console.send_line("X(0)=1") == ""
    assert console.send_line("X(1)=2") == ""
    assert console.send_line("X(2)=3") == ""
    assert console.send_line("Y(0)=2") == ""
    assert console.send_line("Y(1)=4") == ""
    assert console.send_line("Y(2)=6") == ""
    assert console.send_line("PRINT MATH(CORREL X(), Y())") == "1"


def test_math_array_add_mul(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM A(2)") == ""
    assert console.send_line("DIM B(2)") == ""
    assert console.send_line("DIM C(2)") == ""
    assert console.send_line("A(0)=1") == ""
    assert console.send_line("A(1)=2") == ""
    assert console.send_line("A(2)=3") == ""
    assert console.send_line("B(0)=10") == ""
    assert console.send_line("B(1)=20") == ""
    assert console.send_line("B(2)=30") == ""
    assert console.send_line("MATH ADD A(), B(), C()") == ""
    assert console.send_line("PRINT C(0);C(1);C(2)") == "112233"
    assert console.send_line("MATH MUL A(), B(), C()") == ""
    assert console.send_line("PRINT C(0);C(1);C(2)") == "104090"
    assert console.send_line("MATH SCALE A(), B(), C()") == ""
    assert console.send_line("PRINT C(0);C(1);C(2)") == "104090"
    assert console.send_line("MATH ADD A(), 5, C()") == ""
    assert console.send_line("PRINT C(2)") == "8"
    assert console.send_line("MATH SCALE A(), 4, C()") == ""
    assert console.send_line("PRINT C(1)") == "8"
    assert console.send_line("MATH MUL A(), 4, C()") == ""
    assert console.send_line("PRINT C(1)") == "8"
    assert console.send_line("MATH ADD A(), COS(0), C()") == ""
    assert console.send_line("PRINT C(0)") == "2"
    assert console.send_line("DIM D(3)") == ""
    err = console.send_line("MATH ADD A(), D(), C()")
    assert "SIZE MISMATCH" in err.upper()
    err = console.send_line("MATH MUL A(), D(), C()")
    assert "SIZE MISMATCH" in err.upper()
    err = console.send_line("MATH SCALE A(), D(), C()")
    assert "SIZE MISMATCH" in err.upper()


def _write_bas(console, path, lines):
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def _run_bas(console, path, lines, timeout=20.0):
    _write_bas(console, path, lines)
    out = console.send_line(f'RUN "{path}"', timeout=timeout)
    parts = [p.strip() for p in out.replace("\r", "\n").split("\n") if p.strip()]
    return parts


def test_math_scale_add_kernel_faster_than_for(console):
    setup = [
        "DIM POS.X(100), SPEED(100), DX(100)",
        "RATIO=1.5",
        "FOR I=0 TO 100",
        "POS.X(I)=I",
        "SPEED(I)=2",
        "NEXT I",
    ]
    for_parts = _run_bas(
        console,
        "MFOR.BAS",
        setup
        + [
            "TIMER=0",
            "FOR F=1 TO 400",
            "FOR I=0 TO 100",
            "POS.X(I)=POS.X(I)+(SPEED(I)*RATIO)",
            "NEXT I",
            "NEXT F",
            "PRINT TIMER",
            "PRINT INT(POS.X(50))",
        ],
    )
    math_parts = _run_bas(
        console,
        "MMATH.BAS",
        setup
        + [
            "TIMER=0",
            "FOR F=1 TO 400",
            "MATH SCALE SPEED(), RATIO, DX()",
            "MATH ADD POS.X(), DX(), POS.X()",
            "NEXT F",
            "PRINT TIMER",
            "PRINT INT(POS.X(50))",
        ],
    )
    for_ms = int(for_parts[0])
    math_ms = int(math_parts[0])
    assert int(math_parts[1]) == int(for_parts[1])
    assert for_ms > 0
    assert math_ms * 2 < for_ms


def test_help_math(console):
    out = dump_topic(console, "MATH")
    assert "CINT" in out
    assert "EVAL" in out
    assert "ATAN3" in out
    assert "MATH SET" in out
    assert "MATH MUL" in out
    assert "FFT" in out
    assert "pos().x" in out or "pts().x" in out
    fn = dump_topic(console, "FUNCTIONS")
    assert "CINT" in fn
    assert "EVAL" in fn
    assert "MATH()" in fn or "<MATH>()" in fn
    basic = dump_topic(console, "CMM2")
    assert "MATH" in basic
    assert "LIBRARY MATH" not in basic

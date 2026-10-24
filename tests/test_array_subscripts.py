"""Array subscripts built from expressions or function calls (#1310).

The trace-cache compiler used to call mmb_find_var() with nidx == 0 on an
array index variable and raise ?ARRAY during compilation, and the implicit
assignment scanner stopped at the zero high byte of 3-byte keyword tokens
(ABS, LEN, ...), so `a(qq(0)) = 5` and `a(ABS(-2)) = 8` failed. Both the
trace-cache ON and OFF paths must accept every shape below.
"""

SETUP = [
    "DIM A(10), QQ(3), B(5,5), S$(5)",
    "QQ(0)=2 : QQ(1)=0 : QQ(2)=3 : QQ(3)=1 : A(2)=42",
]

# Each block assigns through a different subscript shape and prints the value
# that must have landed. The printed lines are also the expected RUN output.
VARIANT_LINES = [
    'A(QQ(0)) = 5',
    'PRINT "V1=";A(2)',
    'LET A(QQ(0)) = 6',
    'PRINT "V2=";A(2)',
    'A(QQ(QQ(1))) = 7',
    'PRINT "V3=";A(2)',
    'B(QQ(0), 1) = 8',
    'PRINT "V4=";B(2,1)',
    'B(1, QQ(0)) = 9',
    'PRINT "V5=";B(1,2)',
    'S$(QQ(0)) = "hi"',
    'PRINT "V6=";S$(2)',
    'A(QQ(0)+1) = 10',
    'PRINT "V7=";A(3)',
    'QQ(QQ(3)) = 11',
    'PRINT "V8=";QQ(1)',
    'X = A(QQ(0))',
    'PRINT "V9=";X',
    'IF A(QQ(0)) = 7 THEN PRINT "V10=ok"',
    'IF QQ(0) = 2 THEN A(QQ(0)) = 12',
    'PRINT "V11=";A(2)',
    'FOR I=1 TO 1',
    'A(QQ(0)) = I',
    'NEXT I',
    'PRINT "V12=";A(2)',
    'A((QQ(0))) = 13',
    'PRINT "V13=";A(2)',
    'PRINT "V14=";A(QQ(0))',
    'A(ABS(-2)) = 14',
    'PRINT "V15=";A(2)',
    'A(LEN("ab")) = 15',
    'PRINT "V16=";A(2)',
    'A(MAX(1,2)) = 16',
    'PRINT "V17=";A(2)',
    'A(INT(2.4)) = 17',
    'PRINT "V18=";A(2)',
    'B(ABS(-1), 1) = 18',
    'PRINT "V19=";B(1,1)',
    'A(1+ABS(-1)) = 19',
    'PRINT "V20=";A(2)',
    'LET A(ABS(-2)) = 20',
    'PRINT "V21=";A(2)',
    'A(SQR(4)) = 21',
    'PRINT "V22=";A(2)',
    'A(1+1) = 22',
    'PRINT "V23=";A(2)',
    'X=2 : A(X*1) = 23',
    'PRINT "V24=";A(2)',
    'X = A(ABS(-2))',
    'PRINT "V25=";X',
    'B(SQR(4), 1) = 24',
    'PRINT "V26=";B(2,1)',
    'B(1, SQR(4)) = 25',
    'PRINT "V27=";B(1,2)',
    'X = A(SQR(4))',
    'PRINT "V28=";X',
]

EXPECTED = [
    "V1=5",
    "V2=6",
    "V3=7",
    "V4=8",
    "V5=9",
    "V6=hi",
    "V7=10",
    "V8=11",
    "V9=7",
    "V10=ok",
    "V11=12",
    "V12=1",
    "V13=13",
    "V14=13",
    "V15=14",
    "V16=15",
    "V17=16",
    "V18=17",
    "V19=18",
    "V20=19",
    "V21=20",
    "V22=21",
    "V23=22",
    "V24=23",
    "V25=23",
    "V26=24",
    "V27=25",
    "V28=23",
]


def _write_bas(console, path, lines):
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def _output_lines(out):
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


def test_array_expression_subscripts_tracecache(console):
    lines = SETUP + VARIANT_LINES
    assert console.send_line("NEW") == ""

    assert console.send_line("OPTION TRACECACHE OFF") == ""
    _write_bas(console, "TCASOFF.BAS", lines)
    off = _output_lines(console.send_line('RUN "TCASOFF.BAS"'))
    assert off == EXPECTED

    assert console.send_line("OPTION TRACECACHE ON") == ""
    _write_bas(console, "TCASON.BAS", lines)
    on = _output_lines(console.send_line('RUN "TCASON.BAS"'))
    assert on == EXPECTED

"""Bitwise AND / OR / XOR / INV, logical NOT, and shifts (#1311)."""

import pytest

CASES = [
    ("13 XOR 7", "10"),
    ("12 AND 10", "8"),
    ("12 OR 3", "15"),
    ("12345678901 AND 255", "53"),
    ("&HFF00 AND &H0FF0", "3840"),
    ("&HF0 OR &H0F", "255"),
    ("&HFF XOR &H0F", "240"),
    ("-1 AND 255", "255"),
    ("-8 OR 3", "-5"),
    ("6.7 AND 3", "3"),
    ("&H7FFFFFFFFFFFFFFF AND &HF", "15"),
    ("&H100000000 OR 1", "4294967297"),
    ("2 AND 1", "0"),
    ("1 AND 1", "1"),
    ("0 OR 0", "0"),
    ("(3>2) AND (5>4)", "1"),
    ("3>2 AND 5>4", "1"),
    ("7 AND 3 OR 8", "11"),
]


@pytest.mark.parametrize("expr,expected", CASES)
def test_bitwise_values(console, expr, expected):
    assert console.send_line("NEW") == ""
    assert console.send_line("PRINT " + expr) == expected


def test_bitwise_typed_variables(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("x% = 13") == ""
    assert console.send_line("y% = 7") == ""
    assert console.send_line("PRINT x% XOR y%") == "10"
    assert console.send_line("PRINT x% AND y%") == "5"
    assert console.send_line("PRINT x% OR y%") == "15"


def test_not_stays_logical(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("PRINT NOT 0") == "1"
    assert console.send_line("PRINT NOT 5") == "0"
    assert console.send_line("PRINT NOT -1") == "0"
    assert console.send_line("PRINT NOT (1=1)") == "0"


def test_inv_bitwise_not(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("PRINT INV 5") == "-6"
    assert console.send_line("PRINT INV 0") == "-1"
    assert console.send_line("PRINT INV -1") == "0"
    assert console.send_line("PRINT INV INV 5") == "5"


def test_keyword_suffix_names_still_variables(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("INV% = 5") == ""
    assert console.send_line("PRINT INV%") == "5"
    assert console.send_line("NOT% = 7") == ""
    assert console.send_line("PRINT NOT%") == "7"


def test_comparisons_still_boolean(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("PRINT 3>2") == "1"
    assert console.send_line("PRINT 3<2") == "0"


def test_if_conditions_bitwise(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('IF 2 AND 1 THEN PRINT "T"') == ""
    assert console.send_line('IF 4 AND 4 THEN PRINT "T"') == "T"
    assert console.send_line("k=5") == ""
    assert console.send_line('IF k > 1 AND k < 10 THEN PRINT "T"') == "T"
    assert console.send_line('IF NOT 2 THEN PRINT "T"') == ""


def test_shift_operators(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("PRINT 1 << 4") == "16"
    assert console.send_line("PRINT 256 >> 2") == "64"
    assert console.send_line("PRINT -8 >> 1") == "-4"
    assert console.send_line("PRINT 1 << 63") == "-9223372036854775808"


def test_bitwise_precedence_with_arithmetic(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("PRINT 1 + 2 AND 3") == "3"
    assert console.send_line("PRINT 1 OR 2 + 2") == "5"

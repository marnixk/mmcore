"""OPTION FRAMEPROFILE: CSV is written when RUN finishes (not during the loop)."""

import re

from harness import MMBasicConsole


def _write_bas(console: MMBasicConsole, path: str, lines: list[str]) -> None:
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def test_frame_profile_writes_csv_header(console):
    assert console.send_line("OPTION FRAMEPROFILE ON") == ""
    _write_bas(
        console,
        "FPTEST.BAS",
        [
            "MODE 2,16",
            "PAGE WRITE 1",
            "CLS RGB(0,0,0)",
            "BOX 10,10,40,40,1,RGB(255,0,0),0",
            "PAGE COPY 1 TO 0",
        ],
    )
    out = console.send_line('RUN "FPTEST.BAS"', timeout=12.0)
    assert "?" not in out
    _write_bas(
        console,
        "FPRD.BAS",
        [
            'OPEN "A:/.mmcore-frame-profile.csv" FOR INPUT AS #1',
            "FOR I=1 TO 8",
            "LINE INPUT #1, L$",
            "PRINT L$",
            "NEXT I",
            "CLOSE #1",
        ],
    )
    out2 = console.send_line('RUN "FPRD.BAS"', timeout=8.0)
    assert "mmcore-frame-profile" in out2
    assert re.search(r"index,wall_us", out2)
    assert re.search(r"^0,\d+", out2, re.MULTILINE)

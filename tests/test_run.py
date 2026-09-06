"""Bare RUN reloads the current program file, same as RUN file$."""


def _write_bas(console, path: str, body: str) -> None:
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    assert console.send_line(f'PRINT #1, "{body}"') == ""
    assert console.send_line("CLOSE #1") == ""


def test_run_reloads_current_file(console):
    _write_bas(console, "RELOAD.BAS", "PRINT 111")
    assert console.send_line('RUN "RELOAD.BAS"') == "111"
    _write_bas(console, "RELOAD.BAS", "PRINT 222")
    assert console.send_line("RUN") == "222"


def test_run_after_save_reloads_file(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 PRINT 1") == ""
    assert console.send_line('SAVE "SVRUN.BAS"') == ""
    _write_bas(console, "SVRUN.BAS", "PRINT 2")
    assert console.send_line("RUN") == "2"


def test_run_without_file_uses_memory(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 PRINT 333") == ""
    assert console.send_line("RUN") == "333"

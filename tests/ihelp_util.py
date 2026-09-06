"""Helpers for the IHELP / HELP TUI (serial dump of the offscreen frame)."""


def open_ihelp(con, topic: str | None = None, quiet: float = 0.85) -> str:
    assert con._ser is not None
    con.drain(quiet=0.15)
    cmd = b"IHELP\r" if not topic else f"IHELP {topic}\r".encode()
    con._ser.sendall(cmd)
    return con.drain(quiet=quiet).decode(errors="replace")


def keys(con, data: bytes, quiet: float = 0.4) -> str:
    assert con._ser is not None
    con._ser.sendall(data)
    return con.drain(quiet=quiet).decode(errors="replace")


def scroll_all(con, start: str = "", pages: int = 16) -> str:
    seen = start
    for _ in range(pages):
        seen += keys(con, b"\x1b[6~", quiet=0.32)
    return seen


def close_ihelp(con) -> None:
    keys(con, b"\x03", quiet=0.4)


def dump_topic(con, topic: str | None = None) -> str:
    seen = scroll_all(con, open_ihelp(con, topic))
    close_ihelp(con)
    return seen

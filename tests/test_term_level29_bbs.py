"""Level29 BBS login wire-format checks (live TCP, skipped offline)."""

import socket
import time

import pytest

BBS = ("bbs.fozztexx.com", 23)
USER = b"ireal"
PASS = b"ds9space"


def _recv_all(sock: socket.socket, seconds: float = 2.0) -> bytes:
    sock.settimeout(0.3)
    deadline = time.time() + seconds
    out = b""
    while time.time() < deadline:
        try:
            chunk = sock.recv(8192)
        except socket.timeout:
            continue
        if not chunk:
            break
        out += chunk
    return out


def _announce(sock: socket.socket) -> None:
    sock.sendall(
        bytes(
            [
                255,
                251,
                24,
                255,
                253,
                3,
                255,
                251,
                31,
                255,
                250,
                31,
                0,
                80,
                0,
                32,
                255,
                240,
                255,
                250,
                24,
                0,
                65,
                78,
                83,
                73,
                255,
                240,
            ]
        )
    )
    _recv_all(sock, 1.0)


@pytest.mark.skip(reason="requires live Level29 BBS")
def test_level29_bbs_accepts_cr_only_login():
    """MMBasic char-mode sends CR-only; CRLF after username breaks this BBS."""
    sock = socket.create_connection(BBS, timeout=10)
    try:
        _announce(sock)
        sock.sendall(USER + b"\r")
        mid = _recv_all(sock, 2.0)
        sock.sendall(PASS + b"\r")
        tail = _recv_all(sock, 3.0)
        blob = mid + tail
        assert b"Password:" in blob
        assert b"Invalid user or password" not in blob
        assert b"Welcome ireal!" in blob
    finally:
        sock.close()


@pytest.mark.skip(reason="requires live Level29 BBS")
def test_level29_bbs_rejects_crlf_after_username():
    """Document failure mode seen when line mode sends CRLF instead of CR."""
    sock = socket.create_connection(BBS, timeout=10)
    try:
        _announce(sock)
        sock.sendall(USER + b"\r\n")
        mid = _recv_all(sock, 2.0)
        sock.sendall(PASS + b"\r")
        tail = _recv_all(sock, 3.0)
        blob = mid + tail
        assert b"Invalid user or password" in blob
    finally:
        sock.close()

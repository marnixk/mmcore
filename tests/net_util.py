"""Guest -> host TCP reachability helper for live-network tests.

QEMU user-mode networking normally exposes the host at ``10.0.2.2``. Some
sandboxed/emulated environments hand the guest a DHCP address but cannot
deliver guest->host TCP (``gateway unreachable``); live-network assertions are
skipped there rather than failing.

A host-side ``accept()`` alone is not enough: SLIRP can complete the host half
of the handshake while the guest's ``OPEN`` never becomes usable (the next
``PRINT #9`` reports ``?FILE``). The probe therefore requires the guest to
actually write to the socket.
"""

import os
import socket
import threading

import pytest


def live_net_enabled() -> bool:
    """True when live guest<->host / external-network tests are opted in.

    QEMU SLIRP guest->host TCP is only transiently usable in sandboxed CI
    environments, so the live tests are flaky under the parallel suite (see
    #389). Set ``MMCORE_LIVE_NET=1`` to run them.
    """
    return os.environ.get("MMCORE_LIVE_NET", "").strip().lower() in (
        "1",
        "true",
        "yes",
        "on",
    )


def host_tcp_available(con, host: str = "10.0.2.2", timeout: float = 10.0) -> bool:
    """Probe guest -> host TCP, retrying because SLIRP is intermittent."""
    for _ in range(3):
        if _host_tcp_probe_once(con, host, timeout):
            return True
        try:
            con.send_line("CLOSE #9")
        except Exception:
            pass
    return False


def _host_tcp_probe_once(con, host: str, timeout: float) -> bool:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    srv.settimeout(timeout)
    port = srv.getsockname()[1]
    received: list[bytes] = []

    def probe():
        conn = None
        try:
            conn, _ = srv.accept()
            conn.settimeout(timeout)
            try:
                received.append(conn.recv(64))
            except OSError:
                received.append(b"")
        except OSError:
            pass
        finally:
            if conn is not None:
                try:
                    conn.close()
                except OSError:
                    pass

    th = threading.Thread(target=probe, daemon=True)
    th.start()
    try:
        con.send_line(f'OPEN "TCP:{host}:{port}" AS #9', timeout=timeout + 8)
        wrote = con.send_line('PRINT #9, "MMPROBE"', timeout=timeout)
        th.join(timeout=timeout)
    finally:
        con.send_line("CLOSE #9")
        srv.close()
    return wrote == "" and bool(received) and b"MMPROBE" in received[0]


def require_host_tcp(con, host: str = "10.0.2.2", timeout: float = 10.0) -> None:
    if not host_tcp_available(con, host, timeout):
        pytest.skip("QEMU SLIRP guest->host TCP unavailable in this environment")


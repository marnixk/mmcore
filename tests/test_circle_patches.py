"""Vendored Circle patches apply cleanly in build order and the patched TCP
reassembly queue behaves on the host (tests/tcp_reassembly_host.cpp)."""

import os
import re
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CIRCLE = os.path.join(REPO, "circle")
PATCHES = [
    os.path.join(REPO, "patches", "circle-wifi-149.patch"),
    os.path.join(REPO, "patches", "circle-tcp-robust.patch"),
    os.path.join(REPO, "patches", "circle-tcp-send.patch"),
]


def _patched_files(patch_path: str) -> list[str]:
    text = open(patch_path, encoding="utf-8").read()
    return sorted(set(re.findall(r"^\+\+\+ b/(\S+)", text, flags=re.M)))


def _pristine_patched_tree(dest: str) -> None:
    files = sorted({f for p in PATCHES for f in _patched_files(p)})
    assert files
    archive = subprocess.run(
        ["git", "-C", CIRCLE, "archive", "HEAD", *files],
        check=True,
        capture_output=True,
    ).stdout
    os.makedirs(dest, exist_ok=True)
    subprocess.run(["tar", "-x", "-C", dest], input=archive, check=True)
    for p in PATCHES:
        with open(p, "rb") as fh:
            res = subprocess.run(
                ["patch", "-d", dest, "-p1", "--forward", "--no-backup-if-mismatch"],
                stdin=fh,
                capture_output=True,
                text=True,
            )
        assert res.returncode == 0, f"{os.path.basename(p)} failed:\n{res.stdout}{res.stderr}"
        assert "fuzz" not in res.stdout.lower(), res.stdout


@pytest.fixture(scope="module")
def patched_tree(tmp_path_factory):
    dest = str(tmp_path_factory.mktemp("circle-patched"))
    _pristine_patched_tree(dest)
    return dest


def test_build_script_applies_patches_in_order():
    text = open(os.path.join(REPO, "scripts", "build.sh"), encoding="utf-8").read()
    a = text.index("circle-wifi-149.patch")
    b = text.index("circle-tcp-robust.patch")
    c = text.index("circle-tcp-send.patch")
    assert a < b < c
    assert "mmbasic-issue-149" in text
    assert "mmbasic-tcp-robust" in text
    assert "mmbasic-tcp-send" in text


def test_patches_carry_their_markers(patched_tree):
    ether = open(os.path.join(patched_tree, "addon/wlan/ether4330.c"), encoding="utf-8").read()
    tcp = open(os.path.join(patched_tree, "lib/net/tcpconnection.cpp"), encoding="utf-8").read()
    raq = open(os.path.join(patched_tree, "lib/net/reassemblyqueue.cpp"), encoding="utf-8").read()
    netdev = open(os.path.join(patched_tree, "lib/net/netdevlayer.cpp"), encoding="utf-8").read()
    qh = open(os.path.join(patched_tree, "include/circle/net/netbufferqueue.h"), encoding="utf-8").read()
    assert "mmbasic-issue-149" in ether
    assert "mmbasic-tcp-robust" in tcp
    assert "CReassemblyQueue::TrimSegment" in tcp
    assert "return -NET_ERROR_NOT_CONNECTED;" in tcp
    assert "m_bEnabled" not in raq
    assert "TCP reassembly queue disabled" not in raq
    assert "mmbasic-tcp-send" in netdev
    assert "mmbasic-tcp-send" in tcp
    assert "EnqueueFront" in qh
    assert "Frame deferred" in netdev
    assert "Frame dropped" not in netdev


def test_receive_drains_rx_queue_before_reporting_errno(patched_tree):
    tcp = open(os.path.join(patched_tree, "lib/net/tcpconnection.cpp"), encoding="utf-8").read()
    body = tcp[tcp.index("int CTCPConnection::Receive (") :]
    body = body[: body.index("\n}\n")]
    dequeue = body.index("m_RxQueue.Dequeue ()")
    errno_check = body.index("if (m_nErrno < 0)")
    assert dequeue < errno_check
    assert "return -NET_ERROR_CONNECTION_RESET;" not in body


def test_reassembly_queue_host_behaviour(patched_tree, tmp_path):
    exe = os.path.join(str(tmp_path), "tcp_reassembly_host")
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-O0",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-I",
            os.path.join(REPO, "tests", "circle_shim"),
            "-I",
            os.path.join(patched_tree, "include"),
            "-o",
            exe,
            os.path.join(REPO, "tests", "tcp_reassembly_host.cpp"),
            os.path.join(patched_tree, "lib", "net", "reassemblyqueue.cpp"),
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "all checks passed" in out.stdout


def test_net_cpp_reports_close_reason():
    net = open(os.path.join(REPO, "console", "net.cpp"), encoding="utf-8").read()
    term = open(os.path.join(REPO, "mmbasic", "src", "cmd_term.c"), encoding="utf-8").read()
    assert "mmb_net_tcp_close_reason" in net
    assert "closed by remote host" in net
    assert "reset by peer" in net
    assert "timed out" in net
    assert "tcp_lost(mmb_net_tcp_close_reason())" in term
    assert "Connection closed" in term
    assert "term_net_lost_report" in term


def test_send_new_segment_does_not_advance_on_failure(patched_tree):
    tcp = open(os.path.join(patched_tree, "lib/net/tcpconnection.cpp"), encoding="utf-8").read()
    body = tcp[tcp.index("boolean CTCPConnection::SendNewSegment") :]
    body = body[: body.index("int CTCPConnection::PacketReceived")]
    send = body.index("if (!SendSegment")
    nxt = body.index("m_nSND_NXT +=")
    move = body.index("m_TxQueue.MoveOn")
    assert send < nxt < move
    assert "return FALSE" in body[send:nxt]


def test_term_and_connect_coalesce_iac():
    term = open(os.path.join(REPO, "mmbasic", "src", "cmd_term.c"), encoding="utf-8").read()
    conn = open(os.path.join(REPO, "mmbasic", "src", "cmd_connect.c"), encoding="utf-8").read()
    announce = term[term.index("static void telnet_announce(void)\n{") :]
    announce = announce[: announce.index("static int term_want_echo")]
    assert "iac_flush();" in announce
    assert "term_net_send(" not in announce
    assert "iac_append" in term
    assert "iac_flush" in conn
    assert "tcp_send" in conn

"""Vendored Circle patches apply cleanly in build order and the patched TCP
reassembly queue behaves on the host (tests/tcp_reassembly_host.cpp)."""

import os
import platform
import re
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CIRCLE = os.path.join(REPO, "circle")


def _sanitizer_flags() -> list[str]:
    # Apple clang's AddressSanitizer runtime deadlocks during initialization on
    # recent macOS releases and Homebrew's GCC ships no libasan, so only UBSan
    # is usable there. Linux keeps the full address+undefined pair.
    if platform.system() == "Darwin":
        return ["-fsanitize=undefined"]
    return ["-fsanitize=address,undefined"]


PATCHES = [
    os.path.join(REPO, "patches", "circle-wifi-149.patch"),
    os.path.join(REPO, "patches", "circle-tcp-robust.patch"),
    os.path.join(REPO, "patches", "circle-tcp-send.patch"),
    os.path.join(REPO, "patches", "circle-tcp-ack.patch"),
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
    d = text.index("circle-tcp-ack.patch")
    assert a < b < c < d
    assert "mmbasic-issue-149" in text
    assert "mmbasic-tcp-robust" in text
    assert "mmbasic-tcp-send" in text
    assert "mmbasic-tcp-ack" in text


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
    assert "mmbasic-tcp-ack" in tcp
    qcpp = open(os.path.join(patched_tree, "lib/net/netbufferqueue.cpp"), encoding="utf-8").read()
    assert "RemoveHeader" in qcpp
    assert "nBytesAck == 1" not in tcp


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
            *_sanitizer_flags(),
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


def test_srv_send_only_frees_rejected_buffer():
    """CTCPConnection::Send() owns the CNetBuffer once it queues it.

    Freeing it on the success path double-frees the TX queue and halts the
    kernel (the FTP server 220-then-crash bug). Only the rejected path may
    delete it.
    """
    net = open(os.path.join(REPO, "console", "net.cpp"), encoding="utf-8").read()
    body = net[net.index("int mmb_net_srv_send(int conn") :]
    body = body[: body.index("\n}\n")]
    send = body.index("rc = tl->Send(pb, flags, c->h);")
    assert "delete pb" not in body[:send]
    after = body[send:]
    assert after.index("if (rc < 0)") < after.index("delete pb")
    assert after.index("delete pb") < after.index("return (int)chunk;")
    # DONTWAIT skips CTCPConnection's TX threshold, so send must apply its own.
    assert "bTxReady" in body


def test_ftp_xfer_treats_zero_send_as_backpressure():
    ftp = open(os.path.join(REPO, "mmbasic", "src", "cmd_ftp.c"), encoding="utf-8").read()
    assert "if (rc == 0)" in ftp
    assert "peer window full" in ftp
    assert "FTP_IDLE_MS" in ftp


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
    assert "tcp_send_all" in conn
    assert "tcp_send" in conn


def test_established_one_byte_ack_flushes_txqueue(patched_tree):
    tcp = open(os.path.join(patched_tree, "lib/net/tcpconnection.cpp"), encoding="utf-8").read()
    body = tcp[tcp.index("case TCPStateEstablished:") :]
    body = body[body.index("unsigned nBytesAck") :]
    body = body[: body.index("void CTCPConnection::OnDuplicateAck")]
    assert "nBytesAck == 1" not in body
    assert "m_TxQueue.Flush (nBytesAck)" in body


def test_receive_wakes_on_any_in_order_data(patched_tree):
    tcp = open(os.path.join(patched_tree, "lib/net/tcpconnection.cpp"), encoding="utf-8").read()
    step = tcp[tcp.index("// mmbasic-tcp-robust: a segment which starts before RCV.NXT") :]
    step = step[: step.index("case TCPStateSynReceived:")]
    assert "TCP_CONFIG_RX_THRESHOLD" not in step
    assert "m_Event.Set ()" in step
    assert "UpdateReceiveWindow" in step
    assert "m_ReassemblyQueue.GetBytesQueued" in tcp[tcp.index("void CTCPConnection::UpdateReceiveWindow") :]


def test_ooo_counts_toward_receive_window(patched_tree):
    tcp = open(os.path.join(patched_tree, "lib/net/tcpconnection.cpp"), encoding="utf-8").read()
    raq = open(
        os.path.join(patched_tree, "include/circle/net/reassemblyqueue.h"), encoding="utf-8"
    ).read()
    assert "GetBytesQueued" in raq
    step = tcp[tcp.index("// mmbasic-tcp-robust: a segment which starts before RCV.NXT") :]
    step = step[: step.index("case TCPStateSynReceived:")]
    ooo = step[step.index("else\n\t\t\t{") :]
    assert ooo.index("UpdateReceiveWindow") < ooo.index("SendSegment (TCP_FLAG_ACK")


def test_dupack_ignores_data_bearing_segments(patched_tree):
    tcp = open(os.path.join(patched_tree, "lib/net/tcpconnection.cpp"), encoding="utf-8").read()
    body = tcp[tcp.index("else if (le (nSEG_ACK, m_nSND_UNA)") :]
    body = body[: body.index("else if (gt (nSEG_ACK, m_nSND_NXT)")]
    assert "nDataLength == 0" in body
    assert "OnDuplicateAck" in body
    assert body.index("nDataLength == 0") < body.index("OnDuplicateAck")


def test_fin_ack_does_not_under_count_data(patched_tree):
    tcp = open(os.path.join(patched_tree, "lib/net/tcpconnection.cpp"), encoding="utf-8").read()
    body = tcp[tcp.index("unsigned nBytesAck") :]
    body = body[: body.index("void CTCPConnection::UpdateReceiveWindow")]
    assert "m_bFINSent" in body
    assert "nSEG_ACK == m_nSND_NXT" in body


def test_zero_window_persist_probes_one_byte(patched_tree):
    tcp = open(os.path.join(patched_tree, "lib/net/tcpconnection.cpp"), encoding="utf-8").read()
    body = tcp[tcp.index("boolean CTCPConnection::SendNewSegment") :]
    body = body[: body.index("int CTCPConnection::PacketReceived")]
    assert "m_nSND_WND == 0" in body
    assert "nLength == 1" in body


def test_txqueue_flush_trims_partial_ack(tmp_path):
    exe = os.path.join(str(tmp_path), "tcp_txqueue_host")
    src = os.path.join(REPO, "tests", "tcp_txqueue_host.cpp")
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-O0",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            *_sanitizer_flags(),
            "-I",
            os.path.join(REPO, "tests", "circle_shim"),
            "-o",
            exe,
            src,
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "all checks passed" in out.stdout

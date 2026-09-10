"""Host test: 512KB-style circular TCP RX leftover buffer."""

import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_net_rxbuf_wraps_without_dropping_mid_sequence(tmp_path):
    src = os.path.join(REPO, "tests", "net_rxbuf_host.c")
    impl = os.path.join(REPO, "console", "net_rxbuf.c")
    exe = os.path.join(tmp_path, "net_rxbuf_host")
    subprocess.run(
        [
            "gcc",
            "-O0",
            "-Wall",
            "-Werror",
            "-I",
            os.path.join(REPO, "console"),
            "-o",
            exe,
            src,
            impl,
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], check=True, capture_output=True, text=True)
    assert "all checks passed" in out.stdout


def test_term_incoming_feed_spans_hold_without_514_byte_copy():
    term = open(os.path.join(REPO, "mmbasic", "src", "cmd_term.c"), encoding="utf-8").read()
    assert "term_rx_store[MMB_NET_RX_CAP]" in term
    assert "term_rx_interpret" in term
    assert "mmb_net_rxbuf_push" in term
    assert "feed_at(" not in term
    assert "T.hold" not in term
    assert "unsigned char buf[514]" not in term
    assert "if (T.hold_n + n > (int)sizeof(buf))" not in term

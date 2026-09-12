"""Detect TCP send holes in Ethernet pcaps (client-to-server sequence gaps)."""

import importlib.util
import os
import socket
import struct

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _pcap_mod():
    path = os.path.join(REPO, "scripts", "pcap_tcp_summary.py")
    spec = importlib.util.spec_from_file_location("pcap_tcp_summary", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


pcap_tcp_summary = _pcap_mod()
client_seq_holes = pcap_tcp_summary.client_seq_holes


def _ipv4_tcp_frame(src_ip, dst_ip, sport, dport, seq, flags, payload=b"", ack=0):
    eth = bytes(12) + struct.pack("!H", 0x0800)
    ip_payload_len = 20 + len(payload)
    tot = 20 + ip_payload_len
    src = socket.inet_aton(src_ip)
    dst = socket.inet_aton(dst_ip)
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, tot, 0, 0, 64, 6, 0, src, dst)
    doff_byte = 5 << 4
    tcp = struct.pack(
        "!HHIIBBHHH", sport, dport, seq, ack, doff_byte, flags, 8192, 0, 0
    )
    return eth + ip + tcp + payload


def _write_pcap(path, frames):
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        for frame in frames:
            f.write(struct.pack("<IIII", 0, 0, len(frame), len(frame)))
            f.write(frame)


def test_client_seq_holes_empty_on_contiguous(tmp_path):
    iss = 1000
    frames = [
        _ipv4_tcp_frame("10.0.2.15", "10.0.2.100", 12345, 23, iss, 0x02),
        _ipv4_tcp_frame(
            "10.0.2.15", "10.0.2.100", 12345, 23, iss + 1, 0x18, b"\xff" * 28
        ),
        _ipv4_tcp_frame(
            "10.0.2.15", "10.0.2.100", 12345, 23, iss + 29, 0x18, b"ireal\r"
        ),
    ]
    path = str(tmp_path / "ok.pcap")
    _write_pcap(path, frames)
    assert client_seq_holes(path, dport=23) == []


def test_client_seq_holes_finds_gap(tmp_path):
    iss = 1000
    frames = [
        _ipv4_tcp_frame("10.0.2.15", "10.0.2.100", 12345, 23, iss, 0x02),
        _ipv4_tcp_frame(
            "10.0.2.15", "10.0.2.100", 12345, 23, iss + 1, 0x18, b"AAA"
        ),
        _ipv4_tcp_frame(
            "10.0.2.15", "10.0.2.100", 12345, 23, iss + 7, 0x18, b"BBB"
        ),
    ]
    path = str(tmp_path / "hole.pcap")
    _write_pcap(path, frames)
    holes = client_seq_holes(path, dport=23)
    assert len(holes) == 1
    assert holes[0]["missing"] == iss + 4
    assert holes[0]["got"] == iss + 7


def test_client_seq_holes_ignores_retransmit(tmp_path):
    iss = 1000
    first = b"hello"
    frames = [
        _ipv4_tcp_frame("10.0.2.15", "10.0.2.100", 12345, 23, iss, 0x02),
        _ipv4_tcp_frame(
            "10.0.2.15", "10.0.2.100", 12345, 23, iss + 1, 0x18, first
        ),
        _ipv4_tcp_frame(
            "10.0.2.15", "10.0.2.100", 12345, 23, iss + 1, 0x18, first
        ),
        _ipv4_tcp_frame(
            "10.0.2.15", "10.0.2.100", 12345, 23, iss + 1 + len(first), 0x18, b"x"
        ),
    ]
    path = str(tmp_path / "rtx.pcap")
    _write_pcap(path, frames)
    assert client_seq_holes(path, dport=23) == []

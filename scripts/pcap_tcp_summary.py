#!/usr/bin/env python3
"""Summarise TCP payloads in a pcap (Ethernet or Linux cooked)."""

from __future__ import annotations

import collections
import struct
import sys


def _u16(b: bytes, o: int) -> int:
    return struct.unpack_from("!H", b, o)[0]


def _u32(b: bytes, o: int) -> int:
    return struct.unpack_from("!I", b, o)[0]


def _iter_packets(path: str):
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic == b"\xd4\xc3\xb2\xa1":
            endian = "<"
        elif magic == b"\xa1\xb2\xc3\xd4":
            endian = ">"
        else:
            raise SystemExit(f"not a pcap: {path}")
        hdr = f.read(20)
        linktype = struct.unpack_from(endian + "I", hdr, 16)[0]
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                return
            incl = struct.unpack_from(endian + "I", ph, 8)[0]
            data = f.read(incl)
            if len(data) < incl:
                return
            yield linktype, data


def _ipv4_tcp(linktype: int, frame: bytes):
    if linktype == 1:
        if len(frame) < 14:
            return None
        if _u16(frame, 12) != 0x0800:
            return None
        ip = frame[14:]
    elif linktype == 113:
        if len(frame) < 16:
            return None
        proto = _u16(frame, 14)
        if proto != 0x0800:
            return None
        ip = frame[16:]
    else:
        return None
    if len(ip) < 20 or (ip[0] >> 4) != 4 or ip[9] != 6:
        return None
    ihl = (ip[0] & 0x0F) * 4
    tot = _u16(ip, 2)
    src = ".".join(str(b) for b in ip[12:16])
    dst = ".".join(str(b) for b in ip[16:20])
    tcp = ip[ihl:tot] if tot else ip[ihl:]
    if len(tcp) < 20:
        return None
    sport = _u16(tcp, 0)
    dport = _u16(tcp, 2)
    seq = _u32(tcp, 4)
    ack = _u32(tcp, 8)
    doff = (tcp[12] >> 4) * 4
    flags = tcp[13]
    win = _u16(tcp, 14)
    payload = tcp[doff:]
    return src, sport, dst, dport, seq, ack, flags, win, payload


def _flag_str(flags: int) -> str:
    names = []
    if flags & 0x02:
        names.append("SYN")
    if flags & 0x10:
        names.append("ACK")
    if flags & 0x01:
        names.append("FIN")
    if flags & 0x04:
        names.append("RST")
    if flags & 0x08:
        names.append("PSH")
    return "".join(names) or "."


def _esc(data: bytes) -> str:
    out = []
    for b in data:
        if 32 <= b < 127 and b not in (92,):
            out.append(chr(b))
        elif b == 13:
            out.append("\\r")
        elif b == 10:
            out.append("\\n")
        else:
            out.append(f"\\x{b:02x}")
    return "".join(out)


def summarise(path: str) -> str:
    streams = collections.defaultdict(lambda: {"c2s": b"", "s2c": b"", "pkts": []})
    lines = [f"# {path}"]
    n = 0
    for linktype, frame in _iter_packets(path):
        parsed = _ipv4_tcp(linktype, frame)
        if not parsed:
            continue
        src, sport, dst, dport, seq, ack, flags, win, payload = parsed
        key = tuple(sorted([(src, sport), (dst, dport)]))
        a, b = (src, sport), (dst, dport)
        to_server = a < b
        side = "c2s" if to_server else "s2c"
        st = streams[key]
        if payload:
            st[side] += payload
        n += 1
        if payload or flags & 0x07:
            lines.append(
                f"{src}:{sport} -> {dst}:{dport} {_flag_str(flags)} "
                f"seq={seq} ack={ack} win={win} len={len(payload)} "
                f"{_esc(payload)[:120]}"
            )
    lines.append(f"# packets={n} streams={len(streams)}")
    for key, st in streams.items():
        c2s, s2c = st["c2s"], st["s2c"]
        if not c2s and not s2c:
            continue
        lines.append(f"# stream {key}")
        lines.append(f"# c2s ({len(c2s)} bytes): {_esc(c2s)[:500]}")
        lines.append(f"# s2c ({len(s2c)} bytes): {_esc(s2c)[:800]}")
        for needle in (b"ireal", b"ds9space", b"Welcome ireal", b"Password", b"User:"):
            lines.append(
                f"# has {needle!r}: c2s={needle in c2s} s2c={needle in s2c}"
            )
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    for p in sys.argv[1:]:
        sys.stdout.write(summarise(p))

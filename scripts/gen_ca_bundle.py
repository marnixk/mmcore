#!/usr/bin/env python3
"""Generate mmbasic/third_party/mbedtls/ca_bundle.c from cacert.pem.

The Circle TLS backend has no operating-system trust store, so the Mozilla
root bundle shipped next to the vendored mbedTLS tree is embedded as a C byte
array. The generated file is build output and is git-ignored.

Run by console/Makefile; it only rewrites the output when the rendered text
changes, so a no-op rebuild does not invalidate the kernel.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_SRC = REPO / "mmbasic" / "third_party" / "mbedtls" / "cacert.pem"
DEFAULT_OUT = REPO / "mmbasic" / "third_party" / "mbedtls" / "ca_bundle.c"


def render(pem: bytes) -> str:
    lines = [
        "/* Generated from cacert.pem by scripts/gen_ca_bundle.py. */",
        "/* Do not edit; the build regenerates this file. */",
        "",
        "const char mmb_tls_ca_bundle_pem[] = {",
    ]
    for i in range(0, len(pem), 16):
        chunk = pem[i : i + 16]
        row = ", ".join("0x%02x" % b for b in chunk)
        lines.append("\t" + row + ",")
    # mbedTLS only recognises PEM when the buffer's final byte is NUL.
    lines.append("\t0x00,")
    lines.append("};")
    lines.append("")
    lines.append("const unsigned mmb_tls_ca_bundle_len = sizeof(mmb_tls_ca_bundle_pem) - 1;")
    lines.append("")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=str(DEFAULT_SRC))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    args = ap.parse_args(argv[1:])

    src = Path(args.src)
    out = Path(args.out)
    if not src.is_file():
        print("gen_ca_bundle: missing %s" % src, file=sys.stderr)
        return 1
    text = render(src.read_bytes())
    if out.is_file() and out.read_text() == text:
        return 0
    out.write_text(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

#!/usr/bin/env python3
"""Generate tiny PNG/JPEG/MOD/XM/MP3 test assets as C arrays."""
import os, struct, zlib, textwrap

OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "mmbasic", "assets")
SRC_OUT = os.path.join(os.path.dirname(__file__), "..", "mmbasic", "src", "assets.c")
os.makedirs(OUT_DIR, exist_ok=True)


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def make_png(path, w, h, rgb=(255, 0, 0)):
    raw = b""
    for y in range(h):
        raw += b"\x00"
        raw += bytes([rgb[0], rgb[1], rgb[2], 255]) * w
    data = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 0))
        + chunk(b"IEND", b"")
    )
    open(path, "wb").write(data)
    return data


def make_jpeg(path):
    # 1x1 red JPEG (standard baseline, non-progressive)
    # Generated to be picojpeg-friendly (SOF0 baseline).
    # A well-known 134-byte 1x1 red JPEG:
    jpeg = bytes.fromhex(
        "ffd8ffe000104a46494600010100000100010000ffdb004300080606070605080707070909080a0c140d0c0b0b0c191213"
        "0f141d1a1f1e1d1a1c1c20242e2720222c231c1c2837292c30313434341f27393d38323c2e333432ffc0000b0800010001"
        "01011100ffc4001f0000010501010101010100000000000000000102030405060708090a0bffc400b510000201030302"
        "0403050504040000017d01020300041105122131410613516107227114328191a1082342b1c11552d1f0243362728209"
        "0a161718191a25262728292a3435363738393a434445464748494a535455565758595a636465666768696a7374757677"
        "78797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2"
        "d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffda000800010001003f00fb94ae92d0028a2803ffd9"
    )
    # If that's too large/malformed, fall back to a tiny valid JPEG created with a simple SOF0.
    try:
        from PIL import Image
        import io
        im = Image.new("RGB", (8, 8), (255, 0, 0))
        buf = io.BytesIO()
        im.save(buf, format="JPEG", quality=80, progressive=False)
        jpeg = buf.getvalue()
    except Exception:
        pass
    open(path, "wb").write(jpeg)
    return jpeg


def make_mod(path):
    title = b"TESTMOD" + b"\0" * (20 - 7)
    samples = bytearray(31 * 30)
    # Sample 0: length in 16-bit words (big-endian), volume 64
    length_words = 256  # 512 bytes of sample data
    samples[0:22] = b"SAMP0".ljust(22, b"\0")
    samples[22:24] = struct.pack(">H", length_words)
    samples[24] = 0  # finetune
    samples[25] = 64  # volume
    samples[26:30] = struct.pack(">HH", 0, 0)  # reppnt, replen

    header_tail = bytes([1, 127]) + bytes(128) + b"M.K."

    pattern = bytearray(64 * 4 * 4)
    # Row 0, channel 0: sample 1, period 428 (C-3)
    pattern[0] = 0x11  # sample 1 in high nibble, period high 0x1
    pattern[1] = 0xAC  # period low (428)
    pattern[2] = 0
    pattern[3] = 0

    sample_data = bytearray(length_words * 2)
    for i in range(len(sample_data)):
        sample_data[i] = 0x7F if (i // 32) % 2 == 0 else 0x81  # signed 8-bit square

    body = title + bytes(samples) + header_tail + bytes(pattern) + bytes(sample_data)
    open(path, "wb").write(body)
    return body


def make_xm(path):
    # FastTracker II XM 1.04: 1 channel, 1 pattern, 1 instrument with 1 sample
    name = b"TESTXM".ljust(20, b" ")
    hdr = bytearray()
    hdr += b"Extended Module: "
    hdr += name
    hdr += b"\x1a"
    hdr += b"FastTracker II".ljust(20, b" ")
    hdr += struct.pack("<H", 0x0104)

    header_size = 276
    hdr += struct.pack(
        "<IHHHHHHHH",
        header_size,
        1,   # song length
        0,   # restart
        1,   # channels
        1,   # patterns
        1,   # instruments
        1,   # flags (linear frequencies)
        6,   # tempo
        125, # bpm
    )
    order = bytes([0]) + bytes([255] * 255)
    hdr += order[:256]
    assert len(hdr) == 60 + 276

    # Pattern 0: one uncompressed note on row 0 channel 0
    pat_data = bytes([
        49,  # note (C-4)
        1,   # instrument
        0x40,  # volume column
        0, 0,  # effect
    ])
    pat = struct.pack("<IBHH", 9, 0, 64, len(pat_data)) + pat_data

    # Instrument 1 with one 8-bit mono sample
    sample_len = 512
    inst_hdr_size = 263
    inst = bytearray()
    inst += struct.pack("<I", inst_hdr_size)
    inst += b"ins1".ljust(22, b" ")
    inst += struct.pack("<B", 0)       # type
    inst += struct.pack("<H", 1)       # num_samples
    inst += struct.pack("<I", 40)      # sample header size
    inst += bytes(96)                  # sample_of_notes
    inst += bytes(48)                  # volume envelope points (12 * 4)
    inst += bytes(48)                  # panning envelope points (12 * 4)
    inst += bytes(8)                   # point counts + sustain/loop bytes
    inst += bytes(2)                   # envelope flags
    inst += bytes(4)                   # vibrato type/sweep/depth/rate
    inst += struct.pack("<H", 0)        # volume fadeout
    inst += bytes(22)                  # padding to 263 bytes
    assert len(inst) == inst_hdr_size

    samp_hdr = bytearray(40)
    struct.pack_into("<I", samp_hdr, 0, sample_len)
    struct.pack_into("<I", samp_hdr, 4, 0)   # loop start
    struct.pack_into("<I", samp_hdr, 8, 0)   # loop length
    samp_hdr[12] = 64   # volume
    samp_hdr[13] = 0    # finetune
    samp_hdr[14] = 0    # flags: 8-bit, no loop
    samp_hdr[15] = 128  # panning
    samp_hdr[16] = 0    # relative note
    samp_hdr[18:40] = b"tone".ljust(22, b" ")

    sample_data = bytearray(sample_len)
    for i in range(sample_len):
        sample_data[i] = 0x7F if (i // 32) % 2 == 0 else 0x81

    data = bytes(hdr) + pat + bytes(inst) + bytes(samp_hdr) + bytes(sample_data)
    open(path, "wb").write(data)
    return data


def make_mp3(path):
    # Try ffmpeg; otherwise a silent MPEG-1 Layer III frame (not always decoded).
    import subprocess, tempfile, shutil
    wav = os.path.join(OUT_DIR, "_tone.wav")
    try:
        subprocess.run(
            ["ffmpeg", "-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=0.2",
             "-ar", "44100", "-ac", "1", wav],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["ffmpeg", "-y", "-i", wav, "-codec:a", "libmp3lame", "-q:a", "7", path],
            check=True, capture_output=True,
        )
        os.remove(wav)
        return open(path, "rb").read()
    except Exception:
        # MPEG1 L3 32kbps 44100Hz mono empty-ish frame header + padding
        # 0xFFFB is MPEG1 Layer3, 32kbps, 44.1kHz
        frame = bytes([0xFF, 0xFB, 0x10, 0xC4]) + bytes(104)
        data = frame * 20
        open(path, "wb").write(data)
        return data


def c_array(name, data):
    hexb = ", ".join("0x%02x" % b for b in data)
    wrapped = "\n    ".join(textwrap.wrap(hexb, 80))
    return f"const unsigned char {name}[{len(data)}] = {{\n    {wrapped}\n}};\nconst unsigned {name}_len = {len(data)};\n"


def main():
    png = make_png(os.path.join(OUT_DIR, "test.png"), 8, 8, (255, 0, 0))
    jpg = make_jpeg(os.path.join(OUT_DIR, "test.jpg"))
    mod = make_mod(os.path.join(OUT_DIR, "test.mod"))
    xm = make_xm(os.path.join(OUT_DIR, "test.xm"))
    mp3 = make_mp3(os.path.join(OUT_DIR, "test.mp3"))
    src = """#include "mmb_priv.h"

"""
    src += c_array("asset_png", png) + "\n"
    src += c_array("asset_jpg", jpg) + "\n"
    src += c_array("asset_mod", mod) + "\n"
    src += c_array("asset_xm", xm) + "\n"
    src += c_array("asset_mp3", mp3) + "\n"
    src += """
extern const unsigned char asset_png[];
extern const unsigned asset_png_len;
extern const unsigned char asset_jpg[];
extern const unsigned asset_jpg_len;
extern const unsigned char asset_mod[];
extern const unsigned asset_mod_len;
extern const unsigned char asset_xm[];
extern const unsigned asset_xm_len;
extern const unsigned char asset_mp3[];
extern const unsigned asset_mp3_len;

void mmb_assets_seed(void)
{
    mmb_vfs_seed_file("TEST.PNG", asset_png, asset_png_len);
    mmb_vfs_seed_file("TEST.JPG", asset_jpg, asset_jpg_len);
    mmb_vfs_seed_file("TEST.MOD", asset_mod, asset_mod_len);
    mmb_vfs_seed_file("TEST.XM", asset_xm, asset_xm_len);
    mmb_vfs_seed_file("TEST.MP3", asset_mp3, asset_mp3_len);
}
"""
    open(SRC_OUT, "w").write(src)
    print("wrote", SRC_OUT, "png", len(png), "jpg", len(jpg), "mod", len(mod), "xm", len(xm), "mp3", len(mp3))


if __name__ == "__main__":
    main()

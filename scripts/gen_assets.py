#!/usr/bin/env python3
"""Generate the tiny binary test fixtures under ``ramdisk/tests/``.

Outputs uppercase fixtures (TEST.PNG, TESTZ.PNG, TEST.JPG, TEST.MOD, TEST.XM,
TEST.S3M, TEST.MP3, TEST.WAV) that the QEMU tests consume as ``A:/tests/...``.
No C source is produced; the ramdisk build embeds them from
``ramdisk/tests/``.
"""
import os
import struct
import zlib

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT_DIR = os.path.join(BASE, "ramdisk", "tests")
os.makedirs(OUT_DIR, exist_ok=True)


def out(name):
    return os.path.join(OUT_DIR, name)


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def make_png(path, w, h, rgb=(255, 0, 0), level=0):
    raw = b""
    for y in range(h):
        raw += b"\x00"
        raw += bytes([rgb[0], rgb[1], rgb[2], 255]) * w
    data = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, level))
        + chunk(b"IEND", b"")
    )
    open(path, "wb").write(data)
    return data


def make_jpeg(path):
    import subprocess
    try:
        subprocess.run(
            [
                "ffmpeg", "-y", "-f", "lavfi", "-i", "color=c=red:s=8x8:d=0.1",
                "-frames:v", "1", "-pix_fmt", "yuvj420p", "-q:v", "5",
                path,
            ],
            check=True, capture_output=True,
        )
        return open(path, "rb").read()
    except Exception:
        pass
    try:
        from PIL import Image
        import io
        im = Image.new("RGB", (8, 8), (255, 0, 0))
        buf = io.BytesIO()
        im.save(buf, format="JPEG", quality=80, progressive=False)
        jpeg = buf.getvalue()
        open(path, "wb").write(jpeg)
        return jpeg
    except Exception:
        pass
    raise RuntimeError("cannot generate a picojpeg-friendly 8x8 baseline JPEG")


def make_mod(path):
    title = b"TESTMOD" + b"\0" * (20 - 7)
    samples = bytearray(31 * 30)
    length_words = 256  # 512 bytes of sample data
    samples[0:22] = b"SAMP0".ljust(22, b"\0")
    samples[22:24] = struct.pack(">H", length_words)
    samples[24] = 0  # finetune
    samples[25] = 64  # volume
    samples[26:30] = struct.pack(">HH", 0, 0)  # reppnt, replen

    header_tail = bytes([1, 127]) + bytes(128) + b"M.K."

    pattern = bytearray(64 * 4 * 4)
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

    pat_data = bytes([
        49,  # note (C-4)
        1,   # instrument
        0x40,  # volume column
        0, 0,  # effect
    ])
    pat = struct.pack("<IBHH", 9, 0, 64, len(pat_data)) + pat_data

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


def make_s3m(path):
    """Write a minimal Scream Tracker 3 module: one 8-bit square-wave sample,
    one pattern, one row, C-4 on channel 0 (mirrors make_mod/make_xm)."""
    sample_len = 512
    inst_off = 0x100
    pat_off = 0x200
    sam_off = 0x300

    hdr = bytearray(96)
    name = b"TESTS3M"
    hdr[0:len(name)] = name
    hdr[28] = 0x1A  # type: ST3 module
    hdr[29] = 0x10
    struct.pack_into("<H", hdr, 32, 1)      # sequence length
    struct.pack_into("<H", hdr, 34, 1)      # instruments
    struct.pack_into("<H", hdr, 36, 1)      # patterns
    struct.pack_into("<H", hdr, 38, 0)      # flags
    struct.pack_into("<H", hdr, 40, 0x1300)  # created with ST3
    struct.pack_into("<H", hdr, 42, 1)      # signed samples
    hdr[44:48] = b"SCRM"
    hdr[48] = 64                            # global volume
    hdr[49] = 6                             # initial speed
    hdr[50] = 125                           # initial tempo
    hdr[51] = 0x80 | 64                     # stereo, master volume 64
    hdr[52] = 0                             # ultra click removal
    hdr[53] = 0                             # no default-panning block
    hdr[64] = 0                             # channel 0 -> playback channel 0
    for i in range(1, 32):
        hdr[64 + i] = 255                   # remaining channels unused

    inst = bytearray(80)
    inst[0] = 1                             # PCM sample
    inst[1:9] = b"samp.s3m"
    inst[13] = 0                            # memseg high byte
    struct.pack_into("<H", inst, 14, sam_off >> 4)
    struct.pack_into("<I", inst, 16, sample_len)
    struct.pack_into("<I", inst, 20, 0)     # loop begin
    struct.pack_into("<I", inst, 24, 0)     # loop end
    inst[28] = 64                           # volume
    inst[30] = 0                            # unpacked
    inst[31] = 0                            # 8-bit mono, no loop
    struct.pack_into("<I", inst, 32, 8363)  # C-2 speed
    inst[48:52] = b"tone"
    inst[76:78] = b"SC"                     # "SCRS" signature

    pat = bytearray(struct.pack("<H", 8))
    pat += bytes([0x20, 0x40, 0x01])        # row 0: C-4, instrument 1
    pat += bytes(64)                        # row terminators (0 = next row)

    sample_data = bytearray(sample_len)
    for i in range(sample_len):
        sample_data[i] = 0x7F if (i // 32) % 2 == 0 else 0x81

    body = bytearray(hdr)
    body += bytes([0])                      # order list: pattern 0
    body += struct.pack("<H", inst_off >> 4)  # instrument parapointer
    body += struct.pack("<H", pat_off >> 4)   # pattern parapointer
    body += bytes(inst_off - len(body))
    body += bytes(inst)
    body += bytes(pat_off - len(body))
    body += bytes(pat)
    body += bytes(sam_off - len(body))
    body += bytes(sample_data)
    open(path, "wb").write(bytes(body))
    return bytes(body)


def make_mp3(path):
    import subprocess
    wav = out("_tone.wav")
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
        frame = bytes([0xFF, 0xFB, 0x10, 0xC4]) + bytes(104)
        data = frame * 20
        open(path, "wb").write(data)
        return data


def make_wav(path):
    """Write a small 8-bit mono square-wave WAV (no C output)."""
    rate = 8000
    n = 24000
    body = bytearray()
    for i in range(n):
        body.append(200 if (i // 9) & 1 else 56)
    data_len = len(body)
    hdr = bytearray()
    hdr += b"RIFF" + struct.pack("<I", 36 + data_len) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate, 1, 8)
    hdr += b"data" + struct.pack("<I", data_len)
    open(path, "wb").write(bytes(hdr) + bytes(body))
    return bytes(hdr) + bytes(body)


def main():
    make_png(out("TEST.PNG"), 8, 8, (255, 0, 0), 0)
    make_png(out("TESTZ.PNG"), 8, 8, (0, 255, 0), 9)
    make_jpeg(out("TEST.JPG"))
    make_mod(out("TEST.MOD"))
    make_xm(out("TEST.XM"))
    make_s3m(out("TEST.S3M"))
    make_mp3(out("TEST.MP3"))
    make_wav(out("TEST.WAV"))
    names = sorted(os.listdir(OUT_DIR))
    print("wrote", OUT_DIR, names)


if __name__ == "__main__":
    main()

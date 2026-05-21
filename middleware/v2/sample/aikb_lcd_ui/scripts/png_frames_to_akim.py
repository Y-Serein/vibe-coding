#!/usr/bin/env python3
"""Pack PNG frame sequences into the AKIM pet animation container.

AKIM layout, little-endian:

    char     magic[4]     "AKIM"
    uint32_t version       = 1
    uint32_t frame_count
    uint32_t frame_delay_ms
    uint32_t width
    uint32_t height
    uint32_t flags         bit0 LOOP, bit1 ARGB8888

Pet frames are stored as raw RGBA8888 bytes by default. The board renderer
uses alpha for compositing and scales the frame into the pet stage box.
"""

import argparse
import glob
import os
import struct
import sys
import zlib


PNG_SIG = b"\x89PNG\r\n\x1a\n"
AKIM_MAGIC = b"AKIM"
AKIM_VERSION = 1
AKIM_FLAG_LOOP = 0x1
AKIM_FLAG_ARGB8888 = 0x2


def _paeth(a, b, c):
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png_rgba(path):
    data = open(path, "rb").read()
    if data[:8] != PNG_SIG:
        raise ValueError(f"{path}: not a PNG")

    idx = 8
    width = height = bit_depth = color_type = 0
    interlace = 0
    idat = bytearray()
    while idx < len(data):
        if idx + 8 > len(data):
            raise ValueError(f"{path}: truncated chunk header")
        (length,) = struct.unpack(">I", data[idx : idx + 4])
        ctype = data[idx + 4 : idx + 8]
        body = data[idx + 8 : idx + 8 + length]
        idx += 8 + length + 4
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _comp, _filt, interlace = (
                struct.unpack(">IIBBBBB", body)
            )
        elif ctype == b"IDAT":
            idat.extend(body)
        elif ctype == b"IEND":
            break

    if interlace:
        raise ValueError(f"{path}: interlaced PNG is not supported")
    if bit_depth != 8 or color_type not in (2, 6):
        raise ValueError(
            f"{path}: expected 8-bit RGB/RGBA PNG, got bit_depth={bit_depth} "
            f"color_type={color_type}"
        )

    src_bpp = 4 if color_type == 6 else 3
    raw = zlib.decompress(bytes(idat))
    stride = width * src_bpp
    expected = height * (stride + 1)
    if len(raw) != expected:
        raise ValueError(f"{path}: decompressed {len(raw)} bytes != {expected}")

    rows = []
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = raw[pos]
        pos += 1
        row = bytearray(raw[pos : pos + stride])
        pos += stride
        if ftype == 0:
            pass
        elif ftype == 1:
            for x in range(src_bpp, stride):
                row[x] = (row[x] + row[x - src_bpp]) & 0xFF
        elif ftype == 2:
            for x in range(stride):
                row[x] = (row[x] + prev[x]) & 0xFF
        elif ftype == 3:
            for x in range(stride):
                left = row[x - src_bpp] if x >= src_bpp else 0
                row[x] = (row[x] + ((left + prev[x]) >> 1)) & 0xFF
        elif ftype == 4:
            for x in range(stride):
                left = row[x - src_bpp] if x >= src_bpp else 0
                up = prev[x]
                up_left = prev[x - src_bpp] if x >= src_bpp else 0
                row[x] = (row[x] + _paeth(left, up, up_left)) & 0xFF
        else:
            raise ValueError(f"{path}: unknown PNG filter {ftype} on row {y}")
        rows.append(bytes(row))
        prev = row

    rgba = bytearray(width * height * 4)
    out = 0
    for row in rows:
        for x in range(0, len(row), src_bpp):
            rgba[out + 0] = row[x + 0]
            rgba[out + 1] = row[x + 1]
            rgba[out + 2] = row[x + 2]
            rgba[out + 3] = row[x + 3] if src_bpp == 4 else 255
            out += 4
    return width, height, bytes(rgba)


def expand_inputs(patterns):
    out = []
    for pattern in patterns:
        matches = sorted(glob.glob(pattern))
        out.extend(matches if matches else [pattern])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inputs", nargs="+", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--frame-delay-ms", type=int, default=120)
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--no-loop", action="store_true")
    ap.add_argument(
        "--argb8888",
        action="store_true",
        help="store bytes as BGRA and set AKIM ARGB8888 compatibility flag",
    )
    args = ap.parse_args()

    if args.frame_delay_ms <= 0:
        raise SystemExit("--frame-delay-ms must be positive")

    frames = []
    width = args.width
    height = args.height
    for src in expand_inputs(args.inputs):
        w, h, rgba = decode_png_rgba(src)
        if width is None:
            width = w
        if height is None:
            height = h
        if w != width or h != height:
            raise SystemExit(f"{src}: {w}x{h} != expected {width}x{height}")
        if args.argb8888:
            buf = bytearray(rgba)
            for i in range(0, len(buf), 4):
                buf[i + 0], buf[i + 2] = buf[i + 2], buf[i + 0]
            rgba = bytes(buf)
        frames.append(rgba)
        print(f"frame {len(frames):02d}: {src} {w}x{h}", file=sys.stderr)

    if not frames:
        raise SystemExit("no frames")

    flags = 0 if args.no_loop else AKIM_FLAG_LOOP
    if args.argb8888:
        flags |= AKIM_FLAG_ARGB8888
    header = struct.pack(
        "<4sIIIIII",
        AKIM_MAGIC,
        AKIM_VERSION,
        len(frames),
        args.frame_delay_ms,
        width,
        height,
        flags,
    )
    with open(args.out, "wb") as f:
        f.write(header)
        for frame in frames:
            f.write(frame)

    print(
        f"wrote {args.out}: {len(frames)} frames, {width}x{height}, "
        f"{args.frame_delay_ms}ms, flags=0x{flags:x}, "
        f"{os.path.getsize(args.out)} bytes",
        file=sys.stderr,
    )


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, zlib.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)

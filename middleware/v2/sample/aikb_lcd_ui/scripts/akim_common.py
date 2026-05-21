#!/usr/bin/env python3
"""Small stdlib helpers for AIKB AKIM pet assets."""

import os
import struct
import zlib


PNG_SIG = b"\x89PNG\r\n\x1a\n"
AKIM_MAGIC = b"AKIM"
AKIM_VERSION = 1
AKIM_FLAG_LOOP = 0x1
AKIM_FLAG_ARGB8888 = 0x2
AKIM_HEADER = "<4sIIIIII"
AKIM_HEADER_SIZE = struct.calcsize(AKIM_HEADER)


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


def read_png_rgba(path):
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


def _png_chunk(ctype, body):
    crc = zlib.crc32(ctype)
    crc = zlib.crc32(body, crc) & 0xFFFFFFFF
    return struct.pack(">I", len(body)) + ctype + body + struct.pack(">I", crc)


def write_png_rgba(path, width, height, rgba):
    if width <= 0 or height <= 0:
        raise ValueError("PNG dimensions must be positive")
    if len(rgba) != width * height * 4:
        raise ValueError(f"RGBA length {len(rgba)} != {width}*{height}*4")
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        start = y * stride
        raw.extend(rgba[start : start + stride])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    data = (
        PNG_SIG
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + _png_chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(data)


def crop_rgba(width, height, rgba, roi):
    x, y, w, h = roi
    if x < 0 or y < 0 or w <= 0 or h <= 0 or x + w > width or y + h > height:
        raise ValueError(f"ROI {roi} is outside source {width}x{height}")
    out = bytearray(w * h * 4)
    dst_stride = w * 4
    src_stride = width * 4
    for row in range(h):
        src = ((y + row) * src_stride) + x * 4
        dst = row * dst_stride
        out[dst : dst + dst_stride] = rgba[src : src + dst_stride]
    return bytes(out)


def resize_rgba_nearest(src_w, src_h, rgba, dst_w, dst_h):
    if src_w == dst_w and src_h == dst_h:
        return rgba
    if dst_w <= 0 or dst_h <= 0:
        raise ValueError("resize dimensions must be positive")
    out = bytearray(dst_w * dst_h * 4)
    for y in range(dst_h):
        sy = min(src_h - 1, (y * src_h) // dst_h)
        for x in range(dst_w):
            sx = min(src_w - 1, (x * src_w) // dst_w)
            src = (sy * src_w + sx) * 4
            dst = (y * dst_w + x) * 4
            out[dst : dst + 4] = rgba[src : src + 4]
    return bytes(out)


def rgba_to_bgra(rgba):
    out = bytearray(rgba)
    for i in range(0, len(out), 4):
        out[i + 0], out[i + 2] = out[i + 2], out[i + 0]
    return bytes(out)


def write_akim(path, width, height, frames_rgba, frame_delay_ms,
               loop=True, argb8888=True):
    if not frames_rgba:
        raise ValueError("no AKIM frames")
    if frame_delay_ms <= 0:
        raise ValueError("frame_delay_ms must be positive")
    frame_bytes = width * height * 4
    for idx, frame in enumerate(frames_rgba):
        if len(frame) != frame_bytes:
            raise ValueError(f"frame {idx}: {len(frame)} bytes != {frame_bytes}")

    flags = 0
    if loop:
        flags |= AKIM_FLAG_LOOP
    if argb8888:
        flags |= AKIM_FLAG_ARGB8888

    header = struct.pack(
        AKIM_HEADER,
        AKIM_MAGIC,
        AKIM_VERSION,
        len(frames_rgba),
        frame_delay_ms,
        width,
        height,
        flags,
    )
    with open(path, "wb") as f:
        f.write(header)
        for frame in frames_rgba:
            f.write(rgba_to_bgra(frame) if argb8888 else frame)


def read_akim(path):
    data = open(path, "rb").read()
    if len(data) < AKIM_HEADER_SIZE:
        raise ValueError(f"{path}: too small")
    magic, version, count, delay_ms, width, height, flags = struct.unpack(
        AKIM_HEADER, data[:AKIM_HEADER_SIZE]
    )
    if magic != AKIM_MAGIC:
        raise ValueError(f"{path}: bad magic {magic!r}")
    if version != AKIM_VERSION:
        raise ValueError(f"{path}: unsupported version {version}")
    if width <= 0 or height <= 0 or count <= 0 or delay_ms <= 0:
        raise ValueError(
            f"{path}: invalid {width}x{height} count={count} delay={delay_ms}"
        )
    frame_bytes = width * height * 4
    expected = AKIM_HEADER_SIZE + count * frame_bytes
    if len(data) != expected:
        raise ValueError(f"{path}: size {len(data)} != expected {expected}")

    argb8888 = bool(flags & AKIM_FLAG_ARGB8888)
    frames = []
    pos = AKIM_HEADER_SIZE
    for _idx in range(count):
        frame = data[pos : pos + frame_bytes]
        pos += frame_bytes
        frames.append(rgba_to_bgra(frame) if argb8888 else frame)
    return {
        "path": path,
        "width": width,
        "height": height,
        "frame_count": count,
        "frame_delay_ms": delay_ms,
        "flags": flags,
        "argb8888": argb8888,
        "loop": bool(flags & AKIM_FLAG_LOOP),
        "frames_rgba": frames,
        "size": os.path.getsize(path),
    }

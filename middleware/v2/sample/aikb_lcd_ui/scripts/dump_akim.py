#!/usr/bin/env python3
"""Dump an AKIM animation container back to PNG frames."""

import argparse
import math
import os
import sys

from akim_common import read_akim, resize_rgba_nearest, write_png_rgba


def paste_rgba(dst, dst_w, src, src_w, src_h, ox, oy):
    for y in range(src_h):
        dst_off = ((oy + y) * dst_w + ox) * 4
        src_off = y * src_w * 4
        dst[dst_off : dst_off + src_w * 4] = src[src_off : src_off + src_w * 4]


def write_preview(info, out_path, max_frames, scale):
    frames = info["frames_rgba"]
    count = min(len(frames), max_frames)
    cols = min(8, count)
    rows = int(math.ceil(count / cols))
    src_w = info["width"]
    src_h = info["height"]
    tile_w = src_w * scale
    tile_h = src_h * scale
    gap = 4
    sheet_w = cols * tile_w + (cols + 1) * gap
    sheet_h = rows * tile_h + (rows + 1) * gap
    sheet = bytearray([32, 32, 32, 255] * sheet_w * sheet_h)

    for idx in range(count):
        frame = frames[idx]
        if scale != 1:
            frame = resize_rgba_nearest(src_w, src_h, frame, tile_w, tile_h)
        col = idx % cols
        row = idx // cols
        paste_rgba(sheet, sheet_w, frame, tile_w, tile_h,
                   gap + col * (tile_w + gap), gap + row * (tile_h + gap))
    write_png_rgba(out_path, sheet_w, sheet_h, bytes(sheet))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="input", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--prefix", default="frame")
    ap.add_argument("--preview-out")
    ap.add_argument("--preview-scale", type=int, default=4)
    ap.add_argument("--max-preview-frames", type=int, default=32)
    args = ap.parse_args()

    if args.preview_scale <= 0:
        raise SystemExit("--preview-scale must be positive")
    if args.max_preview_frames <= 0:
        raise SystemExit("--max-preview-frames must be positive")

    info = read_akim(args.input)
    os.makedirs(args.out_dir, exist_ok=True)
    for idx, frame in enumerate(info["frames_rgba"]):
        path = os.path.join(args.out_dir, f"{args.prefix}_{idx:03d}.png")
        write_png_rgba(path, info["width"], info["height"], frame)

    if args.preview_out:
        write_preview(info, args.preview_out,
                      args.max_preview_frames, args.preview_scale)

    print(
        f"{args.input}: {info['frame_count']} frames, "
        f"{info['width']}x{info['height']}, {info['frame_delay_ms']}ms, "
        f"flags=0x{info['flags']:x}, "
        f"{'ARGB8888' if info['argb8888'] else 'RGBA'}, "
        f"{'LOOP' if info['loop'] else 'NO_LOOP'}",
        file=sys.stderr,
    )
    print(f"wrote {info['frame_count']} PNG frames to {args.out_dir}",
          file=sys.stderr)
    if args.preview_out:
        print(f"wrote preview {args.preview_out}", file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)

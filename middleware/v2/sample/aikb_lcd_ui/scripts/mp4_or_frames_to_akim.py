#!/usr/bin/env python3
"""Build an AKIM pet asset from an MP4 or PNG frame sequence.

The intended workflow is two-step:

1. Generate a contact-sheet preview with the ROI rectangle overlaid.
2. After the ROI is approved, crop only that dynamic subject area, nearest-neighbor
   resize to the target pet frame size, and pack ARGB8888 AKIM.
"""

import argparse
import glob
import math
import os
import shutil
import subprocess
import sys
import tempfile
import zlib

from akim_common import (
    crop_rgba,
    read_png_rgba,
    resize_rgba_nearest,
    strip_matte_edges,
    write_akim,
    write_png_rgba,
)


VIDEO_EXTS = {".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v"}


def parse_csv4(text, name):
    parts = text.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError(f"{name} must be x,y,w,h")
    try:
        vals = [float(p.strip()) for p in parts]
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"{name} must contain numbers") from exc
    return vals


def roi_from_args(args, src_w, src_h, require):
    if args.roi and args.roi_percent:
        raise SystemExit("use only one of --roi or --roi-percent")
    if args.roi:
        x, y, w, h = [int(round(v)) for v in parse_csv4(args.roi, "--roi")]
    elif args.roi_percent:
        vals = parse_csv4(args.roi_percent, "--roi-percent")
        # Accept either 0..1 fractions or 0..100 percentages.
        if max(vals) <= 1.0:
            vals = [v * 100.0 for v in vals]
        x = int(round(src_w * vals[0] / 100.0))
        y = int(round(src_h * vals[1] / 100.0))
        w = int(round(src_w * vals[2] / 100.0))
        h = int(round(src_h * vals[3] / 100.0))
    else:
        if require:
            raise SystemExit("packing requires --roi or --roi-percent")
        x, y, w, h = 0, 0, src_w, src_h

    if x < 0 or y < 0 or w <= 0 or h <= 0 or x + w > src_w or y + h > src_h:
        raise SystemExit(f"ROI {x},{y},{w},{h} is outside source {src_w}x{src_h}")
    return x, y, w, h


def is_video_path(path):
    return os.path.isfile(path) and os.path.splitext(path)[1].lower() in VIDEO_EXTS


def collect_png_frames(path):
    if os.path.isdir(path):
        frames = sorted(
            glob.glob(os.path.join(path, "*.png"))
            + glob.glob(os.path.join(path, "*.PNG"))
        )
    else:
        matches = sorted(glob.glob(path))
        frames = matches if matches else [path]
    frames = [f for f in frames if os.path.isfile(f)]
    if not frames:
        raise SystemExit(f"no PNG frames found in {path}")
    return frames


def extract_video_frames(args, temp_dir):
    pattern = os.path.join(temp_dir, "frame_%06d.png")
    cmd = [
        args.ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
    ]
    if args.start_sec > 0:
        cmd += ["-ss", str(args.start_sec)]
    cmd += ["-i", args.input]
    if args.duration_sec is not None:
        cmd += ["-t", str(args.duration_sec)]
    cmd += ["-vf", f"fps={args.fps}", pattern]
    subprocess.run(cmd, check=True)
    frames = collect_png_frames(os.path.join(temp_dir, "frame_*.png"))
    if not frames:
        raise SystemExit("ffmpeg produced no frames")
    return frames


def load_source_frames(args):
    if is_video_path(args.input):
        tmp = tempfile.TemporaryDirectory(prefix="akim_frames_")
        try:
            return extract_video_frames(args, tmp.name), tmp
        except FileNotFoundError as exc:
            tmp.cleanup()
            raise SystemExit(f"{args.ffmpeg}: not found") from exc
        except subprocess.CalledProcessError as exc:
            tmp.cleanup()
            raise SystemExit(f"ffmpeg failed with exit code {exc.returncode}") from exc

    frames = collect_png_frames(args.input)
    start = int(math.floor(args.start_sec * args.fps))
    end = len(frames)
    if args.duration_sec is not None:
        end = min(end, start + int(math.ceil(args.duration_sec * args.fps)))
    frames = frames[start:end]
    if not frames:
        raise SystemExit("frame range is empty")
    return frames, None


def draw_rect(rgba, width, height, roi, color=(255, 48, 32, 255), thickness=2):
    x, y, w, h = roi
    out = bytearray(rgba)
    thickness = max(1, thickness)

    def set_px(px, py):
        if 0 <= px < width and 0 <= py < height:
            off = (py * width + px) * 4
            out[off : off + 4] = bytes(color)

    for t in range(thickness):
        yy1 = y + t
        yy2 = y + h - 1 - t
        for xx in range(x, x + w):
            set_px(xx, yy1)
            set_px(xx, yy2)
        xx1 = x + t
        xx2 = x + w - 1 - t
        for yy in range(y, y + h):
            set_px(xx1, yy)
            set_px(xx2, yy)
    return bytes(out)


def paste_rgba(dst, dst_w, src, src_w, src_h, ox, oy):
    for y in range(src_h):
        dst_off = ((oy + y) * dst_w + ox) * 4
        src_off = y * src_w * 4
        dst[dst_off : dst_off + src_w * 4] = src[src_off : src_off + src_w * 4]


def pick_preview_frames(frames, max_count):
    if len(frames) <= max_count:
        return frames
    out = []
    for i in range(max_count):
        idx = round(i * (len(frames) - 1) / (max_count - 1))
        out.append(frames[idx])
    return out


def write_preview(frames, roi, out_path, max_frames, tile_width):
    preview_frames = pick_preview_frames(frames, max_frames)
    src_w, src_h, _rgba = read_png_rgba(preview_frames[0])
    scale_w = min(tile_width, src_w)
    tile_h = max(1, round(src_h * scale_w / src_w))
    cols = min(4, len(preview_frames))
    rows = int(math.ceil(len(preview_frames) / cols))
    gap = 8
    sheet_w = cols * scale_w + (cols + 1) * gap
    sheet_h = rows * tile_h + (rows + 1) * gap
    sheet = bytearray([32, 32, 32, 255] * sheet_w * sheet_h)

    roi_scaled = (
        round(roi[0] * scale_w / src_w),
        round(roi[1] * tile_h / src_h),
        max(1, round(roi[2] * scale_w / src_w)),
        max(1, round(roi[3] * tile_h / src_h)),
    )
    for idx, frame_path in enumerate(preview_frames):
        w, h, rgba = read_png_rgba(frame_path)
        if w != src_w or h != src_h:
            raise SystemExit(
                f"{frame_path}: {w}x{h} differs from preview source {src_w}x{src_h}"
            )
        tile = resize_rgba_nearest(w, h, rgba, scale_w, tile_h)
        tile = draw_rect(tile, scale_w, tile_h, roi_scaled, thickness=2)
        col = idx % cols
        row = idx // cols
        paste_rgba(sheet, sheet_w, tile, scale_w, tile_h,
                   gap + col * (scale_w + gap), gap + row * (tile_h + gap))
    write_png_rgba(out_path, sheet_w, sheet_h, bytes(sheet))


def build_akim(frames, roi, args):
    src_w, src_h, _ = read_png_rgba(frames[0])
    out_w = args.width if args.width else roi[2]
    out_h = args.height if args.height else roi[3]
    delay_ms = round(1000 / args.fps)
    packed = []
    for idx, path in enumerate(frames):
        w, h, rgba = read_png_rgba(path)
        if w != src_w or h != src_h:
            raise SystemExit(f"{path}: {w}x{h} differs from {src_w}x{src_h}")
        cropped = crop_rgba(w, h, rgba, roi)
        packed_frame = resize_rgba_nearest(roi[2], roi[3], cropped, out_w, out_h)
        packed.append(
            strip_matte_edges(
                packed_frame,
                out_w,
                out_h,
                threshold=args.matte_edge_threshold,
                passes=args.matte_edge_passes,
            )
        )
        print(f"frame {idx:03d}: {path}", file=sys.stderr)

    write_akim(
        args.out,
        out_w,
        out_h,
        packed,
        delay_ms,
        loop=not args.no_loop,
        argb8888=True,
    )
    print(
        f"wrote {args.out}: {len(packed)} frames, {out_w}x{out_h}, "
        f"{delay_ms}ms, matte_edge<={args.matte_edge_threshold}"
        f"x{args.matte_edge_passes}, ARGB8888",
        file=sys.stderr,
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--input", required=True, help="MP4/video path, PNG dir, or PNG glob")
    ap.add_argument("--out", help="output asking.akim path")
    ap.add_argument("--preview-out", help="write ROI contact-sheet preview PNG")
    ap.add_argument("--roi", help="pixel ROI as x,y,w,h")
    ap.add_argument("--roi-percent", help="percent ROI as x,y,w,h; 0..1 also accepted")
    ap.add_argument("--fps", type=int, default=10,
                    help="output animation fps; 10/12 are conservative, 15/20 are smoother")
    ap.add_argument("--start-sec", type=float, default=0.0)
    ap.add_argument("--duration-sec", type=float)
    ap.add_argument("--width", type=int, help="output AKIM frame width")
    ap.add_argument("--height", type=int, help="output AKIM frame height")
    ap.add_argument("--matte-edge-threshold", type=int, default=64,
                    help="clear dark pixels touching transparent edges; "
                         "0 disables, default 64")
    ap.add_argument("--matte-edge-passes", type=int, default=5,
                    help="edge-matte cleanup passes; default 5")
    ap.add_argument("--no-loop", action="store_true")
    ap.add_argument("--max-preview-frames", type=int, default=12)
    ap.add_argument("--preview-tile-width", type=int, default=320)
    ap.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg")
    args = ap.parse_args()

    if not args.out and not args.preview_out:
        raise SystemExit("nothing to do: pass --preview-out and/or --out")
    if args.start_sec < 0:
        raise SystemExit("--start-sec must be >= 0")
    if args.fps <= 0:
        raise SystemExit("--fps must be positive")
    if args.matte_edge_threshold < 0 or args.matte_edge_threshold > 255:
        raise SystemExit("--matte-edge-threshold must be 0..255")
    if args.matte_edge_passes < 0:
        raise SystemExit("--matte-edge-passes must be >= 0")
    if args.duration_sec is not None and args.duration_sec <= 0:
        raise SystemExit("--duration-sec must be positive")
    if args.max_preview_frames <= 0:
        raise SystemExit("--max-preview-frames must be positive")
    if args.preview_tile_width <= 0:
        raise SystemExit("--preview-tile-width must be positive")
    if (args.width is None) ^ (args.height is None):
        raise SystemExit("--width and --height must be provided together")
    if args.width is not None and (args.width <= 0 or args.height <= 0):
        raise SystemExit("--width/--height must be positive")

    temp_holder = None
    try:
        frames, temp_holder = load_source_frames(args)
        src_w, src_h, _rgba = read_png_rgba(frames[0])
        roi = roi_from_args(args, src_w, src_h, require=bool(args.out))
        if args.preview_out:
            write_preview(frames, roi, args.preview_out,
                          args.max_preview_frames, args.preview_tile_width)
            print(
                f"wrote preview {args.preview_out}: source={src_w}x{src_h} "
                f"roi={roi[0]},{roi[1]},{roi[2]},{roi[3]} frames={len(frames)}",
                file=sys.stderr,
            )
        if args.out:
            build_akim(frames, roi, args)
    finally:
        if temp_holder is not None:
            temp_holder.cleanup()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, zlib.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)

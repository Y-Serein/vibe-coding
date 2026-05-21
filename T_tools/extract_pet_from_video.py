#!/usr/bin/env python3
"""Extract a pet sprite frame sequence from a raw H.264 .264 clip.

The aikb LCD UI renders pet animations as .akim containers (raw RGBA
frames). This tool grabs frames from one of the existing videos
(sleep.264 / voice.264 / asking.264 / etc.), keys out the dark
background using luminance, crops to the sprite region, and writes
PNG-RGBA frames suitable for png_frames_to_akim.py.

Pipeline (landscape user-facing orientation):

    .264 (panel: 412x960)
      -> ffmpeg transpose=2 + fps select  -> 960x412 PNG frames
      -> crop ROI (default: 340,20,320,180 — sleep.264 mascot box)
      -> luminance-based soft alpha (saturation-aware fallback)
      -> resize to output WxH (default 96x54 to match asking.akim)
      -> PNG-RGBA frames at <out-dir>/frame_NNNN.png

Hand the resulting directory to png_frames_to_akim.py to build the
.akim, e.g.

    python3 png_frames_to_akim.py \\
        --in '/tmp/standby/frame_*.png' \\
        --out standby.akim \\
        --frame-delay-ms 120 --argb8888
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image
import numpy as np


def parse_roi(s):
    parts = [int(p) for p in s.split(",")]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("ROI must be x,y,w,h")
    return tuple(parts)


def parse_size(s):
    parts = [int(p) for p in s.lower().split("x")]
    if len(parts) != 2:
        raise argparse.ArgumentTypeError("size must be WxH")
    return tuple(parts)


def keyed_alpha(rgb, threshold, gain):
    """Compute alpha channel: luminance-based, saturation-boosted.

    Pure-black background pixels go to alpha=0; saturated coloured
    pixels (the mascot's gold lines and warm-bellied fill) get alpha
    pushed to 255 even if they are dim, so the sprite outline keeps
    its weight after compositing onto the dark dashboard background.
    """
    r = rgb[:, :, 0].astype(np.int32)
    g = rgb[:, :, 1].astype(np.int32)
    b = rgb[:, :, 2].astype(np.int32)
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    sat = mx - mn  # 0..255, simple HSV-S proxy
    lum = (r + g + b) // 3

    # Below threshold luminance AND low saturation -> background.
    a = np.where((lum < threshold) & (sat < threshold // 2), 0,
                 np.clip(lum.astype(np.int32) * gain // 100, 0, 255))
    # Boost saturated pixels regardless of luminance.
    a = np.where(sat > threshold, np.clip(a + sat, 0, 255), a)
    return a.astype(np.uint8)


def process_frame(png_in, png_out, roi, out_size, threshold, gain):
    im = Image.open(png_in).convert("RGB")
    x, y, w, h = roi
    if x + w > im.width or y + h > im.height:
        raise SystemExit(
            f"{png_in}: ROI {roi} exceeds frame {im.width}x{im.height}"
        )
    cropped = im.crop((x, y, x + w, y + h))
    rgb = np.array(cropped)
    alpha = keyed_alpha(rgb, threshold, gain)
    rgba = np.dstack([rgb, alpha])
    out = Image.fromarray(rgba, mode="RGBA")
    # out_size == (0, 0) means "keep ROI pixels as-is" — required for the
    # pet sprite to stay crisp; the on-board pet renderer uses integer
    # scaling, so it is better to give it the right resolution up front
    # than to soft-resize here.
    if out_size != (0, 0) and (out.width, out.height) != out_size:
        out = out.resize(out_size, Image.LANCZOS)
        a = np.array(out)[:, :, 3]
        a = np.where(a < 12, 0, a).astype(np.uint8)
        rgba = np.dstack([np.array(out)[:, :, :3], a])
        out = Image.fromarray(rgba, mode="RGBA")
    out.save(png_out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True, help="input raw .264 clip")
    ap.add_argument("--out-dir", required=True, help="PNG frame output dir")
    ap.add_argument("--roi", type=parse_roi,
                    default=(340, 20, 320, 180),
                    help="x,y,w,h in landscape orientation (default: "
                         "340,20,320,180 — sleep.264 mascot box)")
    ap.add_argument("--size", type=parse_size, default=(0, 0),
                    help="output PNG size WxH. Default 0x0 means keep ROI "
                         "native pixels (no resize, sprite stays sharp). "
                         "Set explicitly to force a resize, e.g. 96x54 to "
                         "match the old asking.akim layout.")
    ap.add_argument("--fps", type=int, default=8,
                    help="frames per second to sample from source video")
    ap.add_argument("--max-frames", type=int, default=24,
                    help="max number of frames written (default 24)")
    ap.add_argument("--threshold", type=int, default=24,
                    help="luminance threshold for background keying")
    ap.add_argument("--gain", type=int, default=180,
                    help="alpha gain percentage applied to non-background "
                         "pixels (default 180)")
    ap.add_argument("--transpose", type=int, default=2, choices=[0, 1, 2, 3],
                    help="ffmpeg transpose direction: 0=none, 1=90°CW, "
                         "2=90°CCW (default; correct for sleep/voice), "
                         "3=90°CW+vflip")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    raw_dir = out_dir / "_raw"
    raw_dir.mkdir()

    # Step 1: pull RGB frames at <fps> in landscape orientation.
    transpose_filter = (
        f"transpose={args.transpose}," if args.transpose != 0 else ""
    )
    vf = f"{transpose_filter}fps={args.fps}"
    cmd = [
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-i", args.video,
        "-vf", vf,
        "-frames:v", str(args.max_frames),
        str(raw_dir / "raw_%04d.png"),
    ]
    subprocess.run(cmd, check=True)

    raws = sorted(raw_dir.glob("raw_*.png"))
    if not raws:
        raise SystemExit(f"no frames extracted from {args.video}")
    print(f"extracted {len(raws)} raw frames from {args.video}",
          file=sys.stderr)

    # Step 2: crop / key / resize.
    for idx, raw in enumerate(raws, start=1):
        out = out_dir / f"frame_{idx:04d}.png"
        process_frame(raw, out, args.roi, args.size,
                      args.threshold, args.gain)
    print(f"wrote {len(raws)} sprite frames to {out_dir}",
          file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())

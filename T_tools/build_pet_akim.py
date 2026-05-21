#!/usr/bin/env python3
"""Build a pet AKIM with a small breathing / floating animation from one sprite.

The source .264 clips are all single-frame static videos, so the
existing pet sprite art has zero motion. To get a 'living' feel
without redrawing artwork, this tool takes one sprite PNG and
synthesises an N-frame loop where the sprite drifts a couple of
pixels and pulses softly. Three styles are supported:

  - breathe : vertical bob, ±1 px, slow sine
  - drift   : horizontal sway, ±1 px, slow sine
  - blink   : alpha pulse on a sub-region (good for 'zzz' or
              status icons)

Output is a .akim container (ARGB8888 / loop on) consumed by
aikb_lcd_ui's pet renderer at /mnt/system/usr/share/aikb/pet/*.akim.
"""

import argparse
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

SCRIPTS_DIR = (
    Path(__file__).resolve().parent.parent
    / "middleware" / "v2" / "sample" / "aikb_lcd_ui" / "scripts"
)
sys.path.insert(0, str(SCRIPTS_DIR))
import akim_common  # noqa: E402


def shift(rgba, dx, dy):
    """Translate an RGBA frame by (dx, dy), filling exposed pixels with alpha=0."""
    h, w = rgba.shape[:2]
    out = np.zeros_like(rgba)
    src_x0 = max(0, -dx)
    src_y0 = max(0, -dy)
    src_x1 = min(w, w - dx)
    src_y1 = min(h, h - dy)
    dst_x0 = max(0, dx)
    dst_y0 = max(0, dy)
    dst_x1 = dst_x0 + (src_x1 - src_x0)
    dst_y1 = dst_y0 + (src_y1 - src_y0)
    out[dst_y0:dst_y1, dst_x0:dst_x1] = rgba[src_y0:src_y1, src_x0:src_x1]
    return out


def synth_breathe(rgba, count, amp=1):
    frames = []
    for i in range(count):
        phase = math.sin(2.0 * math.pi * i / count)
        dy = int(round(phase * amp))
        frames.append(shift(rgba, 0, dy))
    return frames


def synth_drift(rgba, count, amp=1):
    frames = []
    for i in range(count):
        phase = math.sin(2.0 * math.pi * i / count)
        dx = int(round(phase * amp))
        frames.append(shift(rgba, dx, 0))
    return frames


def synth_blink(rgba, count, region):
    """Pulse alpha on a rectangle (x, y, w, h), keeping body opaque."""
    x, y, w, h = region
    frames = []
    for i in range(count):
        phase = (math.sin(2.0 * math.pi * i / count) + 1.0) * 0.5
        # phase in [0..1]; scale alpha of region by 0.25..1.0
        scale = 0.25 + 0.75 * phase
        copy = rgba.copy()
        sub = copy[y:y + h, x:x + w, 3].astype(np.float32)
        copy[y:y + h, x:x + w, 3] = np.clip(sub * scale, 0, 255).astype(np.uint8)
        frames.append(copy)
    return frames


def parse_region(s):
    parts = [int(p) for p in s.split(",")]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("region must be x,y,w,h")
    return tuple(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sprite", required=True, help="input sprite PNG (RGBA)")
    ap.add_argument("--out", required=True, help="output .akim path")
    ap.add_argument("--style", default="breathe",
                    choices=["breathe", "drift", "blink", "static"])
    ap.add_argument("--frames", type=int, default=24,
                    help="number of frames in the akim loop (default 24, "
                         "smooth at ~333ms/frame for the standard 8s loop)")
    ap.add_argument("--frame-delay-ms", type=int, default=0,
                    help="per-frame delay in ms. 0 (default) derives it "
                         "from --total-duration-ms / --frames so the loop "
                         "matches the source video length.")
    ap.add_argument("--total-duration-ms", type=int, default=8000,
                    help="target total loop length in ms; combined with "
                         "--frames decides --frame-delay-ms when that one "
                         "is 0. Defaults to 8000 (the 192-frame 24fps "
                         "source clips' duration).")
    ap.add_argument("--amplitude", type=int, default=1,
                    help="pixel amplitude for breathe/drift")
    ap.add_argument("--blink-region", type=parse_region,
                    default=(64, 0, 32, 24),
                    help="x,y,w,h region for blink style (default upper-right "
                         "corner where zzz/icons usually live)")
    args = ap.parse_args()

    im = Image.open(args.sprite).convert("RGBA")
    rgba = np.array(im)

    frame_delay = args.frame_delay_ms
    if frame_delay <= 0:
        if args.total_duration_ms <= 0 or args.frames <= 0:
            raise SystemExit(
                "must give either --frame-delay-ms or "
                "--total-duration-ms + --frames"
            )
        frame_delay = max(1, args.total_duration_ms // args.frames)

    if args.style == "breathe":
        frames = synth_breathe(rgba, args.frames, args.amplitude)
    elif args.style == "drift":
        frames = synth_drift(rgba, args.frames, args.amplitude)
    elif args.style == "blink":
        frames = synth_blink(rgba, args.frames, args.blink_region)
    elif args.style == "static":
        frames = [rgba.copy() for _ in range(args.frames)]
    else:
        raise SystemExit(f"unknown style {args.style}")

    width, height = im.width, im.height
    payload = [f.tobytes() for f in frames]
    akim_common.write_akim(
        args.out, width, height, payload, frame_delay,
        loop=True, argb8888=True,
    )
    total_ms = frame_delay * len(frames)
    print(f"wrote {args.out}: {len(frames)}x {width}x{height} style={args.style}"
          f" delay={frame_delay}ms total={total_ms}ms",
          file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())

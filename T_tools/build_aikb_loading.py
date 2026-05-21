#!/usr/bin/env python3
"""Build the host-transition loading artefacts.

Generates a sleep.264-styled "INCOMING / host data" landscape PNG and emits:
  - loading.264      : H.264 Constrained Baseline 412x960 yuv420p 24fps,
                       FIFO-hot-switched on top of sample_vdecvo right before
                       the fb<->mipi-tx mode swap, so the user never sees the
                       2 s black gap.
  - loading_fb.bin   : 412x960 ARGB8888 panel-native bytes, dd'd onto /dev/fb0
                       the instant soph_fb is reloaded so the panel re-init
                       reads the loading frame instead of the green test
                       pattern.

The PNG is the single source of truth; the .264 and the _fb.bin are both
derived from it so the user sees the same image across the swap.
"""

import argparse
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

CW, CH = 960, 412
BG = (8, 8, 6)
GOLD = (244, 184, 61)
GOLD_BRIGHT = (255, 194, 51)
DIM = (150, 134, 108)

FONT_DIR = "/usr/share/fonts/truetype/dejavu"
F_BIG = f"{FONT_DIR}/DejaVuSansMono-Bold.ttf"
F_REG = f"{FONT_DIR}/DejaVuSansMono.ttf"


def render_landscape_png(out_path):
    img = Image.new("RGB", (CW, CH), BG)
    draw = ImageDraw.Draw(img)

    f_big = ImageFont.truetype(F_BIG, 120)
    f_sub = ImageFont.truetype(F_REG, 26)
    f_tag = ImageFont.truetype(F_BIG, 20)
    f_small = ImageFont.truetype(F_REG, 16)

    # Header band
    draw.line([(28, 46), (CW - 28, 46)], fill=GOLD, width=1)
    draw.text((36, 18), "AIKB · LOADING", fill=GOLD, font=f_tag)
    state = "HOST"
    sb = draw.textbbox((0, 0), state, font=f_tag, anchor="lt")
    draw.text((CW - 36 - (sb[2] - sb[0]), 18), state, fill=GOLD,
              font=f_tag, anchor="lt")

    # Big center label
    label = "INCOMING"
    sub = "host data ..."
    bb = draw.textbbox((0, 0), label, font=f_big, anchor="lt")
    lw = bb[2] - bb[0]
    lh_visual = int(120 * 1.18)
    sb = draw.textbbox((0, 0), sub, font=f_sub, anchor="lt")
    sub_w = sb[2] - sb[0]
    sub_h = sb[3] - sb[1]

    body_top, body_bottom = 60, CH - 50
    gap = 28
    stack_h = lh_visual + gap + sub_h
    label_y = body_top + ((body_bottom - body_top) - stack_h) // 2
    label_x = (CW - lw) // 2
    draw.text((label_x, label_y), label, fill=GOLD_BRIGHT, font=f_big,
              anchor="lt")
    draw.text(((CW - sub_w) // 2, label_y + lh_visual + gap // 2),
              sub, fill=DIM, font=f_sub, anchor="lt")

    # Footer
    draw.line([(28, CH - 44), (CW - 28, CH - 44)], fill=GOLD, width=1)
    draw.text((36, CH - 28), "preparing dashboard", fill=DIM, font=f_small)
    bullets = "● ● ●"
    bb = draw.textbbox((0, 0), bullets, font=f_small, anchor="lt")
    draw.text((CW - 36 - (bb[2] - bb[0]), CH - 28), bullets, fill=GOLD,
              font=f_small, anchor="lt")

    img.save(out_path)
    return img


def encode_h264(png_path, out_path, duration_s="3"):
    subprocess.run(
        [
            "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
            "-loop", "1", "-framerate", "24", "-t", duration_s,
            "-i", str(png_path),
            "-vf", "transpose=1,format=yuv420p",
            "-r", "24",
            "-c:v", "libx264",
            "-profile:v", "baseline",
            "-level", "3.1",
            "-pix_fmt", "yuv420p",
            "-x264-params", "keyint=24:min-keyint=24:scenecut=0",
            "-an",
            "-f", "h264",
            str(out_path),
        ],
        check=True,
    )


def pack_panel_fb(landscape_img, out_path, alpha=255):
    """Pre-rotate 960x412 RGB -> 412x960 ARGB8888 panel-native bytes."""
    arr = np.array(landscape_img.convert("RGB"))
    assert arr.shape == (CH, CW, 3), arr.shape
    # ROT_CW (transpose=1 / numpy k=-1) matches what fb_blit produces with
    # --rotate auto on this panel; see make_splash_fb.py for derivation.
    fb_rgb = np.rot90(arr, k=-1)
    assert fb_rgb.shape == (CW, CH, 3), fb_rgb.shape
    a = np.full(fb_rgb.shape[:2], alpha, dtype=np.uint32)
    r = fb_rgb[:, :, 0].astype(np.uint32)
    g = fb_rgb[:, :, 1].astype(np.uint32)
    b = fb_rgb[:, :, 2].astype(np.uint32)
    pix = (a << 24) | (r << 16) | (g << 8) | b  # 0xAARRGGBB
    payload = pix.astype("<u4").tobytes()
    expected = CH * CW * 4
    if len(payload) != expected:
        raise SystemExit(f"unexpected payload {len(payload)} != {expected}")
    Path(out_path).write_bytes(payload)


def main():
    p = argparse.ArgumentParser()
    p.add_argument(
        "--video-dir",
        default="/home/rv_nano/AIKB/LicheeRV-Nano-Build/buildroot/board/cvitek/SG200X/overlay/mnt/system/usr/share/aikb/video",
    )
    p.add_argument(
        "--fb-dir",
        default="/home/rv_nano/AIKB/LicheeRV-Nano-Build/buildroot/board/cvitek/SG200X/overlay/mnt/system/usr/share/aikb",
    )
    p.add_argument("--png", default="/tmp/aikb_key_ui_png/loading.png")
    p.add_argument("--duration", default="3")
    args = p.parse_args()

    Path(args.video_dir).mkdir(parents=True, exist_ok=True)
    Path(args.fb_dir).mkdir(parents=True, exist_ok=True)
    Path(args.png).parent.mkdir(parents=True, exist_ok=True)

    img = render_landscape_png(args.png)
    encode_h264(args.png, Path(args.video_dir) / "loading.264", args.duration)
    pack_panel_fb(img, Path(args.fb_dir) / "loading_fb.bin")
    print("wrote loading.264 and loading_fb.bin")


if __name__ == "__main__":
    main()

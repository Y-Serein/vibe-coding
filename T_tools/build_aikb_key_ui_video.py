#!/usr/bin/env python3
"""Build raw H.264 'key UI' videos in the sleep.264 visual style.

Composition is done in 960x412 landscape (the same logical canvas sleep.264
uses); ffmpeg then transposes to the 412x960 panel orientation so the user,
holding the device in its landscape pose, reads the text in its natural
direction.

Output format matches sleep.264 exactly:
  H.264 Constrained Baseline, 412x960, yuv420p, 24fps.
"""

import argparse
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# Landscape canvas (what the user sees while holding the device landscape).
CW, CH = 960, 412
BG = (8, 8, 6)
GOLD = (244, 184, 61)
GOLD_BRIGHT = (255, 194, 51)
DIM = (150, 134, 108)
FAINT = (90, 82, 66)

FONT_DIR = "/usr/share/fonts/truetype/dejavu"
F_MONO_BOLD = f"{FONT_DIR}/DejaVuSansMono-Bold.ttf"
F_MONO = f"{FONT_DIR}/DejaVuSansMono.ttf"

KEYS = [
    # (basename,            label,         subtitle,            state_tag)
    ("reject_ui",           "REJECT",      "cancel / back",     "STOP"),
    ("session_ui",          "SESSION",     "task panel",        "PANEL"),
    ("vote_review_ui",      "REVIEW",      "vote / review",     "VOTE"),
    ("agent_model_ui",      "MODEL",       "agent / model",     "PICK"),
    ("multi_ui",            "MULTI",       "multi-function",    "MORE"),
    ("confirm_ui",          "CONFIRM",     "apply / send",      "GO"),
    ("menu_debug_ui",       "MENU",        "menu / debug",      "MENU"),
]


def text_size(draw, text, font):
    bbox = draw.textbbox((0, 0), text, font=font)
    return bbox[2] - bbox[0], bbox[3] - bbox[1]


def render_png(label, subtitle, state_tag, out_path):
    img = Image.new("RGB", (CW, CH), BG)
    draw = ImageDraw.Draw(img)

    # Scale big label down for long words so 7-char "CONFIRM"/"SESSION" still fit.
    big_size = 140 if len(label) <= 5 else (120 if len(label) <= 6 else 104)
    f_big = ImageFont.truetype(F_MONO_BOLD, big_size)
    f_sub = ImageFont.truetype(F_MONO, 26)
    f_tag = ImageFont.truetype(F_MONO_BOLD, 20)
    f_small = ImageFont.truetype(F_MONO, 16)
    f_state = ImageFont.truetype(F_MONO_BOLD, 20)

    # Header band (top of landscape canvas).
    draw.line([(28, 46), (CW - 28, 46)], fill=GOLD, width=1)
    # Left label "AIKB · <label>"
    head_left = f"AIKB · {label}"
    draw.text((36, 18), head_left, fill=GOLD, font=f_tag)
    # Right state tag
    sw, _ = text_size(draw, state_tag, f_state)
    draw.text((CW - 36 - sw, 18), state_tag, fill=GOLD, font=f_state)

    # Big center label, optically centered in the body band.
    # We measure with anchor='lt' (left-top) so dy/dh stay predictable.
    big_bbox = draw.textbbox((0, 0), label, font=f_big, anchor="lt")
    lw = big_bbox[2] - big_bbox[0]
    lh = big_bbox[3] - big_bbox[1]
    sub_bbox = draw.textbbox((0, 0), subtitle, font=f_sub, anchor="lt")
    sub_w = sub_bbox[2] - sub_bbox[0]
    sub_h = sub_bbox[3] - sub_bbox[1]

    # Use a generous visual line height so the subtitle never clips descenders.
    lh_visual = int(big_size * 1.18)
    body_top, body_bottom = 60, CH - 50
    gap = 28
    stack_h = lh_visual + gap + sub_h
    label_y = body_top + ((body_bottom - body_top) - stack_h) // 2
    label_x = (CW - lw) // 2
    draw.text((label_x, label_y), label, fill=GOLD_BRIGHT, font=f_big,
              anchor="lt")

    draw.text(((CW - sub_w) // 2, label_y + lh_visual + gap // 2),
              subtitle, fill=DIM, font=f_sub, anchor="lt")

    # Footer band
    draw.line([(28, CH - 44), (CW - 28, CH - 44)], fill=GOLD, width=1)
    draw.text((36, CH - 28), "press any key", fill=DIM, font=f_small)
    foot_r = "● ● ●"
    rw, _ = text_size(draw, foot_r, f_small)
    draw.text((CW - 36 - rw, CH - 28), foot_r, fill=GOLD, font=f_small)

    img.save(out_path)


def encode_h264(png_path, out_path, duration_s, transpose):
    vf = f"transpose={transpose},format=yuv420p"
    cmd = [
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-loop", "1", "-framerate", "24", "-t", str(duration_s),
        "-i", str(png_path),
        "-vf", vf,
        "-r", "24",
        "-c:v", "libx264",
        "-profile:v", "baseline",
        "-level", "3.1",
        "-pix_fmt", "yuv420p",
        "-x264-params", "keyint=24:min-keyint=24:scenecut=0",
        "-an",
        "-f", "h264",
        str(out_path),
    ]
    subprocess.run(cmd, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--out-dir",
        default="/home/rv_nano/AIKB/LicheeRV-Nano-Build/buildroot/board/cvitek/SG200X/overlay/mnt/system/usr/share/aikb/video",
        help="overlay video directory (will be synced to output/install separately)",
    )
    parser.add_argument(
        "--png-dir",
        default="/tmp/aikb_key_ui_png",
        help="intermediate PNG dir (960x412 landscape source)",
    )
    parser.add_argument("--duration", default="2", help="output duration seconds")
    parser.add_argument("--only", default="",
                        help="comma-separated basenames to render (default: all)")
    parser.add_argument("--transpose", default="1", choices=["1", "2"],
                        help="ffmpeg transpose: 1=90°CW, 2=90°CCW")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    png_dir = Path(args.png_dir)
    png_dir.mkdir(parents=True, exist_ok=True)

    only = {s.strip() for s in args.only.split(",") if s.strip()}
    for basename, label, subtitle, state_tag in KEYS:
        if only and basename not in only:
            continue
        png_path = png_dir / f"{basename}.png"
        h264_path = out_dir / f"{basename}.264"
        print(f"render {basename}: label={label!r} sub={subtitle!r}")
        render_png(label, subtitle, state_tag, png_path)
        encode_h264(png_path, h264_path, args.duration, args.transpose)
        print(f"  wrote {h264_path}")


if __name__ == "__main__":
    sys.exit(main())

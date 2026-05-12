#!/usr/bin/env python3
"""Convert a landscape PNG into a panel-native YV12 C header for u-boot.

The GC9503 panel on LicheeRV-Nano is 412x960 portrait. The Vibe Coding promo
artwork is authored as 960x412 landscape PNGs. This tool:

  1. Loads the PNG (RGB).
  2. Resizes to 960x412 if needed.
  3. Rotates 90 deg CW into panel-native 412x960 portrait orientation -- this
     matches the (px = PANEL_W - 1 - y, py = x) mapping baked into
     u-boot-2021.10/cmd/cvi_vo.c::gc9503_put_yuv_pixel().
  4. Converts RGB -> YUV using BT.601 limited range -- same constants as
     gc9503_rgb_to_yuv() so the in-board CSC (SCL_CSC_601_LIMIT_YUV2RGB) is
     correct.
  5. 4:2:0 chroma downsamples (2x2 box average).
  6. Emits a single .h with three `static const unsigned char` planes, packed
     tight (Y stride = 412, U/V stride = 206) -- the consumer in cvi_vo.c
     copies row-by-row into pitch_y=ALIGN(412,32)=416 / pitch_c=ALIGN(206,32)=224
     buffers.
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image


PANEL_W = 412   # GC9503_PANEL_W
PANEL_H = 960   # GC9503_PANEL_H
LAND_W = PANEL_H
LAND_H = PANEL_W


def rgb_to_yuv_601_limited(rgb):
    r = rgb[:, :, 0].astype(np.int32)
    g = rgb[:, :, 1].astype(np.int32)
    b = rgb[:, :, 2].astype(np.int32)
    y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16
    u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128
    v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128
    return (
        np.clip(y, 0, 255).astype(np.uint8),
        np.clip(u, 0, 255).astype(np.uint8),
        np.clip(v, 0, 255).astype(np.uint8),
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True, help="input PNG")
    ap.add_argument("--out", dest="dst", required=True, help="output .h")
    ap.add_argument("--symbol", default="IMG_9872_YV12",
                    help="C symbol prefix (e.g. FOO -> FOO_Y/_U/_V)")
    ap.add_argument("--guard", default="CVI_UBOOT_SPLASH_H",
                    help="header include-guard macro")
    args = ap.parse_args()

    im = Image.open(args.src).convert("RGB")
    if im.size != (LAND_W, LAND_H):
        print(f"WARN: {args.src}: {im.size} != {LAND_W}x{LAND_H}; resizing",
              file=sys.stderr)
        im = im.resize((LAND_W, LAND_H), Image.LANCZOS)

    # CW 90 deg into panel native portrait (PIL rotate(-90) = CW 90, expand=True
    # keeps full pixels). Result: PANEL_W x PANEL_H = 412 x 960.
    im_rot = im.rotate(-90, expand=True)
    if im_rot.size != (PANEL_W, PANEL_H):
        raise SystemExit(f"unexpected rotated size {im_rot.size}")
    arr = np.array(im_rot)            # (PANEL_H, PANEL_W, 3)
    y, u, v = rgb_to_yuv_601_limited(arr)

    # 4:2:0 chroma: 2x2 box average
    u_ds = (u.reshape(PANEL_H // 2, 2, PANEL_W // 2, 2)
              .mean(axis=(1, 3)).astype(np.uint8))
    v_ds = (v.reshape(PANEL_H // 2, 2, PANEL_W // 2, 2)
              .mean(axis=(1, 3)).astype(np.uint8))

    planes = [
        ("Y", y.tobytes(), PANEL_W, PANEL_H),
        ("U", u_ds.tobytes(), PANEL_W // 2, PANEL_H // 2),
        ("V", v_ds.tobytes(), PANEL_W // 2, PANEL_H // 2),
    ]

    with open(args.dst, "w") as f:
        f.write(f"/* Auto-generated from {os.path.basename(args.src)} by "
                f"tools/cvi_make_splash.py. */\n")
        f.write(f"/* Panel-native YV12, {PANEL_W}x{PANEL_H} portrait, "
                f"BT.601 limited range. */\n")
        f.write(f"/* Do NOT edit manually -- regenerate by re-running the "
                f"script. */\n")
        f.write(f"#ifndef {args.guard}\n")
        f.write(f"#define {args.guard}\n\n")
        for name, data, w, h in planes:
            f.write(f"/* {args.symbol}_{name}: {w}x{h} */\n")
            f.write(f"static const unsigned char {args.symbol}_{name}"
                    f"[{len(data)}] = {{\n")
            for i in range(0, len(data), 16):
                chunk = data[i:i + 16]
                f.write("\t" + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
            f.write("};\n\n")
        f.write(f"#endif /* {args.guard} */\n")

    total = sum(len(d) for _, d, _, _ in planes)
    print(f"wrote {args.dst}: Y={len(planes[0][1])} U={len(planes[1][1])} "
          f"V={len(planes[2][1])} total={total} bytes ({total/1024:.0f} KB)",
          file=sys.stderr)


if __name__ == "__main__":
    main()

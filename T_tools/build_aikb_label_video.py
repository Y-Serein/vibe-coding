#!/usr/bin/env python3
"""Build a board-ready raw H.264 status video in the IMG_9884 shell style."""

import argparse
import subprocess
from pathlib import Path


def run(cmd):
    subprocess.run(cmd, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--shell",
        default="/home/rv_nano/Sipeed/rv_nano/tools/vibe-bridge/images/IMG_9884.PNG",
        help="960x412 static UI shell reference",
    )
    parser.add_argument("--label", required=True, help="large center label")
    parser.add_argument("--sub", default="", help="optional smaller subtitle")
    parser.add_argument("--out", required=True, help="output raw .264 file")
    parser.add_argument("--preview", default=None, help="optional landscape preview PNG")
    parser.add_argument("--duration", default="8", help="output duration seconds")
    args = parser.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    safe_label = args.label.replace("\\", "\\\\").replace(":", "\\:").replace("'", "\\'")
    safe_sub = args.sub.replace("\\", "\\\\").replace(":", "\\:").replace("'", "\\'")
    subtitle = ""
    if safe_sub:
        subtitle = (
            ",drawtext=text='{}':x=(w-text_w)/2:y=254:"
            "fontcolor=0xf2dfb0:fontsize=24:box=0"
        ).format(safe_sub)

    compose_landscape = (
        "[0:v]scale=960:412:flags=lanczos,"
        "drawbox=x=258:y=201:w=444:h=92:color=0x070707d0:t=fill,"
        "drawbox=x=258:y=201:w=444:h=92:color=0xf4b83daa:t=2,"
        "drawbox=x=305:y=239:w='350*(0.5+0.5*sin(2*PI*t/2))':h=4:color=0xffc233:t=fill,"
        "drawbox=x='292+12*sin(2*PI*t)':y=232:w=12:h=12:color=0xffc233:t=fill,"
        "drawbox=x='656-12*sin(2*PI*t)':y=232:w=12:h=12:color=0xffc233:t=fill,"
        "drawtext=text='[  {}  ]':x=(w-text_w)/2:y=214:"
        "fontcolor=0xffc233:fontsize=42:box=0"
        "{}"
        ",format=yuv420p"
    ).format(safe_label, subtitle)
    encode_filter = compose_landscape + ",transpose=1"

    run(
        [
            "ffmpeg",
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-loop",
            "1",
            "-framerate",
            "24",
            "-t",
            args.duration,
            "-i",
            args.shell,
            "-filter_complex",
            encode_filter,
            "-r",
            "24",
            "-c:v",
            "libx264",
            "-profile:v",
            "baseline",
            "-level",
            "3.1",
            "-pix_fmt",
            "yuv420p",
            "-x264-params",
            "keyint=24:min-keyint=24:scenecut=0",
            "-an",
            "-f",
            "h264",
            str(out),
        ]
    )

    if args.preview:
        run(
            [
                "ffmpeg",
                "-y",
                "-hide_banner",
                "-loglevel",
                "error",
                "-loop",
                "1",
                "-framerate",
                "24",
                "-t",
                "0.05",
                "-i",
                args.shell,
                "-filter_complex",
                compose_landscape,
                "-frames:v",
                "1",
                args.preview,
            ]
        )


if __name__ == "__main__":
    main()

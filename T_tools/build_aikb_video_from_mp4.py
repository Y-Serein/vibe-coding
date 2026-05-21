#!/usr/bin/env python3
"""Compose the AIKB static shell with a cropped center MP4 video."""

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
    parser.add_argument(
        "--video",
        default="/home/rv_nano/Sipeed/rv_nano/tools/vibe-bridge/images/new/vedio_asking.mp4",
        help="source animation video",
    )
    parser.add_argument("--out", required=True, help="output raw .264 file")
    parser.add_argument(
        "--preview",
        default=None,
        help="optional landscape preview PNG after composition",
    )
    parser.add_argument(
        "--encoded-preview",
        default=None,
        help="optional 412x960 encoded-orientation preview PNG",
    )
    parser.add_argument("--duration", default="8", help="output duration seconds")
    args = parser.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    # Source videos are 1920x1080 and already contain a full UI.  Crop away
    # its own header/footer and side borders, keep the central pet/title/status
    # content, fit it into IMG_9884's content band without changing aspect
    # ratio, then rotate into the 412x960 orientation expected by the current
    # sample_vdecvo path.
    center = (
        "[1:v]"
        "crop=1920:660:0:220,"
        "scale=-2:290:flags=lanczos"
        "[center]"
    )
    compose_landscape = (
        "[0:v]scale=960:412:flags=lanczos[base];"
        f"{center};"
        "[base][center]overlay=(W-w)/2:55:shortest=1,"
        "format=yuv420p"
    )
    encode_filter = compose_landscape + ",transpose=1"

    cmd = [
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
        "-i",
        args.video,
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
    run(cmd)

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
                "-ss",
                "2",
                "-i",
                args.video,
                "-filter_complex",
                compose_landscape,
                "-frames:v",
                "1",
                args.preview,
            ]
        )

    if args.encoded_preview:
        run(
            [
                "ffmpeg",
                "-y",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                str(out),
                "-frames:v",
                "1",
                args.encoded_preview,
            ]
        )


if __name__ == "__main__":
    main()

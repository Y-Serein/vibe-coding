# AIKB LCD UI sample

This sample renders a 960x412 landscape UI for the GC9503 412x960 LCD when the
panel is mounted sideways. It writes directly to a Linux framebuffer. The
default view is a VT100-compatible terminal surface fed by Type-C serial bytes;
the previous JSON dashboard remains available with `--view dashboard`.

## Build

```sh
cd /home/rv_nano/AIKB/LicheeRV-Nano-Build/middleware/v2/sample/aikb_lcd_ui
make
```

The normal SDK `middleware/v2/sample/Makefile` also picks this directory up when
the SDK sample build environment is active.

## Run

Render once to an image for local inspection:

```sh
./aikb_lcd_ui --dump-ppm /tmp/aikb-lcd-ui.ppm
```

Run on the board framebuffer with the VT100 terminal view and mock data:

```sh
./aikb_lcd_ui --fb /dev/fb0 --rotate auto
```

For one-shot framebuffer debugging, keep the process alive briefly. The cvifb
driver disables the GOP framebuffer layer when the last `/dev/fb0` fd closes,
so a plain one-frame run may disappear immediately after exit.

```sh
./aikb_lcd_ui --fb /dev/fb0 --rotate auto --once --hold 5000
```

Run with Type-C serial input. On the board this is expected to be the USB gadget
serial endpoint, commonly `/dev/ttyGS0`; on the computer side it appears as a
USB serial device.

```sh
./aikb_lcd_ui --fb /dev/fb0 --input /dev/ttyGS0 --rotate auto --view terminal
```

`--rotate auto` renders the UI as 960x412 and automatically rotates it clockwise
when the framebuffer reports 412x960. Use `--rotate ccw` if the physical mount
is opposite.

The app refreshes cvifb by calling `FBIOPAN_DISPLAY` after mmap writes, which
also lets the driver synchronize the framebuffer memory for the display engine.
If the panel shows the right shapes but the framebuffer overlay looks blended or
transparent, try `--alpha 0`; the default is `--alpha 255`.

For `cvifb` 32 bpp the app defaults to GOP `ARGB8888` byte packing. If colors
look red/blue swapped during low-level testing, compare these modes:

```sh
./aikb_lcd_ui --fb /dev/fb0 --rotate auto --pixel-format argb8888
./aikb_lcd_ui --fb /dev/fb0 --rotate auto --pixel-format abgr8888
./aikb_lcd_ui --fb /dev/fb0 --rotate auto --pixel-format fb
```

## Terminal view

`--view terminal` treats `--input` as a raw UTF-8 terminal byte stream. It
handles the common VT100/ANSI controls used by CLIs:

- C0 controls: CR, LF, BS, TAB, ESC
- cursor movement and positioning: `CSI A/B/C/D/H/f`
- erase controls: `CSI J/K`
- scroll region and line scrolling: `CSI r/L/M`, reverse index
- save/restore cursor: `ESC 7/8`, `CSI s/u`
- SGR colors: reset, bold, inverse, 8/16 colors, and truecolor

Powerline private-use glyphs `U+E0B0`..`U+E0B3` are drawn internally so status
bars render even if the active font lacks those symbols.

For text rendering the app prefers Sarasa/Sorasa-style mono CJK fonts when they
are present, then falls back to the image's WQY/DejaVu fonts and finally to the
built-in 8x16 glyphs. A specific font can be selected with:

```sh
./aikb_lcd_ui --fb /dev/fb0 --rotate auto --font /mnt/system/fonts/SarasaMonoSC-Regular.ttf
```

Useful font paths checked automatically:

- `/usr/share/fonts/sarasa-gothic/SarasaMonoSC-Regular.ttf`
- `/usr/share/fonts/sarasa-gothic/SarasaTermSC-Regular.ttf`
- `/usr/share/fonts/sarasa-gothic/SarasaGothicSC-Regular.ttf`
- `/usr/share/fonts/sarasa/SarasaMonoSC-Regular.ttf`
- `/usr/share/fonts/sarasa/SarasaTermSC-Regular.ttf`
- `/usr/share/fonts/sarasa/SarasaGothicSC-Regular.ttf`
- `/usr/share/fonts/SarasaMonoSC-Regular.ttf`
- `/usr/share/fonts/SarasaTermSC-Regular.ttf`
- `/usr/share/fonts/SarasaGothicSC-Regular.ttf`
- `/usr/share/fonts/sorasa-gothic/SorasaGothic-Regular.ttf`
- `/mnt/system/fonts/SarasaMonoSC-Regular.ttf`
- `/mnt/system/fonts/SarasaTermSC-Regular.ttf`
- `/mnt/system/fonts/SarasaGothicSC-Regular.ttf`
- `/mnt/system/fonts/SorasaGothic-Regular.ttf`
- `/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc`
- `/usr/share/fonts/dejavu/DejaVuSansMono.ttf`

## Dashboard input protocol

`--view dashboard` keeps the older newline-delimited flat JSON protocol. The
parser intentionally accepts one session/update per line and avoids external
JSON dependencies, which keeps it easy to feed from a small Type-C bridge.

Run the dashboard manually:

```sh
./aikb_lcd_ui --fb /dev/fb0 --input /dev/ttyGS0 --rotate auto --view dashboard
```

Summary line:

```json
{"type":"summary","active":3,"window":"5h rolling window","avg_usage":62,"total_spend":41.20,"focus":"WEBAPI"}
```

Session line:

```json
{"type":"session","id":"WEBAPI","session":"WEBAPI","tool":"Codex","mode":"AUTO","model":"gpt-4.1","cost":23.10,"usage":42,"reset":"2h 31m","state":"WAIT","task":"API integration cleanup","now":"editing router and auth middleware"}
```

Claude Code hook style lines are also mapped directly:

```json
{"type":"SessionStart","session_id":"abc","name":"WEBAPI","source":"claude-code","model":"opus-4.7"}
{"type":"PreToolUse","session_id":"abc","tool_name":"Bash","tool_input":"cargo test"}
{"type":"Stop","session_id":"abc"}
```

The UI currently displays these stable fields:

- `session`, `tool`, `mode`, `model`, `cost`, `usage`, `reset`, `state`
- top-level `active`, `window`, `avg_usage`, `total_spend`, `focus`
- bottom focus detail: `task`, `now`, `cost`

This means the later computer-side bridge only needs to transform Claude/Codex
runtime data into the same flat session/update lines.

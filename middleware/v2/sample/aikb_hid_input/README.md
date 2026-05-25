# AIKB HID Input

`aikb_hid_input` scans the AIKB buttons and rotary encoder and bridges them to
the host over the vendor HID interface, using the **vibe-bridge protocol v0**
(see `tools/vibe-bridge/docs/hid_protocol.md`).

## Pin Map

Inputs are configured as GPIO inputs with software pull-up enabled for the
current test board. RTC GPIOE inputs keep the existing RTC ioblk pull-up path
and also program the SDIO1 pad-control addresses from the SG2002 U-Boot
headers. GPIOA22/A23/A24/A25/A27 use the cv181x EMMC pad-control registers at
`0x0300191c..0x0300192c`; GPIOA15/SPK_EN uses `0x03001908`. Buttons and the
encoder switch are active-low. The final hardware can drop these software
pull-ups once external pulls are present and verified.

| Logical input | Board pin | Pad | GPIO bit |
| --- | --- | --- | --- |
| key0 | P19 | RTC pad | GPIOE19 |
| key1 | A22 | EMMC_CLK / SPINOR_SCK alt | GPIOA22 |
| key2 | A25 | EMMC_DAT0 / SPINOR_MOSI alt | GPIOA25 |
| key3 | A27 | EMMC_DAT3 / SPINOR_WP_X alt | GPIOA27 |
| key4 | A23 | EMMC_CMD / SPINOR_MISO alt | GPIOA23 |
| key5 | A24 | EMMC_DAT1 / SPINOR_CS_X alt | GPIOA24 |
| key6 | A15 | SPK_EN | GPIOA15 |
| encoder A | P22 | RTC pad | GPIOE22 |
| encoder B | P23 | RTC pad | GPIOE23 |
| encoder E | P21 | RTC pad | GPIOE21 |

## Wire format

Every HID report — host-bound and device-bound — carries a 6-byte header:

```text
byte0 = report_id  (0x10 host-bound, 0x20 device-bound, 0x30 feature)
byte1 = command    (CMD_*)
byte2 = sid_lo     ┐ session_id, uint16 little-endian
byte3 = sid_hi     ┘ (0 = broadcast / unset)
byte4 = plen_lo    ┐ payload_length, uint16 LE; max 58
byte5 = plen_hi    ┘
byte6..= payload
byte..63 = zero pad
```

### Host → board (output report 0x20)

| cmd | Name | Payload |
| --- | --- | --- |
| 0x01 | `CMD_REQUEST_SESSION` | UTF-8 plugin/wrapper hint (optional) |
| 0x04 | `CMD_SESSION_HEARTBEAT` | empty; host must send one every 10s per live sid (`sid` field carries the target) |
| 0x30 | `CMD_VT100_STREAM` | raw VT100 bytes — only forwarded to the screen FIFO when `sid == g_active_sid` (board-focused window) |
| 0x50 | `CMD_STATUS_UPDATE` | `[state]` (1 byte `SessionState`: `CONNECTED/DISCONNECTED/RUN/WAIT/DONE/ERROR`); board stores it for the grid row |
| 0x51 | `CMD_TOKEN_USAGE` | `[input:u64 LE][output:u64 LE][cost_cents:u64 LE]` (24 B); forwarded as `session N token in=X out=Y cost=Z` to `--ctrl-out` |
| 0x52 | `CMD_TURN_APPEND` | `[role:u8][text:utf-8]` (role 0=user 1=assistant 2=tool 3=system); forwarded as `session N turn role=... text:...` |
| 0x53 | `CMD_PERMISSION_REQ` | `[req_id:u64 LE][tool_len:u8][tool][args_summary]`; forwarded as `session N permission reqid=... tool=... args:...` |
| 0x54 | `CMD_AGENT_META` | `[kind:u8][cwd_len:u8][cwd][branch]` (kind 0=claude 1=codex 2=vscode 3=cursor 4=browser); forwarded as `session N meta kind=... cwd=... branch=...` |
| 0x40 | `CMD_UI_SCALE_CHANGE` | forwards `cell W H` to `aikb_lcd_ui --ctrl` |
| 0x20 | `CMD_WINDOW_SWITCH` | **deprecated**: board owns its own UI now; firmware ignores it |
| 0x21 | `CMD_WINDOW_ACTIVATE` | **deprecated**: board owns its own UI now; firmware ignores it |

### Board → host (input reports 0x10)

| cmd | Name | Payload |
| --- | --- | --- |
| 0x02 | `CMD_SESSION_RESPONSE` | `[Status]` (1 byte: `SESSION_CREATED` etc.) |
| 0x03 | `CMD_SESSION_INVALID` | `[Status]` (`INVALID` / `RECLAIMED` / `EXPIRED` / `POOL_FULL` / `DISCONNECTED`) |
| 0x05 | `CMD_SESSION_FOCUS` | empty; `sid` is the session the on-board picker confirmed. Host opens the VT100 stream gate so only this sid's bytes are sent back. |
| 0x10 | `CMD_KEY_EVENT` | `[key_bits, encoder_pressed]` — bits 0..6 = newly pressed key0..key6 edges, bit7 reserved, byte1 bit0 = encoder switch press edge |
| 0x11 | `CMD_ENCODER_EVENT` | `[delta:int8]` — left −1, right +1 |
| 0x12 | `CMD_PERMISSION_RES` | `[req_id:u64 LE][decision:u8]` where decision is `0=allow`, `1=deny`, `2=always`; emitted when the picker approves/rejects a pending permission. |

`CMD_KEY_EVENT` and `CMD_ENCODER_EVENT` always carry the **currently-displayed
sid** (the board's `g_active_sid` while in the terminal view). Picker-view
input is consumed board-locally and is **not** forwarded over HID; only
`CMD_SESSION_FOCUS` crosses the wire when the user confirms a selection.

## Session table

The firmware owns a 256-slot session table indexed by sid. Allocation and
liveness rules:

- `CMD_REQUEST_SESSION` → linear-probe a free slot starting from `g_next_sid`,
  initialise `last_heartbeat_ms = now`, emit `session N state connected\n` on
  the lcd_ui ctrl-out FIFO.
- Pool full → evict the LRU slot, send `CMD_SESSION_INVALID(RECLAIMED)` for
  the old sid first, then `CMD_SESSION_RESPONSE(CREATED)` for the new owner.
- `CMD_SESSION_HEARTBEAT` → touch `last_heartbeat_ms` and clear the
  `disconnected` flag (re-emits `session N state connected\n` if recovering).
- `CMD_VT100_STREAM` for an unknown sid → reply `CMD_SESSION_INVALID(INVALID)`,
  drop the payload. `sid == 0` is no longer accepted (the legacy dashboard JSON
  side channel has been retired; the board renders the grid from its own table).
- `CMD_STATUS_UPDATE` payload[0] is parsed as a `SessionState` byte; stored on
  the entry and mirrored to lcd_ui as `session N state X\n`.

Heartbeat reaper, run every ~1s in the main loop:

- `now − last_heartbeat_ms ≥ 30 s` and not yet flagged → mark
  `disconnected = true`, set `state_byte = DISCONNECTED`, emit
  `session N state disconnected\n` to lcd_ui, send
  `CMD_SESSION_INVALID(sid, EXPIRED)` to host so it stops streaming.
- `now − last_heartbeat_ms ≥ 60 s` (already disconnected) → free the slot,
  emit `session N removed\n`, drop `g_active_sid` / `g_selected_sid` if they
  pointed at it.

## ui-ctrl FIFO (lcd_ui → hid_input)

`--ui-ctrl-in PATH` opens a FIFO that `aikb_lcd_ui` writes line-oriented
commands to. The grammar is intentionally tiny:

```text
view picker          # board entered the session picker
view terminal        # board returned to the terminal view
select N             # picker highlight moved to sid N
focus N              # picker confirmed sid N — board emits CMD_SESSION_FOCUS
                     # and switches g_view back to BOARD_VIEW_TERMINAL
permission N reqid=R decision=allow|deny|always
                     # picker answered pending permission R for sid N
```

The reverse direction (`--ctrl-out`, hid_input → lcd_ui) carries the existing
`cell W H` line plus the new session-table notifications:

```text
session N state X    # X ∈ {connected, disconnected, run, wait, done, error}
session N hint TEXT  # host label from CMD_REQUEST_SESSION, e.g. terminal profile
session N removed    # entry was freed by the 60 s GC
```

## Screen FIFO

`CMD_VT100_STREAM` payload bytes are written **verbatim** to the path given by
`--screen-out`. The host generates VT100 escape sequences (`\x1b[2J\x1b[H`,
cursor moves, etc.) directly into the payload — there is no in-firmware
translation layer any more.

The default boot script creates `/tmp/aikb_lcd_ui.in` as a FIFO, starts
`aikb_lcd_ui` on it, and runs `aikb_hid_input --screen-out /tmp/aikb_lcd_ui.in`.

## Local pet event FIFO

`--event-out PATH` is optional. It mirrors local button and encoder activity to
a board-local FIFO for `aikb_lcd_ui --view pet --event-input PATH` while keeping
the existing HID reports to the Host unchanged.

The event lines are:

```text
KEY 0 DOWN
KEY 1 DOWN
KEY 2 DOWN
ENC +1
ENC -1
ENC_BTN DOWN
```

## Board test

```sh
/mnt/system/usr/bin/aikb_hid_input --hid /dev/hidg0 \
    --screen-out /tmp/aikb_lcd_ui.in --event-out /tmp/aikb_pet_events.in --debug
cat /tmp/aikb_hid_input.log
```

The encoder emits one `CMD_ENCODER_EVENT` after two valid quadrature edges,
which matches the current mechanical encoder used on the test board. If the
encoder direction is reversed on the final hardware, start with `--reverse`.

## Host-side validation

After flashing, run the probe from the host side:

```sh
cd /home/rv_nano/Sipeed/rv_nano/tools/vibe-bridge
./scripts/probe_hidraw.sh /dev/hidraw0
```

Expect `RESULT=PASS` from `hid handshake` (the previous `RESULT=LEGACY_NOISE`
on old firmware confirmed the link works).

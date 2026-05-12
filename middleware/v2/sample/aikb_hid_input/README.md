# AIKB HID Input

`aikb_hid_input` scans the AIKB buttons and rotary encoder and bridges them to
the host over the vendor HID interface, using the **vibe-bridge protocol v0**
(see `tools/vibe-bridge/docs/hid_protocol.md`).

## Pin Map

All inputs are configured as GPIOA inputs with internal pull-up enabled and
pull-down disabled. Buttons and the encoder switch are active-low.

| Logical input | Board pin | Pad | GPIO bit |
| --- | --- | --- | --- |
| key0 | A15 | SPK_EN | GPIOA15 |
| key1 | A24 | SPINOR_CS_X | GPIOA24 |
| key2 | A23 | SPINOR_MISO | GPIOA23 |
| encoder A | A27 | SPINOR_WP_X | GPIOA27 |
| encoder B | A25 | SPINOR_MOSI | GPIOA25 |
| encoder E | A22 | SPINOR_SCK | GPIOA22 |

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
| 0x30 | `CMD_VT100_STREAM` | raw VT100 bytes for the active LCD |
| 0x50 | `CMD_STATUS_UPDATE` | informational; firmware just touches sid |
| 0x20 / 0x21 / 0x40 | `WINDOW_SWITCH` / `WINDOW_ACTIVATE` / `UI_SCALE_CHANGE` | currently no-op in firmware (handled by host daemon) |

### Board → host (input reports 0x10)

| cmd | Name | Payload |
| --- | --- | --- |
| 0x02 | `CMD_SESSION_RESPONSE` | `[Status]` (1 byte: `SESSION_CREATED` etc.) |
| 0x03 | `CMD_SESSION_INVALID` | `[Status]` (`INVALID` / `RECLAIMED` / `POOL_FULL`) |
| 0x10 | `CMD_KEY_EVENT` | `[key_bits]` — bit0..bit2 = key0..key2, bit7 = encoder switch |
| 0x11 | `CMD_ENCODER_EVENT` | `[delta:int8]` — left −1, right +1 |

Key and encoder events use `sid = 0` (broadcast); the host daemon decides
which session to route them to.

## Session table

The firmware owns a 256-slot session table indexed by sid. Allocation rules:

- `CMD_REQUEST_SESSION` → linear-probe a free slot starting from `g_next_sid`.
- Pool full → evict the LRU slot, send `CMD_SESSION_INVALID(RECLAIMED)` for
  the old sid first, then `CMD_SESSION_RESPONSE(CREATED)` for the new owner.
- `CMD_VT100_STREAM` for an unknown sid → reply `CMD_SESSION_INVALID(INVALID)`,
  drop the payload.
- Any traffic for a known sid touches its `last_active_ms`.

Long-term TTL expiry is handled by the host daemon; the firmware only enforces
the LRU pressure when the pool is full.

## Screen FIFO

`CMD_VT100_STREAM` payload bytes are written **verbatim** to the path given by
`--screen-out`. The host generates VT100 escape sequences (`\x1b[2J\x1b[H`,
cursor moves, etc.) directly into the payload — there is no in-firmware
translation layer any more.

The default boot script creates `/tmp/aikb_lcd_ui.in` as a FIFO, starts
`aikb_lcd_ui` on it, and runs `aikb_hid_input --screen-out /tmp/aikb_lcd_ui.in`.

## Board test

```sh
/mnt/system/usr/bin/aikb_hid_input --hid /dev/hidg0 \
    --screen-out /tmp/aikb_lcd_ui.in --debug
cat /tmp/aikb_hid_input.log
```

If the encoder direction is reversed on the final hardware, start with
`--reverse`.

## Host-side validation

After flashing, run the probe from the host side:

```sh
cd /home/rv_nano/Sipeed/rv_nano/tools/vibe-bridge
./scripts/probe_hidraw.sh /dev/hidraw0
```

Expect `RESULT=PASS` from `hid handshake` (the previous `RESULT=LEGACY_NOISE`
on old firmware confirmed the link works).

# AIKB HID Input

`aikb_hid_input` scans the AIKB buttons and rotary encoder and reports them
through the vendor-defined USB HID interface described by
`/home/rv_nano/Sipeed/rv_nano/docs/hid_vendor_terminal_spec.pdf`.

## Pin Map

All inputs are configured as GPIOA inputs with internal pull-up enabled and
pull-down disabled. Buttons and the encoder switch are active-low.

| Logical input | Board pin | Pad | GPIO bit | HID bit |
| --- | --- | --- | --- | --- |
| key0 | A15 | SPK_EN | GPIOA15 | report 0x10 byte1 bit0 |
| key1 | A24 | SPINOR_CS_X | GPIOA24 | report 0x10 byte1 bit1 |
| key2 | A23 | SPINOR_MISO | GPIOA23 | report 0x10 byte1 bit2 |
| encoder A | A27 | SPINOR_WP_X | GPIOA27 | quadrature A |
| encoder B | A25 | SPINOR_MOSI | GPIOA25 | quadrature B |
| encoder E | A22 | SPINOR_SCK | GPIOA22 | report 0x10 byte1 bit7 |

The program writes report ID `0x10` as a 64-byte HID gadget report:

```text
byte0 = 0x10
byte1 = key0..key2 in bit0..bit2, encoder switch in bit7
byte2 = signed encoder delta, left = -1, right = +1
byte3..63 = 0
```

Output report ID `0x20` is parsed and acknowledged with input report ID `0x21`;
the terminal text path is intentionally kept separate for the LCD UI to consume
later.

## Board Test

```sh
/mnt/system/usr/bin/aikb_hid_input --hid /dev/hidg0 --debug
cat /tmp/aikb_hid_input.log
```

If the encoder direction is reversed on the final hardware, start it with
`--reverse`.

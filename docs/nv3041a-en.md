# The NV3041A of the Guition JC4827W543: stray pixels outside a window

On the Guition JC4827W543, a 4.3-inch board with an ESP32-S3 and a 480x272
NV3041A panel on QSPI, the panel now and then writes a pair of pixels just
outside the window it was given. When the screen is redrawn in small
windows, as an animation or an overlay that changes in places does, these
pixels stay behind as dots and short lines on the picture. Our tests show
that DGX sends the right commands and the right number of pixels. The fault
is in the panel or the board, and the board maker's own driver shows it too.
Sending whole frames from a virtual screen hides it.

## The hardware and software

Table 1. What the measurements were made on.

| Part | Version |
| --- | --- |
| Board | Guition JC4827W543 |
| Chip | ESP32-S3 revision v0.2, 8 MB PSRAM (3.3 V), 4 MB flash |
| Panel controller | NV3041A, datasheet v1.2 of 2022-10-11 |
| Panel wiring | QSPI: CS GPIO45, SCLK GPIO47, D0-D3 GPIO21, 48, 40, 39; TE GPIO0 |
| ESP-IDF | 6.1 |
| Bus clock | 26.7 MHz (32 MHz asked for), also 16 MHz |
| DGX | after 0.8.3: one chip select over all the pixels of a window |

## What it looks like

A window written with too many pixels does not wrap around on this panel:
the surplus goes outside the window. We saw this when we deliberately sent
32 extra clocks after the last pixel of every window: they appeared as dots
next to the windows. The fault itself looks the same. Now and then a pair
of pixels, one 32-bit word, lands just outside a window, above it or to the
left of it in the upright picture. Inside an area that is redrawn on every
frame, a wrong pixel is gone by the next frame, so the fault is only
visible where nothing is drawn: around the window. To see it, we drew a
one-pixel frame just outside each window and watched it lose pixels. Some
of the pixels that land there are of a wrong color too, green or purple,
which points to the panel catching a stray clock edge.

It is rare. A window written about 70 times a second in pieces of about
1000 pixels lost a pair within 10 to 20 minutes. The Game of Life morph,
which rewrites a 255x255 window about 60 times a second, lost several pairs
within a few minutes.

## Why DGX is not the cause

The host test `test_life_panel` runs the Game of Life morph exactly as the
board does: the same patterns, a 480x272 screen and a bus buffer of 10424
bytes. Every frame goes to a model of the panel that fills its memory the
way the commands tell it to. After every frame, the model's memory matches
a virtual screen drawn the same way, pixel for pixel, and no pixel comes
outside a window begun. `test_frame_to_panel` checks the same for frames
sent from a virtual screen under an overlay, and also that every window
goes in one chip select that ends exactly at its last pixel.

On the wire, DGX sends what the board maker's driver sends (Arduino_GFX,
`Arduino_ESP32QSPI`): CASET and RASET as commands, then one chip select for
the whole window, opcode 0x32 with address 0x002C00 on one line, then the
pixels on four lines in pieces with no opcode or address of their own.

## What was tried

Table 2. Changes tried against the stray pixels, and what each did.

| Change | Effect |
| --- | --- |
| Bus clock 16 MHz instead of 26.7 MHz | none |
| A window and 0x2C with every piece, the chip select raised between pieces | none |
| 0x2C sent alone first, then the pixels with 0x3C, as Arduino_GFX does | none |
| CASET and RASET sent again with every frame | none |
| The bounds of the window even or odd | none |
| Pieces a multiple of 4 bytes, or 2 bytes more | no clear difference |
| Pauses of up to 1 ms between pieces | none |
| More pieces to a frame (a 4000-byte buffer instead of 10424) | strays remain, some of them off in color |
| SPI mode 1, as LovyanGFX uses | strays remain |
| The weakest drive of the QSPI pins | strays remain |
| MADCTL without the MX and MY mirroring (the picture upside down) | fewer strays, not none |
| The address of opcode 0x32 on four lines, as the table in section 6.3.4 of the datasheet reads | the panel ignores the pixels |
| Each window written at a fixed time after the TE pulse | no strays within a few minutes; does not rule them out |

The datasheet contradicts itself on the address of opcode 0x32. The table
in section 6.3.4 and figure 6.3.4.2 put it on four lines in 6 clocks, while
figure 6.4.12.1 puts it on one line in 24 clocks. The panel takes only the
second.

## What others saw

A user of the board with the maker's Arduino_GFX had bright remnants of a
logo left on the screen after it changed. They went away when every frame
was put together in memory and sent whole instead of in small updates
([thelastoutpostworkshop/support#1](https://github.com/thelastoutpostworkshop/support/issues/1)).
The maker's own demos draw into a full-screen canvas and send all of it.

## What to do

Draw into a virtual screen and send the whole of it, or at least whole
frames, with `dgx_vscreen_to_screen()`. A stray pixel then lives for one
frame. A full frame of 480x272 at 16 bits is 261,120 bytes: 19.6 ms on the
wire at 26.7 MHz, 13.1 ms at 40 MHz. Updates in small windows work, but now
and then they leave a pair of pixels behind next to a window. On a screen
that is then left as it is, those pixels stay.

## The TE line

The panel marks every frame it shows on its TE line, which reaches GPIO0
of the board through R9, a 0-ohm resistor. TEON (0x35) alone does not turn
it on: the TE output is off until `te_out_oe` of TE_CTRL (0xE7, bit 4) is
set on the maker's page of registers (0xFF = 0xA5, then 0xE7 = 0x10, then
0xFF = 0x00). TEON does nothing while TE is already on, so send TEOFF
(0x34) before TEON to change its mode. With the maker's settings, the panel
shows a frame every 15.6 ms (64 Hz). DGX does not use TE.

## Turning the picture

According to [atomic14](https://www.atomic14.com/esp32/boards/guition-jc4827w543/),
only 0° and 180° work on this panel. We have not tried
`dgx_nv3041a_orientation()` with the axes swapped.

# The Guition JC4827W543 and its NV3041A panel

The Guition JC4827W543 is a 4.3-inch board with an ESP32-S3 and a 480x272
IPS panel, driven by an NV3041A controller over QSPI. DGX drives it with
`dgx_qspi_init()` and `dgx_nv3041a_init()`. This panel took more work than
any other display DGX supports. Its datasheet is wrong in places, it drops
settings sent at the wrong moment, and now and then it writes a pair of
pixels outside the window it was given, whatever the driver does. This note
collects what was found, measured and tried.

In short:

- Send whole frames. Draw into a virtual screen the size of the screen and
  send all of it every frame. Updates in small windows now and then leave
  stray pixels behind; see [Stray pixels outside a window](#stray-pixels-outside-a-window).
- Use 40 MHz for the most frames a second: 13.9 ms to send a frame. It is
  the limit the datasheet sets, but the panel works there on the board it
  was tried on. At 26.7 MHz, within the datasheet, a frame takes 20.4 ms but
  goes from PSRAM by DMA with nothing copied, and leaves 92 % of the sending
  core free.
  80 MHz garbles the picture. See [A whole frame from PSRAM](#a-whole-frame-from-psram).
- Use DGX 0.9.1 or later. 0.9.0 held the chip select low over a whole
  window, which is unsafe with other devices on the same SPI host, and
  under ESP-IDF 6.0 and 6.1 versions before 0.9.1 connect SPI outputs to
  GPIO0, the TE line of the panel.
- Only the upright landscape picture has been tried.

## The hardware and software

Table 1. The board and the software everything below was measured with.

| Part | Version |
| --- | --- |
| Board | Guition JC4827W543 |
| Chip | ESP32-S3 revision v0.2, 8 MB octal PSRAM (3.3 V), 4 MB flash |
| PSRAM settings | `CONFIG_SPIRAM_MODE_OCT`, `CONFIG_SPIRAM_SPEED_80M` |
| Panel controller | NV3041A, datasheet v1.2 of 2022-10-11 |
| Panel wiring | QSPI: CS GPIO45, SCLK GPIO47, D0-D3 GPIO21, 48, 40, 39; TE GPIO0; no reset line |
| Backlight | GPIO1, on when high |
| ESP-IDF | 6.1; DGX builds for this board under 5.5.4 too |
| SPI driver | `esp_driver_spi` of ESP-IDF, SPI2_HOST in quad mode |
| Bus clock | 26.7 MHz (32 MHz asked for); also 16 and 40 MHz |
| DGX | 0.8.2 to 0.9.1; what changed between them is in [How DGX got here](#how-dgx-got-here) |

## Setting it up

```c
dgx_bus_protocols_t *bus = dgx_qspi_init(SPI2_HOST, SPI_DMA_CH_AUTO, GPIO_NUM_21, GPIO_NUM_48, GPIO_NUM_40,
                                         GPIO_NUM_39, GPIO_NUM_47 /* sclk */, GPIO_NUM_45 /* cs */, 40 * 1000 * 1000, 0);
dgx_screen_t *lcd = dgx_nv3041a_init(bus, GPIO_NUM_NC, GPIO_NUM_1, 16, DgxScreenRGB);
/* the whole picture: 480 x 272 x 2 = 261,120 bytes, in PSRAM */
dgx_screen_t *frame = dgx_vscreen_init(lcd->width, lcd->height, 16, DgxScreenRGB);
for (;;) {
    /* ... draw the next picture into frame ... */
    dgx_vscreen_to_screen(lcd, 0, 0, frame, 0);
}
```

The virtual screen needs PSRAM: `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`
and `CONFIG_SPIRAM_SPEED_80M=y`. With `CONFIG_SPIRAM_USE_MALLOC`, the default,
a block this large goes to PSRAM by itself. How the frame then gets to the
panel depends on the clock; see [A whole frame from PSRAM](#a-whole-frame-from-psram).

## How the panel takes commands and pixels

The panel has no D/C line. Every transfer begins with an 8-bit opcode and a
24-bit address, both on one line, and the command of the panel is the
middle byte of the address.

Table 2. The three kinds of transfer DGX sends, and what goes in each part.

| Transfer | Opcode | Address | Data |
| --- | --- | --- | --- |
| A command and its parameters | 0x02 | `00 cmd 00`, such as 0x002A00 for CASET | the parameters, on one line |
| The first piece of pixels of a window | 0x32 | 0x002C00, memory write | pixels, on four lines |
| Every later piece of the window | 0x32 | 0x003C00, memory write continue | pixels, on four lines |

- 0x2C, 0x3C and the software reset 0x01 are missing from the command list
  of the datasheet. The panel takes all three.
- The datasheet contradicts itself on the address of opcode 0x32. The table
  in section 6.3.4 and figure 6.3.4.2 put it on four lines in 6 clocks,
  while figure 6.4.12.1 puts it on one line in 24 clocks. The panel takes
  only the second; with the first it ignores the pixels.
- Pixels sent past the end of a window do not wrap around to its start: the
  panel puts them outside the window.
- Every transfer is a chip select of its own, made by the SPI hardware, so
  other devices on the host can be served between two pieces of a picture.
  The datasheet asks for the chip select to go low 10 ns before the first
  clock edge and stay low 30 ns after the last (TCSS and TCSH, table
  8.3.2.1), and to stay high 30 ns between transfers (TCHW). With
  `cs_ena_pretrans` and `cs_ena_posttrans` at 0, the ESP32-S3 raises the
  chip select about half a clock after the last edge, 12 ns at 40 MHz. DGX
  sets them to one clock and two clocks: 25 and 50 ns at 40 MHz.
- DGX reads nothing from the panel.

The maker's driver, `Arduino_ESP32QSPI` of the Arduino_GFX 1.4.4 in the
Guition package, writes pixels another way. It sends 0x2C alone as a
command, then all the pixels of a window under one chip select that it
drives as a GPIO, the later pieces with no opcode or address of their own.
Both ways put the right pixels on the panel, and the stray pixels come
either way.

## Starting the panel

`dgx_nv3041a_init()` sends this, in this order:

1. SWRESET (0x01), then 120 ms. The board has no reset line for the panel.
2. The maker's registers between 0xFF = 0xA5 and 0xFF = 0x00, byte for byte
   as `Arduino_NV3041A` of the maker's Arduino_GFX sends them: power, gate
   and source timings, gamma. They include COLMOD 0x01, which is RGB565 on
   this controller; 0x00 would be RGB666.
3. SLPOUT (0x11), then 120 ms. During this time the panel loads its factory
   defaults into the registers (7.2.9), so whatever the panel is to keep
   has to come after it. Before 0.9.0, MADCTL and the inversion were sent
   only before SLPOUT.
4. MADCTL again. Upright is MX and MY both set (0xC0): the glass is mounted
   turned round against the memory of the controller. DgxScreenBGR adds the
   BGR bit (0x08).
5. INVON (0x21), then 20 ms. The glass is IPS, and without inversion its
   colors come out negative. Display commands take effect from the next
   frame (7.2.10).
6. The memory is filled black, then DISPON (0x29), then 100 ms, as the
   maker's demo waits, and only then the backlight. The panel shows noise
   while it starts, so the backlight stays off until here.

## The bus clock

The SPI clock of the ESP32-S3 is 80 MHz divided by a whole number, and a
clock that does not divide evenly is rounded down: 32 MHz runs at 26.7 MHz.
For four lines and RGB565, the datasheet asks for a clock period above
25 ns (TSCYCW, table 8.3.2.1), that is, a clock below 40 MHz.

Table 3. The clocks tried on the JC4827W543 with ESP-IDF 6.1 and DGX 0.8.2 to
0.9.1, and how the picture looked.

| Asked for | Runs at | Within the datasheet | Picture |
| --- | --- | --- | --- |
| 16 MHz | 16 MHz | yes | right |
| 32 MHz | 26.7 MHz | yes | right |
| 40 MHz | 40 MHz | at its limit | right |
| 80 MHz | 80 MHz | no | garbled |

A fill of the whole screen took 13.45 ms at 40 MHz with DGX 0.8.3, where the
wire alone needs 13.06 ms. What a whole frame from a virtual screen takes at
each clock is in the next section. A lower clock does nothing against the
stray pixels: they come as often at 16 MHz as at 26.7 MHz. To run within the
datasheet, ask for 26.7 MHz itself: 26666667 Hz, 80 MHz divided by 3.

## A whole frame from PSRAM

The numbers in this section are for the case of this note: a whole frame of
480x272 at 16 bits, drawn in a virtual screen in PSRAM and sent with
`dgx_vscreen_to_screen()`. A frame drawn in internal memory, or sent in
parts, takes other times.

`dgx_vscreen_to_screen()` sends such a frame one of two ways:

- **Copy.** The rows are copied, half a bus buffer at a time, into the bus
  buffer in internal memory, and sent from there while the CPU fills the
  other half. This is how every frame went before 0.9.1.
- **In place.** The rows go by DMA straight from PSRAM, in pieces of the bus
  buffer, with `SPI_TRANS_DMA_USE_PSRAM`, and nothing is copied. The DMA
  then shares PSRAM with the CPU. If PSRAM falls behind, the SPI driver
  reports an underflow and the pieces it hit come out wrong.

DMA from PSRAM falls behind at 40 MHz on four lines, 20 MB/s, on the very
first frame and with nothing else running. At 26.7 MHz and below it keeps
up, also while the other core writes PSRAM all the time. So the QSPI bus of
DGX 0.9.1 sends a frame from PSRAM in place at 26.7 MHz and below, and
through the copy above that. If an underflow comes anyway, with a slower
PSRAM, the bus logs it once and copies from then on.

The bench sent 60 frames back to back with nothing drawn between them and
timed each one. The free CPU was counted by a task at the lowest priority on
the sending core, compared with what it counts with the core to itself.
"PSRAM busy" means another task on the other core wrote a buffer of 256 KB
in PSRAM over and over the whole time. The slowest frame of a run was less
than 0.25 ms slower than the average of the run.

Table 4. The time of a whole frame, in ms on average, on the JC4827W543 with
ESP-IDF 6.1 and DGX 0.9.1, the PSRAM octal at 80 MHz, and the time the wire
alone needs for 261,120 bytes on four lines.

| Clock | Wire alone | Copy | In place | Copy, PSRAM busy | In place, PSRAM busy |
| --- | --- | --- | --- | --- | --- |
| 40 MHz | 13.06 | 13.89 | not used, PSRAM falls behind | 14.45 | not used |
| 26.7 MHz | 19.58 | 20.81 | 20.35 | 20.99 | 20.38 |
| 20 MHz | 26.11 | 27.61 | 27.08 | 27.75 | 27.11 |
| 16 MHz | 32.64 | 34.64 | 33.80 | 34.75 | 33.82 |

Table 5. The share of the sending core left free while the frames of
table 4 were sent, in percent, same board and software.

| Clock | Copy | In place | Copy, PSRAM busy | In place, PSRAM busy |
| --- | --- | --- | --- | --- |
| 40 MHz | 58.8 | not used | 5.8 | not used |
| 26.7 MHz | 68.4 | 92.4 | 28.9 | 89.1 |
| 20 MHz | 74.3 | 92.8 | 46.9 | 89.6 |
| 16 MHz | 76.8 | 93.1 | 48.6 | 90.6 |

What the tables show:

- 40 MHz gives the most frames a second, 13.9 ms to send one. The copy keeps
  the sending core busy for 41 % of that time, and for 94 % when the other
  core works on PSRAM too: the copy reads PSRAM, and that is slower then.
- At 26.7 MHz a frame takes 20.4 ms, but in place it leaves about 90 % of
  the sending core free, whatever the other core does.
- In place is 0.5 to 0.8 ms a frame faster than the copy at the same clock.
  What is left over the wire, about 30 µs a piece, is mostly the start of
  every piece.
- These are the times of sending alone. A program that draws a frame and
  then sends it, in one task, spends the drawing on top. With two virtual
  screens, one drawn while the other is sent, the free CPU of table 5 is
  what the drawing gets on the sending core, and the panel shows a frame
  every 15.6 ms (see [The TE line](#the-te-line)).

## Stray pixels outside a window

### What it looks like

A window written with too many pixels does not wrap around on this panel:
the surplus goes outside the window. This shows when 32 extra clocks are
deliberately sent after the last pixel of every window: they appear as dots
next to the windows. The fault looks much like them: now and then a pair
of pixels, one 32-bit word, lands just outside a window, above it or to the
left of it in the upright picture. Inside an area that is redrawn on every
frame, a wrong pixel is gone by the next frame, so the fault is only
visible where nothing is drawn: around the window. To see it, a one-pixel
frame was drawn just outside each window and watched for lost pixels. Some
of the pixels that land there are of a wrong color too, green or purple,
which points to the panel catching a stray clock edge.

It is rare. A window written about 70 times a second in pieces of about
1000 pixels lost a pair within 10 to 20 minutes. The Game of Life morph,
which rewrites a 255x255 window about 60 times a second, lost several pairs
within a few minutes.

### Why DGX is not the cause

The host test `test_life_panel` runs the Game of Life morph exactly as the
board does: the same patterns, a 480x272 screen and a bus buffer of 10424
bytes. Every frame goes to a model of the panel that fills its memory the
way the commands tell it to. After every frame, the model's memory matches
a virtual screen drawn the same way, pixel for pixel, and no pixel comes
outside a window begun. `test_frame_to_panel` checks the same for frames
sent from a virtual screen under an overlay, and also that the command that
writes memory always goes with the first pixels of a window, never alone.
The maker's own driver shows the fault as well; see
[What others saw](#what-others-saw).

### What was tried

Table 6. Changes tried against the stray pixels on the JC4827W543 at 26.7 MHz
unless the row says otherwise, and what each did.

| Change | Effect |
| --- | --- |
| Bus clock 16 MHz instead of 26.7 MHz | none |
| A window and 0x2C with every piece, the chip select raised between pieces | none |
| 0x2C sent alone first, then all the pixels with 0x3C under one chip select, as Arduino_GFX does | none |
| The chip select of the SPI hardware for every piece, held one clock before and two after, 0x3C with every piece after the first (DGX 0.9.1) | strays remain, about as often |
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

### What others saw

A user of the board with the maker's Arduino_GFX had bright remnants of a
logo left on the screen after it changed. They went away when every frame
was put together in memory and sent whole instead of in small updates
([thelastoutpostworkshop/support#1](https://github.com/thelastoutpostworkshop/support/issues/1)).
The maker's own demos draw into a full-screen canvas and send all of it.

### What to do

Send whole frames, as in [Setting it up](#setting-it-up). A window of the
whole screen has no outside for a stray pair to land in, and a wrong pixel
inside it lives until the next frame. A picture that is then to stay as it
is for a long time can be sent once more a moment later. Updates in small
windows work, but now and then they leave a pair of pixels behind next to a
window, and on a screen that is then left as it is, those pixels stay.

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
only 0° and 180° work on this panel. DGX has tried only the upright
picture: `dgx_nv3041a_orientation()` has not been tried in any direction,
with the axes swapped or not.

## ESP-IDF 6.0 and 6.1: GPIO0

In ESP-IDF 6.0 and 6.1 the SPI driver handles a quad bus as an octal one:
`spi_common.c` tests `flags & SPICOMMON_BUSFLAG_OCTAL`, which is true for a
quad bus too, where ESP-IDF 5.5 tests that all its bits are set. With the
pins data4 to data7 of `spi_bus_config_t` left at 0, the driver connects its
SPI data4 to data7 outputs to GPIO0, the TE line of this panel, and logs
"GPIO 0 is conflict with others and be overwritten" three times. DGX 0.9.1
sets those four pins to -1 on the hosts it sets up. A program that sets up
the host itself for `dgx_qspi_init_on_bus()` has to set them to -1 as well.
Found in the sources of ESP-IDF 6.0.1, 6.0.3 and 6.1; 5.5.4 and 5.5.5 are
not affected.

## Other parts of the board

Table 7. The other parts of the board and their pins, from the maker's
schematic (sheet ESP32-4827A043 V0.2). None of them has been tried with DGX.

| Part | Pins |
| --- | --- |
| Capacitive touch, GT911 on I2C | SDA GPIO8, SCL GPIO4, RST GPIO38, INT GPIO3 |
| Resistive touch, XPT2046 on SPI, on the variants that have it | CS GPIO38, CLK GPIO12, DIN GPIO11, DOUT GPIO13, IRQ GPIO3 |
| TF card on SPI | CS GPIO10, MOSI GPIO13, MISO GPIO11, CLK GPIO12 |
| I2S amplifier NS4168 | LRCLK GPIO2, DIN GPIO41, BCLK GPIO42 |
| Octal PSRAM | GPIO35 to GPIO37, not free |

The ESP_Panel configuration in the maker's package gives the GT911 SDA 7 and
SCL 8, against both the schematic and the maker's LVGL demo. The TF card
has pins of its own, so it can go on SPI3_HOST and leave SPI2_HOST to the
panel.

## How DGX got here

- 0.8.2 brought the QSPI bus and the driver. It used the chip select of the
  SPI hardware with no hold, sent 0x2C with the first piece of a window and
  0x3C with every later one, and sent INVOFF and MADCTL only before SLPOUT.
- 0.8.3 made the bus buffer follow the clock, 15,640 bytes at 40 MHz, and
  stopped waiting after every piece of a fill.
- After 0.8.3, stripes and stray pixels near the start of windows were put
  down to 0x3C. Changes that never went into a release sent 0x2C with every
  piece instead, which made every piece start again at the top of its
  window and write over the pieces before it, and then a window of its own
  for every piece.
- 0.9.0 sent MADCTL and INVON after SLPOUT and waited 100 ms after DISPON.
  It drove the chip select as a GPIO and held it low over a whole window,
  as the maker's driver does, on the belief that the panel rejects 0x3C
  after the chip select rises. The stray pixels stayed, and the
  measurements in this note traced them to the panel.
- 0.9.1 tested that belief apart from the short hold. With the hold the
  datasheet asks for, the panel takes 0x3C after the chip select rises: a
  window written in 89 pieces of odd lengths, each going on with 0x3C, came
  out exact. So DGX is back on the chip select of the SPI hardware. The
  held chip select of 0.9.0 had two faults, found by reading the code: a
  transfer of another device on a host DGX set up would go into the panel,
  and a window left unfinished kept the chip select low, and on a host of
  the program kept the host locked. 0.9.1 also stopped the SPI outputs
  going to GPIO0 under ESP-IDF 6, and sends a frame from PSRAM by DMA as it
  lies where PSRAM keeps up.

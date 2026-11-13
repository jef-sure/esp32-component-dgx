# Supported panels

The display controllers DGX drives, their constructors and options, and how
to make the screen of a module DGX has no driver for. Getting a display
going: the [README](../README.md#getting-started).

Each driver returns a `dgx_screen_t *` you can draw on. Color panels take a
pixel depth and a channel order; monochrome panels keep a RAM buffer that DGX
flushes for you; the e-paper panel keeps one that goes to the glass when the
program asks. Use `GPIO_NUM_NC` for any reset, backlight or CS line your
board does not wire.

| Controller | Type | Buses | Constructor | Kconfig option |
| --- | --- | --- | --- | --- |
| ST7735 | color TFT, 128x160 and the 0.96" 80x160 | SPI, P8 | `dgx_st7735_init(bus, rst, backlight, color_bits, cbo, resolution)` | `CONFIG_DGX_ENABLE_SPI_ST7735` |
| ST7789 | color TFT | SPI, P8 | `dgx_st7789_init(bus, rst, backlight, color_bits, cbo, resolution)` | `CONFIG_DGX_ENABLE_SPI_ST7789` |
| ILI9341 | color TFT | SPI, P8 | `dgx_ili9341_init(bus, rst, backlight, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SPI_ILI_9341` |
| NV3041A | color TFT, 480x272 | QSPI | `dgx_nv3041a_init(bus, rst, backlight, color_bits, cbo)` | `CONFIG_DGX_ENABLE_NV3041A` |
| ST77916 | round color TFT, 360x360 | QSPI, SPI | `dgx_st77916_init(bus, rst, backlight, color_bits, cbo)` | `CONFIG_DGX_ENABLE_ST77916` |
| GC9A01 | round color TFT | SPI, P8 | `dgx_gc9a01_init(bus, rst, backlight, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SPI_GC9A01` |
| SSD1351 | color OLED | SPI, P8 | `dgx_ssd1351_init(bus, rst, color_bits, cbo)` | `CONFIG_DGX_ENABLE_SSD1351` |
| SSD1322 | 16-gray OLED, 256x64, 240x128, 128x64 | P8, SPI | `dgx_ssd1322_init(bus, resolution, rst)` | `CONFIG_DGX_ENABLE_SSD1322` |
| SSD1306 | mono OLED | SPI, I2C | `dgx_ssd1306_init(bus, resolution, is_ext_vcc, rst)` | `CONFIG_DGX_ENABLE_SSD1306` |
| SSD1309 | mono OLED | SPI, I2C | `dgx_ssd1309_init(bus, resolution, rst)` | `CONFIG_DGX_ENABLE_SSD1309` |
| SH1106 | mono OLED | SPI, I2C | `dgx_sh1106_init(bus, resolution, rst)` | `CONFIG_DGX_ENABLE_SH1106` |
| ST7565R | mono LCD | SPI | `dgx_st7565r_init(bus, rst, backlight)` | `CONFIG_DGX_ENABLE_ST7565R` |
| ST7920 | mono LCD | SPI | `dgx_st7920_init(bus, rst, backlight, cs)` | `CONFIG_DGX_ENABLE_ST7920` |
| SSD1680 | e-paper of black, white and red, 128x296 and 122x250 | SPI | `dgx_ssd1680_init(bus, resolution, rst, busy)` | `CONFIG_DGX_ENABLE_SSD1680` |
| JD9165 | color TFT, 1024x600 | DSI | `dgx_jd9165_init(bus, rst, backlight, color_bits, cbo, frames)` | `CONFIG_DGX_ENABLE_JD9165` |
| JD9365 | color TFT, 800x1280 | DSI | `dgx_jd9365_init(bus, rst, backlight, color_bits, cbo, frames)` | `CONFIG_DGX_ENABLE_JD9365` |

Common parameters:

- `bus` — a `dgx_bus_protocols_t *` from `dgx_spi_init()`, `dgx_qspi_init()`,
  `dgx_i2c_init()`, `dgx_p8_init()` or `dgx_dsi_init()`.
- `rst` — reset GPIO, or `GPIO_NUM_NC` if the panel has no reset line.
- `backlight` (every display with a backlight: the color TFTs and the mono
  LCDs; the OLEDs and e-paper have none) — backlight GPIO, high for on, or
  `GPIO_NUM_NC` if it is not under software control. The driver turns it on
  once the panel is set up, and `dgx_<driver>_backlight(scr, on)` turns it
  off and on; without the line that call returns `ESP_ERR_NOT_SUPPORTED`.
  The driver does no more than that. For brightness by PWM the program
  drives the line with LEDC itself and passes `GPIO_NUM_NC` here: once LEDC
  has the pin, the backlight call changes nothing and still returns
  `ESP_OK`.
- `color_bits` — pixel depth; `16` (RGB565) is the usual choice. What each
  driver takes: [Color depths](#color-depths). A driver given a depth it
  cannot set up logs an error and returns NULL. For `18` see
  [docs/18-bit-pixels-en.md](18-bit-pixels-en.md)
  ([Russian](18-bit-pixels-ru.md)).
- `cbo` — channel order, `DgxScreenRGB` or `DgxScreenBGR`: the order colors
  come out in on the usual modules of the controller. The usual ILI9341
  modules show red as red with the BGR bit of the controller set, so there
  `DgxScreenRGB` sets that bit, the other way round from the other drivers.
  If red and blue are exchanged on your module, pass the other value.
- `resolution` (ST7789) — the panel: `ST7789_RESOLUTION_240x240`,
  `ST7789_RESOLUTION_240x320`, or `ST7789_RESOLUTION_320x240` for a 240x320
  panel in landscape; `ST7789_RESOLUTION_76x284` or
  `ST7789_RESOLUTION_284x76` for the 2.25" bar (an ST7789P3, colors in BGR
  order, not yet tried on a module); `ST7789_RESOLUTION_135x240` or
  `ST7789_RESOLUTION_240x135` for the 1.14" glass, which lies 52 columns and
  40 rows into the memory of the controller. Swapping the axes in
  `dgx_st7789_orientation()` turns the screen between the two shapes.
- `resolution` (ST7735) — the panel: `ST7735_RESOLUTION_128x160`, or
  `ST7735_RESOLUTION_160x128` for it in landscape;
  `ST7735_RESOLUTION_80x160` or `ST7735_RESOLUTION_160x80` for the 0.96" IPS
  glass. Its controller is set for 128x160 on the module, the glass lies 24
  columns into it, and its colors need no inversion and go in BGR order on
  the usual modules: Adafruit starts it as `INITR_MINI160x80`. A module
  that shows cyan for red and noise along two edges is of the other kind,
  see [A panel of your own module](#a-panel-of-your-own-module). Before 0.11
  the constructor had no such argument and made the 128x160.

Color panels, ST7920, SSD1306, SSD1309, SH1106, SSD1322 and SSD1680 also
expose an orientation setter (`dgx_<driver>_orientation(scr, dir_x, dir_y,
swap_xy)`). A color controller is told its scan direction. An ST7920 has no
command for that, and the OLED controllers mirror by their segments and rows
only as a module wires its glass: these drivers turn the picture where they
send it to the panel, and with `swap_xy` the screen of a 128x64 panel is
64x128. SSD1680 puts each pixel into the picture turned as it is drawn. A few
drivers add extras: those with a backlight have `dgx_<driver>_backlight()`,
GC9A01 has `dgx_gc9a01_display_off()` /
`dgx_gc9a01_display_on()`, SSD1351 has `dgx_ssd1351_brightness()`, SSD1306
has `dgx_ssd1306_contrast()`, SSD1309 has `dgx_ssd1309_contrast()` and
`dgx_ssd1309_column_offset()`, and SH1106 the same two under its own name.

JD9165 is a MIPI-DSI display for an ESP32-P4 with PSRAM. The chip keeps a
frame buffer for it, 1.2 MB at 16 bits per pixel for 1024x600, and drawing
changes that buffer, so a pixel can be read back and all eight orientations
work. The driver has the init commands of the 7-inch panel of the Guition
JC1060P470 board. JD9365 works the same way: an 800x1280 portrait panel with
the init commands of the newer panel of the 10.1-inch Guition JC8012P4A1C
board, 2 MB a frame at 16 bits. The last argument of the constructor,
`frames`, chooses how the screen is updated:

| `frames` | Frame buffers | Tearing |
| --- | --- | --- |
| `DgxDsiOneFrame` | 1 | possible |
| `DgxDsiTwoFrames` | 2 | none, for one more frame of PSRAM |
| `DgxDsiOneFrameAfterScan` | 1 | none for a virtual screen sent to the display and for a large rectangle, at the cost of a wait |

Everything else a program should know about such a display is in
[docs/dsi-usage-en.md](dsi-usage-en.md)
([Russian](dsi-usage-ru.md)): batches and an even frame rate, pieces
that bypass the processor, settings that depend on the board, several
tasks. How the bus is built and what was measured on it is in
[docs/dsi-display-en.md](dsi-display-en.md)
([Russian](dsi-display-ru.md)), and
[examples/dsi_demo](../examples/dsi_demo) is a working program.

NV3041A is the controller of 4.3-inch 480x272 panels on a QSPI bus, as on
the Guition JC4827W543 (ESP32-S3). The driver has the init commands of that
board's panel, takes 16-bit color only, and starts in upright landscape. It
keeps the backlight off until the panel memory has been cleared, so the noise
the panel shows while it starts is never seen. That board has no reset line
for the panel (`GPIO_NUM_NC`) and its backlight on GPIO1:

```c
dgx_bus_protocols_t *bus = dgx_qspi_init(SPI2_HOST, SPI_DMA_CH_AUTO, GPIO_NUM_21, GPIO_NUM_48, GPIO_NUM_40,
                                         GPIO_NUM_39, GPIO_NUM_47 /* sclk */, GPIO_NUM_45 /* cs */, 40 * 1000 * 1000, 0);
dgx_screen_t *scr = dgx_nv3041a_init(bus, GPIO_NUM_NC, GPIO_NUM_1, 16, DgxScreenRGB);
```

40 MHz is the limit the datasheet sets for four lines and RGB565, and the
panel of that board works there. To stay within the datasheet, ask for
26666667, which runs at 26.7 MHz: the SPI clock is 80 MHz divided by a whole
number, and one that does not divide evenly is rounded down. 80 MHz garbles
the picture. At 18 bits a frame takes 1.5 times as long as at 16, longer
than the panel takes to show one: 18 bits are for a picture that changes
seldom.

The panel of that board now and then writes a pair of pixels just outside a
window it is sent. On a picture updated in small windows they stay behind as
dots; a picture sent whole from a virtual screen, frame after frame, does not
keep them, so send this panel whole frames. A note on the whole panel, from
how it takes pixels to its TE line, with what was measured and tried:
[docs/nv3041a-en.md](nv3041a-en.md) ([Russian](nv3041a-ru.md)).

ST77916 is the controller of round 360x360 panels, on QSPI as a rule, with
the same commands and pixels on that bus as NV3041A. The driver has the init
table of Arduino_GFX for a 1.5-inch glass, seen right at 40 MHz on a module
marked "TFT 1.53 1.0" with the glass T150XV-C16-04. It takes 16 or 18 bits,
keeps the backlight off until the panel memory is black, as NV3041A does, and
scrolls by its own hardware. An init table belongs to its glass: with the table of
another one the picture comes out as stripes, so a module of another glass
takes its own, see [A panel of your own module](#a-panel-of-your-own-module).

## Color depths

The depths each driver sets up, in bits a pixel, and the depths its
controller takes over that bus but DGX does not set up, where they are known:

| Controller | Buses | Set up by DGX | Taken by the controller, not set up |
| --- | --- | --- | --- |
| ST7735 | SPI, P8 | 12, 16, 18 | |
| ST7789 | SPI, P8 | 12, 16, 18 | |
| GC9A01 | SPI, P8 | 12, 16, 18 | |
| ILI9341 | SPI, P8 | 16, 18 | |
| ST77916 | QSPI, SPI | 16, 18 | |
| NV3041A | QSPI | 16, 18 | |
| SSD1351 | SPI, P8 | 16, 18 | |
| SSD1322 | P8, SPI | 4: 16 grays | |
| SSD1306, SSD1309, SH1106 | SPI, I2C | 1 | |
| ST7565R, ST7920 | SPI | 1 | |
| SSD1680 | SPI | 8: an ink a pixel, black, white or red | |
| JD9165, JD9365 | DSI | 16, 24 | |
| the window on the computer | none | 16, 18; 24 with `no_bus` | |
| a virtual screen | none | 1, 4, 8, 12, 16, 18, 24, 32 | |

An empty cell in the last column means that the datasheet of the controller
was not checked for more, not that there is none. 24 bits come only over
MIPI-DSI: no panel DGX drives on SPI, QSPI or the parallel bus has them set
up. Of the 18-bit depths, only those of the ST77916 and the NV3041A, both
on QSPI, have been seen on a panel; see [docs/18-bit-pixels-en.md](18-bit-pixels-en.md).

## A panel of your own module

ST7735, ST7789, ILI9341, GC9A01, NV3041A and ST77916 are descriptions of
their panels, `dgx_panel_t` of [include/drivers/dgx_panel.h](../include/drivers/dgx_panel.h),
and their constructors make a screen of one with `dgx_panel_init()`. Their
headers have them under the names `dgx_panel_st7735`, `dgx_panel_st7735_80x160`, `dgx_panel_st7789_240x240`,
`dgx_panel_st7789_240x320`, `dgx_panel_st7789_76x284`, `dgx_panel_st7789_135x240`, `dgx_panel_ili9341`, `dgx_panel_gc9a01`,
`dgx_panel_nv3041a` and `dgx_panel_st77916`. A module of another glass is a
copy of one with what the module has otherwise: the size of its glass and
where the glass lies in the memory of the controller, the table of commands
of its maker. For a glass of 240x280 that lies 20 rows down in the 240x320 of
an ST7789:

```c
static dgx_panel_t panel; /* kept by its pointer: it lives as long as the screen */
panel               = dgx_panel_st7789_240x320;
panel.height        = 280;
panel.memory_height = 320;
panel.y_offset      = 20;
dgx_screen_t *scr = dgx_panel_init(bus, &panel, rst, GPIO_NUM_NC, 16, DgxScreenRGB);
```

Turned or mirrored, the window follows the glass: with the rows going up it
lies 20 rows up from the end of the memory.

The other kind of the 0.96" ST7735 module, which Adafruit starts as
`INITR_MINI160x80_PLUGIN`, has its controller set for 132x162, the glass
26 columns and 1 row into it, and needs its colors inverted. The table
`on` goes after the screen is filled with black, with
`DGX_PANEL_CLEAR_FIRST`, and takes the INVON:

```c
static const dgx_lcd_init_cmd_t invert[] = {{0x21, 0, 0, 0, {0}}, {0, 0, 0xff, 0, {0}}}; /* INVON, the end */
static dgx_panel_t panel;
panel               = dgx_panel_st7735_80x160;
panel.memory_width  = 132;
panel.memory_height = 162;
panel.x_offset      = 26;
panel.y_offset      = 1;
panel.flags        |= DGX_PANEL_CLEAR_FIRST;
panel.on            = invert;
dgx_screen_t *scr = dgx_panel_init(bus, &panel, rst, GPIO_NUM_NC, 16, DgxScreenBGR);
```

No module of this kind has been tried. `dgx_panel_orientation()` and
`dgx_panel_backlight()` work for every such screen. A table of the maker
gives MADCTL and COLMOD `dgx_panel_adj_madctl` and `dgx_panel_adj_colmod`
as their adjust functions, so that they follow the order of colors and the
depth of the screen. A depth is set up where its COLMOD in `colmod` is not 0.
A controller with a COLMOD of 0 for a depth it takes, as the NV3041A has for
RGB666, names the depths set up in `depths` instead:
`DGX_PANEL_DEPTH_16 | DGX_PANEL_DEPTH_18`.

For SSD1306, pick a geometry from `ssd1306_resolution_t`: `SSD1306_128X32`,
`SSD1306_128X64`, `SSD1306_96X16`, `SSD1306_64X48` or `SSD1306_72X40`.

SSD1309 is the controller of the larger modules, 2.42 inches as a rule. It
takes the commands of the SSD1306 but has no charge pump, so its constructor
has no supply argument: `dgx_ssd1309_init(bus, SSD1309_128X64, rst)`, or
`SSD1309_128X32`. The init commands are those of the u8g2 driver. Some
modules wire the first column of the glass to segment 2 of the controller
and show the picture shifted right by two pixels: tell the driver with
`dgx_ssd1309_column_offset(scr, 2)`.

SSD1322 is the 16-gray OLED controller of the 256x64 modules (Newhaven
NHD-3.12-25664 and the 3.12" clones): `dgx_ssd1322_init(bus, SSD1322_256X64,
rst)`. Other modules wire their glass to other parts of its memory:
`SSD1322_256X64_ZJY`, `SSD1322_240X128`, `SSD1322_240X128_TOPWIN` and
`SSD1322_128X64` (Newhaven NHD-2.7-12864WDW3); their settings come from u8g2
and have not been tried on a module. The controller writes its memory in
groups of four pixels, so the driver keeps the picture in a 4-bit virtual
screen (8 KB for 256x64) and sends what has changed. A color is a gray level, 0 (dark) to 15 (bright), in the high
nibble: `15 << 4`. On the 8-bit parallel bus the controller needs a write
cycle of 300 ns or longer, a clock of 3.3 MHz at most. The picture is turned
by `dgx_ssd1322_orientation()` in software, as on the ST7920; brightness is
set by `dgx_ssd1322_contrast()` (0..255) and `dgx_ssd1322_brightness()`
(0..15), the gray levels by `dgx_ssd1322_grayscale(scr, levels)` (16 values,
each above the one before and none above 180), and
`dgx_ssd1322_display_mode()` turns the whole glass off, fully on, or inverts
it.

SH1106 is what most 1.3-inch 128x64 modules have, and they are often sold
as SSD1306. The two are easy to confuse and easy to tell apart: an SH1106
under the SSD1306 driver shows garbage in the top eight rows and nothing
below, because it does not know the addressing commands of the SSD1306 and
takes the whole picture into one page. It has 132 segments and the page
addressing mode only, so the driver sends a changed rectangle page by page,
each with its own address. `dgx_sh1106_init(bus, SH1106_128X64, rst)` or
`SH1106_64X32`, the 0.42-inch glass. The glass takes a run of the segments:
from segment 2 for 128 columns and from 32 for 64, as u8g2 has it; if a
module of yours shows a blank stripe at an edge, pass the segment of its
first column to `dgx_sh1106_column_offset()`. The init commands are those
of the u8g2 driver written for the controller after its datasheet.

SSD1680 is the controller of the e-paper modules of black, white and red:
`dgx_ssd1680_init(bus, SSD1680_128X296_BWR, rst, busy)` for the 2.9" module
of WeAct Studio, seen right on a module, or `SSD1680_122X250_BWR` for its
2.13" one, not tried. The driver keeps the picture as the controller takes
it, two planes of a bit a pixel, 9.5 KB for the 2.9", and drawing changes
only that. `dgx_ssd1680_refresh()` sends it and has the glass change: all of
it, flashing several times, in about 20 s on the 2.9" at room temperature;
the glass has no fast refresh of a part. `dgx_ssd1680_busy()` is true until
the glass has changed, and the next call of the driver waits for that. A
color is an ink, a byte a pixel: `DGX_BWR_BLACK`, `DGX_BWR_WHITE` or
`DGX_BWR_RED` of `dgx_bits.h`, and `dgx_rgb_to_bwr()` gives the nearest of
them, so `DGX_RED(dgx_rgb_to_bwr)` and the other colors of `dgx_colors.h`
work. Inks do not mix: a font of soft edges is drawn with `DGX_FONT_MONO`.
BUSY has to be wired. With the reset line the controller sleeps between
refreshes; without it, it never sleeps, for only a reset wakes it. The
glass keeps the picture with no power; its maker tests it with a refresh
every 150 s, and more often than every few minutes wears it.
[examples/ssd1680_demo](../examples/ssd1680_demo) is a working program.

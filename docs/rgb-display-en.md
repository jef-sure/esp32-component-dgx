# A Parallel RGB Display in DGX

[Русская версия](rgb-display-ru.md)

How DGX drives a display on the parallel RGB interface of an ESP32-S3, what a
program has to set for it, and what was measured on the board it was made
for. The bus is `src/bus/rgb_esp32.c`, the glass `src/drivers/rgb_panel.c`,
the frame buffer core `src/bus/fb_area.c`, shared with the MIPI-DSI bus.

## The calls

A program makes the bus with the pins of the board and the pixel clock, and
the screen with a description of the glass:

```c
static const dgx_rgb_pins_t pins = {
    .pclk = GPIO_NUM_42, .hsync = GPIO_NUM_39, .vsync = GPIO_NUM_40, .de = GPIO_NUM_41, .disp = GPIO_NUM_NC,
    /* D0..D15: blue from its lowest bit, then green, then red */
    .data = {GPIO_NUM_15, GPIO_NUM_7, GPIO_NUM_6, GPIO_NUM_5, GPIO_NUM_4, GPIO_NUM_9, GPIO_NUM_46, GPIO_NUM_3, GPIO_NUM_8, GPIO_NUM_16,
             GPIO_NUM_1, GPIO_NUM_14, GPIO_NUM_21, GPIO_NUM_47, GPIO_NUM_48, GPIO_NUM_45},
};
dgx_bus_protocols_t *bus = dgx_rgb_init(&pins, 33000000);
dgx_screen_t *lcd = dgx_rgb_panel_init(bus, &dgx_rgb_panel_jc8048b070n, GPIO_NUM_NC, GPIO_NUM_2, 16, DgxScreenRGB, DgxRgbTwoFrames);
```

These are the pins of a Sunton ESP32-8048S070. The pixel clock belongs to
the bus because it is the board's: the glass allows a range, and the board
holds what its PSRAM can feed while the program draws.

An RGB panel takes no commands. Its controller (EK9716, ST7262, ILI6122 and
the like) is set up by pins on the glass, and a program sees only the size
and the timing. So there is no driver per controller: a glass is a
`dgx_rgb_panel_t`, with its size, porches, sync levels and the pixel clock
its sheet allows. Another glass is another description. When the clock of
the bus lies outside that range, the log says so once and the display
starts anyway.

The last argument is one frame buffer, `DgxRgbOneFrame`, or two,
`DgxRgbTwoFrames`. The screen is turned by the bus in all eight
orientations, `dgx_screen_orientation()`, and `dgx_screen_backlight()`
switches the backlight line. A pixel can be read back. Color is 16-bit
only: the bus has the 16 lines of RGB565.

## How the bus works

The panel has no memory: the LCD peripheral sends it the picture all the
time, about 60 times a second. The frame buffer, 750 KB for 800x480, lies in
PSRAM. The GDMA does not read it there. It sends from two bounce buffers in
internal memory, 10 rows each (`CONFIG_DGX_RGB_BOUNCE_ROWS`), and whenever
it has sent one, an interrupt of the bus copies the next 10 rows of the
frame buffer into it while the other one is sent.

That copy reads the frame buffer through the cache, so whatever the
processor has drawn is seen without being written back to PSRAM first. A
batch needs nothing done at its end. After each copy the bus asks the cache
to load the rows of the next one, as esp_lcd does with a frame buffer of its
own: the cache reads them from PSRAM meanwhile, and the next copy reads the
cache.

The panel takes RGB565 with the low byte first. DGX keeps a pixel with the
high byte first. The LCD peripheral has a bit that exchanges the two bytes
as it sends them (`lcd_byte_order`); the driver of IDF does not use it. The
bus sets it, so the frame buffer keeps pixels as DGX does, and the rows of a
virtual screen are copied into it as they lie, with no work per pixel.

A pixel, a rectangle and a window are written straight into the frame
buffer by `fb_area`, which also does the orientation. With two frame
buffers the copy reads one and drawing goes into the other. Where a frame
begins, the copy takes the one it was last told to: the two change places
there, between two frames. What the one drawn into lacks of the other is
kept in tiles of 32x4 pixels and copied by the processor, by the same cores
as on a MIPI-DSI display.

## What a program sets

Four things are the program's and not the bus's, because they concern the
whole program.

**The display is made on the core that draws.** The interrupt of the copy
runs on the core that made the display; it then stops the drawing while it
copies, and the copy has PSRAM to itself. Made on the other core, the copy
ran beside the drawing, both in PSRAM, and took twice as long: at 33 MHz it
could not keep up, and the picture shook up and down.

**At 33 MHz the other core keeps off PSRAM.** What the other core does in
PSRAM slows the copy just as drawing there does. A bench that copied PSRAM
to PSRAM on the other core, 18 MB a second, broke the picture at 33 MHz even
with the display on the drawing core. Table 1 gives the clocks that hold.

Table 1. Whether the bus keeps up, by pixel clock and by what the other core
does: no copy starts after the panel has begun to send its bounce buffer.
Two frame buffers, the rows of each copy brought into the cache before it.

| Pixel clock, MHz | 33 | 28 | 24 | 20 | 16 |
| --- | --- | --- | --- | --- | --- |
| The display on the core that draws | yes | yes | yes | yes | yes |
| The display on the other core, the drawing beside it | no | no | no | yes | yes |
| The display on the core that draws, the other core copying in PSRAM | no | no | yes | yes | yes |

The second row was seen on the Sunton ESP32-8048S070 at 33, 24, 20 and
16 MHz: the bars shook at 33 MHz, at 24 MHz only the lowest line of text
moved, and at 20 and 16 MHz the picture was steady.

**A program that writes to the flash sets `CONFIG_SPIRAM_XIP_FROM_PSRAM`.**
In menuconfig it is Component config > ESP PSRAM > SPI RAM config >
"Enable Executable in place from (XiP) from PSRAM feature". Without it, for
every write to the flash IDF turns the cache off, holds every interrupt not
made with `ESP_INTR_FLAG_IRAM`, and stops the other core. The copy reads the
frame buffer through that cache, so the panel gets no new rows: on the
board writing to NVS stopped the copies for up to 38 ms at a time, the
frames fell from 59.5 to 46–48 a second, and the picture jumped up and down
while the writes went on. An interrupt made with that flag would not help: with the
cache off PSRAM cannot be read at all.

With the option the code and the constants of the program are copied from
the flash to PSRAM at start, and IDF keeps the cache on while the flash is
written. On the board, with about 27 writes to NVS a second, Wi-Fi scanning
all the while, or both, the longest time between two copies was 367 µs
against 320, and the picture stayed steady. DGX does not set it, because it
costs PSRAM: the code and the constants take their size again there. A
program for the board with DGX, Wi-Fi and NVS has 600 KB of code and 110 KB
of constants, about 0.75 MB in pages of 64 KB, as much as one frame buffer
of 800x480. With 8 MB of PSRAM that does not matter; with 2 MB two frame
buffers no longer fit. Pictures and fonts kept as constants are copied too.
A partition mapped with `esp_partition_mmap()` stays in the flash, and with
the cache no longer turned off nothing keeps a program from reading it
while the flash is written; IDF does not say such a read is safe. DGX maps
no partitions. menuconfig says so under the RGB option of DGX while the
option is off.

**`CONFIG_LCD_RGB_RESTART_IN_VSYNC` is best left off.** In menuconfig it is
Component config > ESP-Driver:LCD Controller Configurations > "Always
restart RGB LCD transmission in VSYNC". With it, up to IDF 5.5.6, 6.0.3
and 6.1, esp_lcd starts every frame from the first bounce buffer but goes
on counting which buffer to fill as before. A copy that comes more than a
period late merges two of its events into one, and from then on every copy
goes into the buffer the panel is sending: the start of every band of 10
rows shows what was there before, and the picture keeps short stripes until
the display is made again. Without the option esp_lcd counts anew at every
frame and mends itself there. Table 2 shows it on the board.

Table 2. The copies that went into the bounce buffer being sent, out of
about 8570 in three seconds of a still picture right after a phase whose
copies came late, and the three seconds after that. An ESP32-S3 with the
bus at 33 MHz, IDF 5.5.4.

| `CONFIG_LCD_RGB_RESTART_IN_VSYNC` | The first three seconds | The next three |
| --- | ---: | ---: |
| set | 8568 | 8573 |
| not set | 1 | 0 |

IDF 6.1.1 counts anew at every frame with the option too, and so do the
release branches of 5.5 and 6.0 after 5.5.6 and 6.0.3, so their next
versions will. The bus warns in the log
when the option is set with an IDF that does not, and menuconfig shows a
line under the RGB option of DGX while it is set.

## The board, and how it was checked

The bus was made for the Sunton ESP32-8048S070 and checked on one by a
contributor, by eye and by the log of its serial port. The facts below are
from the maker's archive and its schematic V1.1.

Table 3. The Sunton ESP32-8048S070.

| | |
| --- | --- |
| Module | ESP32-S3-WROOM-1, 16 MB of flash, 8 MB of octal PSRAM at 80 MHz, 240 MHz |
| Glass | JC8048B070N, 7 inches, 800x480, TN, an EK9716 in DE mode |
| Timing | rows: sync 30, back porch 16, front porch 210; frames: sync 13, back porch 10, front porch 22; 1056 x 525 clocks a frame; data on the falling edge |
| Pixel clock | the sheet of the glass asks for 29 to 38 MHz; the maker's demos run 12 and 16 |
| Data lines | blue 15, 7, 6, 5, 4; green 9, 46, 3, 8, 16, 1; red 14, 21, 47, 48, 45, each from its lowest bit; the lowest bits of each color on the glass are grounded |
| Sync | PCLK 42, HSYNC 39, VSYNC 40, DE 41; no reset line, no DISP line |
| Backlight | GPIO 2, the enable of a boost converter, on when high |
| USB | through a CH340 to UART0; the S3's own USB pins carry the I2C of the touch controller |

The board was run with IDF 5.5.4 and 6.1, the data cache at 64 KB with lines
of 64 bytes.

What can be counted was also measured on a Guition JC4827W543, an
ESP32-S3R8 with 8 MB of octal PSRAM in the chip: there the bus runs with all
its pins unset, so the LCD peripheral sends the frames of the same glass
into nothing, and the copies and their counters run as on the Sunton
board. The numbers of the two boards agree to the unit; the picture is the
Sunton's alone.

## Three ways to send the frame

Before the bus, a bench swept the pixel clock from 12 to 33 MHz in three
setups: at every clock four seconds of a still picture, then five seconds
with the processor drawing bars into the frame buffer as fast as it could.

Table 4. What the screen showed, by setup and pixel clock.

| Setup | Still picture | While drawing |
| --- | --- | --- |
| The GDMA reads the frame buffer from PSRAM, no bounce buffers | steady at every clock up to 33 MHz | jerks sideways at 12 and 14 MHz, slants at 16, loses sync from 18 on, worse as the clock rises |
| Bounce buffers filled by the driver of IDF, with `bb_invalidate_cache` | steady | steady up to 16 MHz, a single break at 18, unsteady from 20 |
| Bounce buffers filled by the bus | steady up to 33 MHz | steady up to 33 MHz |

Without bounce buffers the panel waits for PSRAM whenever the processor
uses it, and there is no clock low enough for that; this is also how
LovyanGFX 1.2.7 sends the frame. `bb_invalidate_cache` makes the driver of IDF
drop the lines of the cache it has copied from; a line the processor has
written and not yet written back is dropped with them, and the picture
loses it. The bus does not use the flag. The board holds 33 MHz,
59.5 frames a second, where the maker runs 16.

The glass takes more: at 35 MHz the board showed 63 frames a second, at
38 MHz, the top of the sheet, 68.5, with a steady picture both still and
while drawing.

## What the copy costs

The copy into the bounce buffers is the work of the processor, an
interrupt on the core that made the display. Without the rows brought into
the cache before it, the copy read them from PSRAM as it went, and at
33 MHz took half of that core; with them it reads the cache.

Table 5. The share of the core that made the display the copies take, with
the picture still and while the processor draws bars into it as fast as it
can, without the rows brought into the cache before each copy and with
them. The display on the core that draws, two frame buffers, the Guition
JC4827W543.

| Pixel clock, MHz | Frames a second | Still, before | Still, now | Drawing, before | Drawing, now |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 33 | 59.5 | 51% | 12% | 60% | 30% |
| 28 | 50.5 | 44% | 10% | 53% | 17% |
| 24 | 43.2 | 37% | 8% | 44% | 11% |
| 20 | 36.2 | 31% | 7% | 37% | 8% |
| 16 | 29.0 | 25% | 5% | 37% | 6% |

On the Sunton board at 33 MHz the same: 52% and 61% before, 12% and 29%
now. An hour on the Guition board at 33 MHz, the picture still and bars by
turns, gave no copy later than 338 µs after the one before, against 320,
and the free memory did not change.

## Two frame buffers

With one frame buffer a moving picture may tear: drawing goes into the
frame being sent. With two it does not, and the bars on the board ran even;
in exchange a picture waits for the frame that shows it. At 33 MHz a band of
800x160 put from a virtual screen went 35 times a second with one frame
buffer and 29 with two, and the picture of colors took 77 ms and 96 ms. The
second frame buffer takes another 750 KB of PSRAM.

## The counters

`dgx_rgb_counters()` gives what the copies have cost since the bus started:
frames, copies, the time of all copies and the longest, the period a copy
has, and the longest time between two copies. Read a second apart, the time
of the copies against the second is the share of the core, and against
copies times the period the share of its time a copy takes. The two
longest are of the time since the last reading.

The longest time between two copies says whether the bus keeps up. A copy
reads the cache faster than the panel sends, so it may start late as long
as it starts before the panel reaches its buffer: up to two periods between
two copies. On the board, with the display on the other core, the longest
time was 967 to 992 µs against 320 at 33 MHz, and the picture shook; 987
against 440 at 24 MHz, where only the lowest line of text moved; 655 against
528 at 20 MHz and 670 against 660 at 16 MHz, both steady.

## The bench and the advice

`dgx_rgb_bench(bus, &bench)` finds the highest pixel clock at which the bus
keeps up on the board at hand, with whatever else the program runs. It
starts from the top of what the glass allows and goes down by an eighth at
a time, eight clocks at most, until one holds by the counters: at each,
half a second with nothing drawn, then a second of filling and copying as
fast as it can in PSRAM of its own, two buffers of a third of the panel,
from the core it is called on. The picture on the screen does not change.
It is called from the task that draws, and what the other tasks of the
program do meanwhile, on either core, is measured with it. At the end the
bus runs at its own clock again.

`dgx_rgb_advise(&bench)` gives the clock that held, to give
`dgx_rgb_init()` at the next start; the structure is flat, to be kept in
NVS. `dgx_rgb_bench_text()` says the rest in words: on which core to make
the display, the work of the other core in PSRAM, XIP off, a restart that
keeps on counting. The bench logs both.

Table 6. What the bench found on the Guition JC4827W543 at the timing of
the Sunton glass, two frame buffers, by setup.

| Setup | The clock advised | The copies, still and drawing | The bench took |
| --- | --- | --- | --- |
| The display on the core that draws | 38.0 MHz | 14% and 43% | 1.6 s |
| The display on the other core | 19.4 MHz | 7% and 17% | 9.6 s |
| The display on the core that draws, the other core copying in PSRAM | 22.2 MHz | 20% and 69% | 13.6 s |

That agrees with table 1, which the Sunton board showed by eye; the bench
itself has not been run there yet. It draws harder than bars do: at 38 MHz
on the core that draws, the copies took 43% of it while the bench drew,
where bars at 33 MHz cost 30%.

The bench gives no advice on the frame buffers. On a MIPI-DSI display a
second one mends a weakness of the board, which the bench there measures.
Here the copy reads PSRAM the same with one frame buffer or two, and one
does not make the bus fall behind: it only lets a moving picture tear, as
drawing goes into the frame being sent, and whether that shows depends on
what the program draws, which no bench can know. A program that moves its
pictures takes two. One whose picture mostly stands, a clock or a panel of
controls, does with one: it keeps 750 KB of PSRAM, and a picture does not
wait for the frame that shows it. The bench draws into buffers of its own,
not on the screen, so the clock it finds holds with either.

## The copy by the GDMA

A pair of GDMA channels made the copy instead of the processor, and the
option was dropped. With the picture still it took 3% of the core instead
of 12%. But while the processor draws in PSRAM the GDMA shares PSRAM with
the drawing, where the copy by the processor stops the drawing while it
copies: the copies came late, a late one had to be finished by the
processor before the panel reached it, and at 33 MHz the core went to 61%
instead of 30%, the bars to 22 a second instead of 33.

For whoever tries it: a piece of the copy is read out of PSRAM by the TX
channel and written by the RX channel, and is aligned for both. On an
ESP32-S3 IDF gives an alignment of 1 for the TX and of 4 for an RX that
bursts; pieces of 4095 bytes, aligned for the TX only, stopped the RX at
the first one for good.

## The backlight

GPIO 2 enables the boost converter of the LEDs, and DGX switches it on and
off. Driven by PWM from the LEDC, the brightness follows only at low
frequencies, and unevenly: at 200 Hz 100% and 50% look alike, 20% is a
little darker and 5% clearly dark; at 1 kHz the light falls sooner; at
5 kHz and above anything below 100% is dark, the converter cannot follow
pulses that short.

## What is not done

- **Other boards.** Only this glass and this board were run; a glass with a
  reset or DISP line was not, nor an ESP32-P4 on RGB. The LCD peripheral of
  the P4 has 24 data lines, the whole RGB888 of a glass, where that of the
  S3 has 16; the bus builds for the P4 under IDF 5.5.4 and 6.1, and takes
  16 of them.

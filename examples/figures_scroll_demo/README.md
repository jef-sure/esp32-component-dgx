# DGX Figures and Scroll Demo

Ellipses, arcs and sectors, and a picture scrolled, on a CYD, each in a way it
is used in a program:

- a pie of sectors with an outline around it, a sector over a half turn with
  its arc, sectors of an ellipse: `page_figures()`;
- the sign of Wi-Fi, a dot and arcs of a quarter of a turn over it that come
  one by one, by a soft pen on a page of ink: `wifi_sign()`, an arc a call;
- lines of text that go up in a band, a band that goes sideways, a part of it
  that goes aslant: a virtual screen moved in its memory by `dgx_scroll()` and
  sent to the display, `page_memory()`;
- a chart that goes left while a new piece is drawn at its right edge, with
  what is beside it standing still: the panel scrolls it itself, the screen
  lying, `page_chart()`;
- a terminal with a head and a foot that stay and lines that go up between
  them: the panel scrolls it itself, the screen upright, `page_terminal()`.

The same code is the test these calls were seen and timed with: the numbers
of release 0.5.5 in [CHANGES.md](../../CHANGES.md) were read from its log.

The board is a CYD, an ESP32-2432S028R: an ESP32 with a 320x240 ILI9341 on
SPI. The pins are those of the board, in `app_main()`:

- `MOSI`: GPIO 13
- `MISO`: GPIO 12
- `SCLK`: GPIO 14
- `CS`: GPIO 15
- `DC`: GPIO 2
- `Backlight`: GPIO 21

## What it shows

A round begins with the timings and then goes through seven pages. Every page
has the number of the build and its letter in a corner, and says what moves
its picture.

| Page | What | The picture is moved by |
| --- | --- | --- |
| A | Ellipses, a circle with the ellipse of equal radii over it, arcs by a hard pen, sectors, all drawn straight on the display. | nothing |
| B | Arcs by a soft pen on a page of 8 bits shown through a table: rings by pens of 1 to 5, arcs of an ellipse, the sign of Wi-Fi in four sizes growing arc by arc, and the same sign by curves of three points, as it was drawn before there was an arc. | nothing |
| C | A band of 320x112 scrolled up, to the right, down and, a part of it, aslant. | the memory of a virtual screen, sent to the display whole after every move |
| D | A chart that goes left; a new piece of it is drawn at the right edge, and a circle, a text and a bar in the middle now and then. At the end a frame at the edges of the area. | the panel |
| E | The same with the screen turned half a turn. | the panel |
| F | A terminal on the screen upright: a head and a foot stay, lines go up and then down. At the end two lines from corner to corner of the area. | the panel |
| G | The same with the screen turned half a turn. | the panel |

On pages D to G a red box says that the panel scrolls and counts the moves it
has made. The display is not read, so a move `dgx_scroll()` reports as made
was made by the panel; one it refuses is counted in the log. What is drawn
after a move, the frame, the lines, the texts, has to lie where it is asked
for: the rows of the area are elsewhere in the memory of the panel by then.
E and G have the addresses along the long side of the panel running the
other way than D and F.

## The log

The timings come first, lines that begin with `T`: the figures straight on the
display and on a virtual screen of 8 bits, the sign of Wi-Fi by arcs and by
curves, the scroll in memory on screens of 8, 16 and 12 bits, the scroll by
the panel lying and upright, and a picture sent across the place where the
rows of a scrolled area turn round against the same picture sent beside it.
Each is the least of three runs. A time of code that has not changed moves by
a fifth from one build to another, with the place the code gets in flash.

## Build, flash, read

```sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash
python read.py /dev/ttyUSB0 60 "T done"
```

`read.py` resets the board and prints its log for so many seconds, or until a
line that begins with the third argument; it needs `pyserial`, which the
environment of ESP-IDF has.

Bump `BUILD` in `main/build.h` before every flash: the number is on every
page, and tells which firmware a picture was seen on.

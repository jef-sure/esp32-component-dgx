# DGX NV3007 check

This example checks the NV3007 driver on a 2.79" 142x428 SPI bar module with
an ESP32-C3. It draws a fixed set of test pictures and prints to the serial log
what each picture should look like.

## Wiring

The default pins are the SPI pins of the ESP32-C3 and three free GPIOs next
to them, clear of its flash, USB, console and strapping pins. Module pin names
vary a little between sellers.

| Module pin | ESP32-C3 |
| --- | --- |
| VCC | 3.3V |
| GND | GND |
| SDA (SDI, DIN, MOSI) | GPIO 7 |
| SCL (CLK, SCK) | GPIO 6 |
| CS | GPIO 10 |
| DC (RS) | GPIO 3 |
| RES (RST) | GPIO 4 |
| BLK (BL, LED) | GPIO 5 |

If your wiring differs, change the `DEMO_PIN_*` constants at the top of
`main/main.c`. If RES or BLK is tied to 3.3V, put `GPIO_NUM_NC` there.

## Build and flash

You need ESP-IDF 5.2 or later. The example uses the driver from this checkout,
so build it inside the repository:

```sh
git clone -b nv3007 https://github.com/jef-sure/esp32-component-dgx.git
cd esp32-component-dgx/examples/nv3007_demo
idf.py set-target esp32c3
idf.py -p PORT flash monitor
```

Replace `PORT` with your board's serial port (for example `/dev/ttyUSB0` or
`COM3`). To leave the monitor, press `Ctrl+]`.

## What the screen shows

Every picture has the test number in its title, `TEST 1`; the log has it too.
The pictures run in a loop of about a minute:

1. **Colors** (8 s). Five bars named `RED`, `GREEN`, `BLUE`, `WHITE` and
   `BLACK`, each in the color its name says. Below them are four ramps from
   black to red, green, blue and white.
2. **Orientations** (8 pictures, 4 s each). Each picture shows a white
   square in one corner with a red arrow labeled `x` and a green arrow
   labeled `y`, a large yellow F near the square, and on the other half of the
   screen the test number, the orientation's name and size, and a small yellow
   F. In every picture all text should read normally, not mirrored or upside
   down, and the large F should look exactly like the small one, turned the
   same way. Hold the module so that the text of the first of them reads
   normally and keep it that way for all eight. Pictures 1 to 4 are portrait
   (142x428), pictures 5 to 8 landscape (428x142).
3. **Speed**. The whole screen flashes red, green, blue, white and black
   twice, quickly. Then it shows how long one full-screen fill took.
4. **Lines** (8 s). Row 1: twelve thin vertical lines. Rows 2 and 3: two
   horizontal lines that look the same. Rows 4 and 5: two dotted lines that
   look the same, fainter than rows 2 and 3.

## What to report

Confirm that every picture looks as described above and that there are no
artifacts: no stray pixels, noise, stripes or shifted lines, and no band of
noise along a long edge of the glass. If something is off, describe it; a
photo of that picture helps.

There are three switches at the top of `main/main.c` to try before reporting
a problem, one at a time, flashing again after each:

- Red and blue swapped (the `RED` bar is blue): set `DEMO_COLOR_ORDER` to
  `DgxScreenBGR`.
- Colors inverted, like a photo negative, with the `BLACK` bar white: set
  `DEMO_INVERT` to 1.
- Washed-out or wrong colors, or stripes, with the switches above not
  helping: set `DEMO_TABLE_MAIN` to 1. The driver starts the panel with the
  init table Arduino_GFX has for the 2.79" module; this starts it with the
  other table Arduino_GFX has for the NV3007.

Say which switch, if any, made the pictures right. If the screen stays white
or shows only noise, check the wiring and the 3.3V supply first.

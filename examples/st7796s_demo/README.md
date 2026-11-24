# DGX ST7796S check

This example checks the ST7796S driver on a 320x480 SPI module with an
ESP32. It draws a fixed set of test pictures and prints to the serial log
what each picture should look like.

## Wiring

The default pins are a common ESP32 wiring for SPI displays, on the VSPI
pins. Module pin names vary a little between sellers.

| Module pin | ESP32 |
| --- | --- |
| VCC | 3.3V, or 5V if the module's listing says it takes 5V |
| GND | GND |
| CS | GPIO 5 |
| RESET (RST) | GPIO 4 |
| DC (RS, A0) | GPIO 2 |
| SDI (MOSI, SDA) | GPIO 23 |
| SCK (SCL, CLK) | GPIO 18 |
| LED (BL) | 3.3V |
| SDO (MISO) | not connected |

The touch (T_...) and SD card pins are not used. If your wiring differs,
change the `DEMO_PIN_*` constants at the top of `main/main.c`. If the
backlight is wired to a GPIO instead of 3.3V, put that GPIO in
`DEMO_PIN_BACKLIGHT`.

## Build and flash

You need ESP-IDF 5.2 or later. The example uses the driver from this checkout,
so build it inside the repository:

```sh
git clone -b st7796s https://github.com/jef-sure/esp32-component-dgx.git
cd esp32-component-dgx/examples/st7796s_demo
idf.py set-target esp32
idf.py -p PORT flash monitor
```

Replace `PORT` with your board's serial port (for example `/dev/ttyUSB0` or
`COM3`). To leave the monitor, press `Ctrl+]`.

## What the screen shows

Every picture has the test number in its title, `TEST 1`; the log has it too.
The pictures run in a loop of a little over a minute:

1. **Colors, 16 bits a pixel** (8 s). Five bars named `RED`, `GREEN`,
   `BLUE`, `WHITE` and `BLACK`, each in the color its name says. Below them
   are four ramps from black to red, green, blue and white.
2. **Orientations** (8 pictures, 5 s each). Each picture shows a white
   square in one corner with a red arrow labeled `x` and a green arrow
   labeled `y`, a large yellow F near the square, and on the other half of the
   screen the test number, the orientation's name and size, and a small yellow
   F. In every picture all text should read normally, not mirrored or upside
   down, and the large F should look exactly like the small one, turned the
   same way. Hold the module so that the text of the first of them reads normally
   and keep it that way for all eight. Pictures 1 to 4 are portrait
   (320x480), pictures 5 to 8 landscape (480x320), so in those the arrows
   run along the other sides of the module.
3. **Speed**. The whole screen flashes red, green, blue, white and black
   twice, quickly. Then it shows how long one full-screen fill took.
4. **Colors, 18 bits a pixel** (8 s). The same bars as in 1; the ramps should
   look as smooth as in 1 or smoother.

## What to report

Confirm that every picture looks as described above and that there are no
artifacts: no stray pixels, noise, stripes or shifted lines, and no flicker
outside the speed test. If something is off, describe it; a photo of that
picture helps.

If red and blue are swapped (the `RED` bar is blue), change
`DEMO_COLOR_ORDER` in `main/main.c` to `DgxScreenBGR`, flash again and say
so in the report. If the colors look inverted, like a photo negative, with
the `BLACK` bar white, report it as it is. If the screen stays white or shows
noise, check the wiring first, then try `DEMO_SPI_MHZ` 20.

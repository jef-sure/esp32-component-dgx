# DGX SSD1680 Demo

The 2.9" e-paper module of black, white and red by WeAct Studio, 128x296,
an SSD1680, on SPI of an ESP32, with the driver of DGX.

Wiring, on the pins of VSPI:

| Module | GPIO |
|--------|------|
| BUSY   | 4    |
| RES    | 16   |
| D/C    | 17   |
| CS     | 5    |
| SCL    | 18   |
| SDA    | 23   |
| GND    | GND  |
| VCC    | 3.3V |

On an ESP32-WROVER, GPIO 16 and 17 belong to its PSRAM: take two other pins
for RES and D/C there.

Drawing changes the picture in memory only; `dgx_ssd1680_refresh()` sends it
and the panel shows it, which takes about 20 s, the glass flashing several
times meanwhile.

The demo shows a picture in landscape, then the eight ways the screen can lie
on the glass, each with its axes, the letter F and the build number, and the
landscape picture again, which stays: the glass keeps it with no power. Each
picture is left for 10 s. The log has the time of each refresh.

Build and flash it from this example directory:

```sh
idf.py set-target esp32
idf.py -p /dev/ttyUSB0 flash monitor
```

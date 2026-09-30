# DGX Morph Word Demo

This example uses `TerminusTTFMedium12` to morph the letters in a sequence of Russian words on a CYD with an ILI9341 display:

1. Start with an empty line.
2. Morph each letter into the next word, starting letters 250 ms apart.
3. Hold the completed word for 2 seconds, then continue to the next entry.

Each letter has its own matrix morph and glow renderer, so its motion and phosphor use an independent progress value. The cell width is chosen at startup to fit the longest word and the glow margin on the screen.

CYD wiring (same as `cyd-dotview-morphing`):

- MOSI GPIO 13, MISO GPIO 12, SCLK GPIO 14
- CS GPIO 15, DC GPIO 2, no reset pin, backlight GPIO 21
- SPI2 at 40 MHz

Build and flash from this directory:

```sh
idf.py set-target esp32
idf.py flash monitor
```

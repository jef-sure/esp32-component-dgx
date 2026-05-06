# DGX ILI9341 SPI Demo

This example drives a real ILI9341 panel over SPI on ESP32 and renders an Adafruit-style graphics test sequence using DGX primitives and font rendering.

Default wiring uses a common ESP32 layout:

- `MOSI`: GPIO 23
- `MISO`: GPIO 19
- `SCLK`: GPIO 18
- `CS`: GPIO 5
- `DC`: GPIO 16
- `RST`: GPIO 17
- `Backlight`: connected to 3.3v

The demo initializes the DGX SPI bus, creates an `ILI9341` screen in 16-bit color, switches the panel to landscape mode, and cycles through fill, text, line, rectangle, circle, and triangle scenes.

Build and flash it from this example directory:

```sh
idf.py set-target esp32
idf.py flash monitor
```

If your panel uses different pins or a different SPI clock, change them in `idf.py menuconfig` under `Example ILI9341 SPI Demo`.
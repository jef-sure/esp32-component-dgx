# DGX in an Arduino sketch

A CYD (ESP32-2432S028R: an ESP32 with a 320x240 ILI9341 on SPI) runs a
sketch with `setup()` and `loop()`: a title, a seconds hand and the time
since start, with `millis()`, `delay()` and `Serial` from Arduino and the
drawing from DGX.

The project is an ESP-IDF project with Arduino as a component:

- `main/idf_component.yml` pulls in `espressif/arduino-esp32` and
  `jef-sure/dgx`, DGX from the repository the example sits in by its
  `override_path`. In a project of your own, name `jef-sure/dgx` there
  without `override_path`.
- `sdkconfig.defaults` sets the 1 ms tick Arduino needs and
  `CONFIG_AUTOSTART_ARDUINO`, so Arduino's `app_main()` calls `setup()` and
  `loop()`, and enables the DGX SPI bus and the ILI9341 driver.
- `main/main.cpp` is the sketch. It includes the DGX headers as they are.

## Wiring

The pins of the CYD: MOSI 13, MISO 12, SCLK 14, CS 15, DC 2, backlight 21,
on `SPI2_HOST`; the panel has no reset line of its own. Change the
constants at the top of `main/main.cpp` for another board. Arduino's global
`SPI` object uses `SPI3_HOST` on an ESP32, the host the SD card slot of the
CYD is wired to: the sketch may use it for the card, beside the display.

## Build and flash

The first build downloads the Arduino core and its dependencies.

```sh
idf.py set-target esp32
idf.py flash monitor
```

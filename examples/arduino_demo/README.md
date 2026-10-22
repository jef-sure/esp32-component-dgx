# DGX in an Arduino sketch

A CYD (ESP32-2432S028R: an ESP32 with a 320x240 ILI9341 on SPI) runs a
sketch with `setup()` and `loop()`: a title, a seconds hand and the time
since start, with `millis()`, `delay()` and `Serial` from Arduino and the
drawing from DGX.

The project is an ESP-IDF project with Arduino as a component:

- `main/idf_component.yml` pulls in `espressif/arduino-esp32`; DGX comes from
  the repository the example sits in (`EXTRA_COMPONENT_DIRS` in
  `CMakeLists.txt`). In a project of your own, add `jef-sure/dgx` to the
  same file instead.
- `sdkconfig.defaults` sets the 1 ms tick Arduino needs and
  `CONFIG_AUTOSTART_ARDUINO`, so Arduino's `app_main()` calls `setup()` and
  `loop()`, and enables the DGX SPI bus and the ILI9341 driver.
- `main/main.cpp` is the sketch. It includes the DGX headers as they are.

## Wiring

The pins of the CYD: MOSI 23, MISO 19, SCLK 18, CS 5, DC 16, RST 17,
backlight 21, on `SPI3_HOST`. Change the constants at the top of
`main/main.cpp` for another board. Arduino's global `SPI` object uses
`SPI3_HOST` on an ESP32 as well: if the sketch needs it for an SD card or a
touch controller, give it the other host with `SPIClass(HSPI)`.

## Build and flash

The first build downloads the Arduino core and its dependencies.

```sh
idf.py set-target esp32
idf.py flash monitor
```

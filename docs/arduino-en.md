# DGX in an Arduino sketch

What the recipe [An Arduino sketch](../README.md#an-arduino-sketch) of the
README leaves out: the Arduino libraries that share a bus with a display,
and the same project built by PlatformIO.

## SPI and I2C shared with Arduino libraries

Two Arduino libraries meet DGX on the hardware:

- `SPI`, and libraries built on it such as `SD`, drive their SPI host
  directly, past the ESP-IDF driver DGX uses. The global `SPI` object takes
  `SPI3_HOST` on an ESP32 and `SPI2_HOST` on the other chips. Give the
  display a host of its own, or give the library the other one with
  `SPIClass(HSPI)`.
- `Wire` runs on the ESP-IDF I2C master driver, so a display can share its
  bus with the sensors of the sketch (ESP-IDF 5.4 or newer):

  ```cpp
  Wire.begin(21, 22, 400000);
  auto i2c = (i2c_master_bus_handle_t)i2cBusHandle(0);
  dgx_screen_t *oled = dgx_ssd1306_init(dgx_i2c_init_on_bus(i2c, 0x3C, 400000), SSD1306_128X64, 0, GPIO_NUM_NC);
  ```

## PlatformIO

PlatformIO builds the same project for those who already use it:
`framework = espidf, arduino` in `platformio.ini` runs the same CMake
build, `sdkconfig.defaults` keeps working, and `pio run -t menuconfig`
opens the same configuration. Take the layout of the platform's
`espidf-arduino-blink` example, with the sketch under `src/`, and:

```ini
[env:cyd]
platform = espressif32
framework = espidf, arduino
board = esp32dev
```

Arduino comes from the platform package in PlatformIO, so it is not a
dependency there; `jef-sure/dgx`, named in an `idf_component.yml` next to
the sketch, is the only one to add.

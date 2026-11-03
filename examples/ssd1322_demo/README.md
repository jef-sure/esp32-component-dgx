# DGX SSD1322 P8 Demo

The SSD1322 256x64 OLED of 16 grays on the 8-bit parallel (Intel 8080) bus
of an ESP32, with the driver of DGX.

Default wiring (as wired on the test board):

| SSD1322 | GPIO | Contact |
|---------|------|---------|
| D0      | 13   | IO13    |
| D1      | 14   | IO14    |
| D2      | 16   | RX2     |
| D3      | 17   | TX2     |
| D4      | 18   | IO18    |
| D5      | 19   | IO19    |
| D6      | 21   | IO21    |
| D7      | 22   | IO22    |
| WR      | 25   | IO25    |
| RD      | 26   | IO26    |
| CS      | 27   | IO27    |
| DC      | 32   | IO32    |
| RES     | 33   | IO33    |

The parallel clock is 2.5 MHz: the SSD1322 takes a write cycle of 300 ns at
the shortest (3.3 MHz), and the I2S bus of an ESP32 runs at 40 MHz divided
by a whole number.

The demo cycles through four scenes — the 16 gray bars, text in two fonts,
lines and circles, and a smooth gray ramp — and logs the time of each.

Build and flash it from this example directory:

```sh
idf.py set-target esp32
idf.py -p /dev/ttyUSB1 flash monitor
```

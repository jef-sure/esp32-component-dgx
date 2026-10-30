# 18-bit color on SPI and parallel panels

Every color panel that DGX drives over SPI or the 8-bit parallel bus (P8)
can take 18 bits per pixel (RGB666) instead of the usual 16 (RGB565):
ST7735, ST7789, ILI9341, GC9A01, SSD1351 and ST77916, the last on QSPI too.
Pass `18` as `color_bits` to the driver's init function. You get 262,144 colors instead of 65,536 (red
and blue get six bits instead of five; green has six either way), at the
price of three bytes per pixel on the wire instead of two. The DSI panels
(JD9165, JD9365) take 16 or 24 bits and are not covered here.

## A pixel in memory and on the wire

`DGX_RGB_18(r, g, b)` and `dgx_rgb_to_18()` build an 18-bit value as three
bytes: red, green, blue. Each channel sits in the high six bits of its own
byte, and the low two bits are zero. A virtual screen at 18 bits stores its
pixels as three bytes in the same order, just like a 24-bit one, so putting
it on a display copies its rows unchanged.

The bus sends these bytes as they are. Controllers that follow the MIPI
display command set (ST7735, ST7789, ILI9341, GC9A01) read the six bits of
each channel from D7..D2 and ignore D1..D0, so they need nothing more. Their
drivers only select the 18-bit format.

Table 1. How each driver selects 18 bits, and where the controller expects
the six bits of a channel.

| Controller | 18 bits selected by | Bits of a channel |
| --- | --- | --- |
| ILI9341, ST7789, ST77916 | COLMOD (0x3A) = 0x66 | D7..D2 |
| ST7735, GC9A01 | COLMOD (0x3A) = 0x06 | D7..D2 |
| SSD1351 | remap (0xA0), bits 7:6 = 10b | D5..D0 |

On the wire, 18 bits take half as long again as 16. A full 240x320 frame is
153,600 bytes at 16 bits and 230,400 bytes at 18; at a 40 MHz SPI clock
that is 30.7 ms and 46.1 ms of transfer alone.

## The SSD1351

The SSD1351 is the exception. In its 262k-color mode, over SPI and the
8-bit bus, it takes each channel in the low six bits of a byte, D5..D0, and
ignores D7 and D6 (Table 8-8 of the datasheet, revision 1.3). Sent as DGX
stores them, the bytes would lose the top two bits of every channel.

So at 18 bits the SSD1351 driver converts the bytes on their way out:

- `dgx_ssd1351_init()` allocates a 768-byte buffer (256 pixels) through the
  bus's memory allocator, so that the bus can read it by DMA. If the
  allocation fails, the init function returns NULL.
- The screen's `write_area` copies each byte, shifted right by two bits,
  into this buffer and sends it, up to 768 bytes at a time.
- The pixels passed in are left unchanged, so virtual screens, colors from
  `dgx_rgb_to_18()` and everything else work exactly as with the other
  panels.

At 16 bits there is no buffer and no conversion.

## What the conversion costs

The figures in this section are estimates from instruction counts and the
datasheet. No SSD1351 has been timed.

Before converting each piece, the driver waits until the bus has finished
sending the previous one, because both use the same buffer. The conversion
and the transfer therefore never overlap: the bus sits idle while the CPU
shifts.

On an ESP32 (the chip of the CYD board; IDF 5.5.4, `-O2`), the shift loop
is seven Xtensa instructions per byte, about 5,400 for a full piece: some
22 µs at 240 MHz.

Table 2. One 768-byte piece at the fastest clock the SSD1351 allows
(estimated).

| Bus | Limit in the datasheet | Sending 768 bytes | Bus idle during the shift |
| --- | --- | --- | --- |
| SPI | 50 ns per bit (Table 13-4), 20 MHz | 307 µs | about 7% |
| 8-bit (8080) | 300 ns per write (Table 13-3) | 230 µs | about 9% |

Starting each transfer adds a little more on top.

Two improvements are possible. Neither has been made, because their effect
could not be measured:

- **Keep the buffer pointer in a local variable.** The loop reloads the
  pointer from the screen structure for every byte, because, as far as the
  compiler knows, a byte store could change it. With a local copy the loop
  is five instructions per byte.
- **Convert one half of the buffer while the other half is being sent.**
  When `write_data_async()` of the SPI or P8 bus returns, only that transfer
  is still reading its data, so the half sent before it is free again; the
  transfers of virtual screens rely on the same rule. Within the same 768
  bytes this doubles the number of transfers, each with its own start-up
  cost, so the gain would be a few percent at most and could be none. A
  larger buffer would cost RAM.

## How it is tested

Host tests check the bytes, not a panel:

- `test/host/test_pixels.c` checks that `dgx_rgb_to_18()` puts each channel
  in the high six bits of its own byte on the wire.
- `test/host/test_ssd1351.c` runs the driver against a model of the
  controller at 16 and 18 bits. In 262k mode the model takes three bytes per
  pixel and reads D5..D0. The test draws figures, text and rotated virtual
  screens, once with a bus buffer large enough that the driver sends in
  pieces.

An ST77916 on QSPI has been run at 18 bits, and its colors came out right. No other panel has.

## History

- Before 0.7.1, `DGX_RGB_18()` packed the three channels side by side in
  bits 23..6, so every color containing green or blue came out wrong on
  every panel.
- 0.7.1 gave each channel its own byte, as the MIPI command-set controllers
  expect.
- 0.7.2 added the conversion for the SSD1351, which had shown wrong colors
  at 18 bits all along.

# Changes

## 0.0.8 - 2026-06-14

- fixed heap out-of-bounds access in ILI9341, ST7789, ST7735 and SSD1351 drivers: driver structs now
  correctly embed `dgx_screen_with_bus_t` (instead of a bare `dgx_screen_t` + separate `bus` pointer),
  ensuring `xcmd_set`, `ycmd_set`, `cached_area` and related fields are part of the allocated object
- thanks to **yuwgle** for pointing to the problem

## 0.0.7 - 2026-06-02

- added percent, celsius, dot, `R` and `H` to `CasusDotView` font

## 0.0.6 - 2026-06-01

- fixed `:` (colon) in `CasusDotView` font vertical alignment

## 0.0.5 - 2026-06-01

- added `:` (colon) to `CasusDotView` font

## 0.0.4 - 2026-05-30

- fix GPIO handling in SPI bus functions

## 0.0.3 - 2026-05-30

- added `dgx_gauge_redraw()` to redraw the whole gauge
- improved documentation

## 0.0.2 - 2026-05-13

- added `dgx_gc9a01_display_off()` and `dgx_gc9a01_display_on()` as public GC9A01 driver functions

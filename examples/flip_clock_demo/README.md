# DGX Flip Clock Demo

A split-flap clock on a CYD with an ILI9341 display. Each digit is a small texture; when it changes, the top half of the old digit falls down as a flap and the new digit appears under and on it. The flap is a trapezoid drawn with `dgx_draw_texture_quad()`, which stretches a region of a virtual screen onto any convex quadrilateral:

1. A digit of the `CasusDotView` dot font is drawn as a card into a virtual screen and scaled into a 54x80 texture with `dgx_draw_texture_rect()`.
2. Idle digits are that texture scaled to 55x140 pixels on the panel.
3. During a flip the flap goes straight to the panel as a quad whose far edge moves towards the hinge and gets up to 24 pixels wider on each side, which reads as perspective.

Only the digits that are flipping are redrawn. The clock starts at 12:34 and runs fast, a minute every five seconds, so that there is always something to watch. The serial log reports how long a flip frame takes to draw.

The mapping is affine (no perspective correction) with nearest-texel sampling; the texture and the screen must have the same color depth.

CYD wiring:

- MOSI GPIO 13, MISO GPIO 12, SCLK GPIO 14
- CS GPIO 15, DC GPIO 2, no reset pin, backlight GPIO 21
- SPI2 at 40 MHz

Build and flash from this directory:

```sh
idf.py set-target esp32
idf.py flash monitor
```

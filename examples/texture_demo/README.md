# DGX Texture Demo

Moves a small card around a CYD with an ILI9341 display: it is scaled, turned
and wobbled, and then all three at once. The card is drawn only once, into a
texture; every frame just puts that texture onto four corners.

1. The texture is a 16-bit virtual screen of 96x64 pixels with a frame, the
   word "DGX" written by hand and a red copyright sign in its top-right
   corner, which also shows where the top of the card is when it turns. The
   sign is a letter of a printed font with a circle drawn around.
2. `card_corners()` finds the four corners for a frame: scaling moves them
   from the centre, wobbling swings each one on its own, rotation turns them
   all around the centre.
3. `dgx_draw_texture_quad()` stretches the texture onto those corners in a
   200x200 virtual screen, which goes to the display whole, so nothing
   flickers.

Scaling and rotation keep the card a rectangle, and the affine mapping draws
those exactly. Wobbling makes it an uneven quad; there the mapping is by two
triangles, which gives the picture its slight jelly-like bend.

The four kinds of motion are functions of time at the top of `main/main.c`;
change them or add your own to the `acts` table. The frame rate and the time
a frame takes to draw go to the serial log: about 47 FPS, 3.4-5.2 ms a frame.

CYD wiring:

- MOSI GPIO 13, MISO GPIO 12, SCLK GPIO 14
- CS GPIO 15, DC GPIO 2, no reset pin, backlight GPIO 21
- SPI2 at 40 MHz

Build and flash from this directory:

```sh
idf.py set-target esp32
idf.py flash monitor
```

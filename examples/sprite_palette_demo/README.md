# DGX Sprite and Palette Demo

A small scene on a CYD with an ILI9341 display, drawn anew every frame into
an 8-bit virtual screen of 320x240 and sent to the display through a table
of 256 colors. It shows `dgx_image_blit()` and the color tables of
`dgx_lut.h`:

- **4-bit sprites, one picture, three maps.** The three walkers are the same
  two pictures (standing and in a stride) with three maps of colors. A
  walker that turns round is mirrored around the origin at its feet, so it
  turns where it stands.
- **A reflection.** The walker on the shore has a reflection in the water:
  the same picture as a silhouette (`DGX_IMAGE_SILHOUETTE`), mirrored down
  around the feet and clipped to the water.
- **A clip.** A fourth walker crosses a window and is seen only through it
  (`DGX_IMAGE_CLIP`).
- **1 bit in eight orientations.** An arrow goes through all eight
  orientations around its centre, twice a second, with the name of each.
- **8 bits.** The coins use their pixel values as indices of the frame.
- **Palettes.** A lap of 20 seconds: day, dusk (`dgx_lut_mix()` from the day
  palette to the night one), night with lightning (`dgx_lut_mix_color()`
  with white), dawn, a fade to black and back (`dgx_lut_mix_color()` with
  black). None of it redraws a pixel: only the table changes. The two
  palettes are kept as RGB888 and the table is mixed from them anew every
  frame. The text has entries of its own, made once by
  `dgx_lut_gradient()`, and stays the same through it all.

The top line shows the build number, the frame rate and the time the frame
takes to reach the display. On a CYD: about 28 FPS, 32.2 ms a frame out.

The pictures are drawn as text in `make_sprites.py`, which writes
`main/sprites.c`; run it after changing a picture.

CYD wiring:

- MOSI GPIO 13, MISO GPIO 12, SCLK GPIO 14
- CS GPIO 15, DC GPIO 2, no reset pin, backlight GPIO 21
- SPI2 at 40 MHz

Build and flash from this directory:

```sh
idf.py set-target esp32
idf.py flash monitor
```

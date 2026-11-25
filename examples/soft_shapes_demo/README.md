# DGX Soft Shapes Demo

Shapes and textures placed between pixels, on the round 466x466 AMOLED of
the Waveshare ESP32-S3-Touch-AMOLED-1.32. Eight scenes of eight seconds
each run in a loop. In most of them the left half draws a picture with hard
edges in whole pixels, and the right half draws the same picture with soft
edges, placed in sixteenths of a pixel. The difference shows best in slow
motion: a needle or a dot that moves less than a pixel a frame jumps from
pixel to pixel on the left and glides on the right.

- **DIAL**: a dial whose needle moves slowly. Left: `dgx_draw_arc()`,
  `dgx_draw_thick_line()` for the ticks and the needle, `dgx_fill_circle()`
  for the hub. Right: `dgx_fill_ring_with()` with round ends, its lit part
  in a linear gradient from green to red; `dgx_draw_wedge_with()` for the
  ticks and for the needle, 9 pixels wide at the hub and 1.6 at the tip;
  `dgx_fill_ellipse_with()` for the hub.
- **SHAPES**: the same shapes over stripes, without and with
  `DGX_PAINT_SOFT`. A five-pointed star drawn in one stroke,
  `dgx_fill_polygon_with()`, in a linear gradient; a translucent rounded
  rectangle in a radial gradient, `dgx_fill_round_rectangle_with()`; three
  dots moving about 5 pixels a second; a translucent arc going round.
- **CHART**: values scrolling at 6 pixels a second, under a clip. Left:
  `dgx_draw_polyline()` in whole pixels. Right: `dgx_draw_polyline_with()`
  with `DGX_POLYLINE_MONOTONE`, a soft curve that never overshoots its
  values, and translucent dots at the values.
- **VEIL**: the card of [texture_demo](https://github.com/jef-sure/esp32-component-dgx/tree/main/examples/texture_demo) turning over
  stripes. On the right an `alpha_map`, an 8-bit virtual screen of
  opacities, keeps an oval in the middle opaque and fades the rest out to
  the edges.
- **TURN**: the card turning slowly. Left: `dgx_draw_texture_quad()`, the
  corners in whole pixels and the nearest texel. Right:
  `dgx_draw_texture_quad_with()`, the corners in sixteenths of a pixel,
  `DGX_TEXTURE_BILINEAR` and `DGX_TEXTURE_SOFT`.
- **FLIP**: the card turning about its vertical axis. The left call maps a
  quad that is not a parallelogram as two triangles, which bends the
  picture along a diagonal; the right one maps it in perspective.
- **LENSES**: three lenses of an 8-bit texture circling over stripes, each
  with its own table of colors. One table of opacities, `alpha`, makes
  index 0 transparent and the sectors of the ring translucent.
- **GAUGE**: the arc gauge, `dgx_gauge_init_with()` on the left and
  `dgx_gauge_init_soft()` on the right, its value going from 0 to 100 and
  back.

The line at the top shows the build number and the scene, the line at the
bottom the frame rate. Each frame is drawn in one batch,
`dgx_screen_batch_begin()` to `dgx_screen_batch_end()`, and the screen is
made with `tear_free`: the changed rectangle goes to the panel in step with
its scan, which the TE line reports, so no frame tears.

Positions in `main/main.c` are in pixels, with whole numbers at pixel
centres, as `dgx_fill_circle()` takes them. `at()` converts a position to
the sixteenths of a pixel the calls taking a struct use, where (0, 0) is the
top-left corner of pixel (0, 0); `len16()` converts a radius or a width.

## Speed

The log has a line for each scene:

```
I (...) dgx_soft_shapes_demo: build ..., DIAL: ... FPS; a frame drawn in ... ms left (HARD) and ... ms right (SOFT), sent in ... ms
```

Table 1. The frame rate, and the mean time in ms to draw each half of a
frame and to send the frame, on the Waveshare ESP32-S3-Touch-AMOLED-1.32 at
240 MHz with ESP-IDF 5.5.4. The CO5300 driver keeps the screen in PSRAM,
and the halves are drawn there; sending is the time `dgx_screen_batch_end()`
takes, the wait for the scan included.

| Scene | FPS | Left half | Right half | Sent |
|---|---|---|---|---|
| DIAL | 31.6 | 2.54 | 10.00 | 12.9 |
| SHAPES | 24.4 | 10.77 | 10.83 | 12.7 |
| CHART | 35.4 | 0.77 | 8.81 | 12.2 |
| VEIL | 19.6 | 9.88 | 22.62 | 12.0 |
| TURN | 35.7 | 2.44 | 7.00 | 12.4 |
| FLIP | 34.5 | 1.99 | 8.19 | 12.6 |
| LENSES | 15.0 | 45.84, all three lenses | — | 14.2 |
| GAUGE | 262 | 0.22 | 0.16 | 2.7 |

The hard shapes of SHAPES go through the same calls as the soft ones, so
soft edges cost little there. A gauge redraws and sends only the steps its
value has changed, hence its frame rate.

## Board

- ESP32-S3 with 8 MB of flash and 8 MB of octal PSRAM
- QSPI on SPI2 at 40 MHz: CS GPIO 10, SCLK GPIO 11, D0 to D3 GPIO 12 to 15
- reset GPIO 8, TE GPIO 9, no backlight line

The scenes are laid out for a round 466x466 screen.

Build and flash from this directory:

```sh
idf.py set-target esp32s3
idf.py flash monitor
```

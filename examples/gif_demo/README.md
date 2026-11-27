# DGX GIF Animations Demo

Three GIF animations on a CYD with an ILI9341 display. The build makes each
GIF into a file of pictures of DGX (see
[docs/pictures-en.md](../../docs/pictures-en.md#animations)): every frame
whole, as the GIF draws it over the one before, with its delay and the
number of times the animation is played. The program draws the frame the
clock has come to:

- **A spinner**, 4 bits, its background transparent, shown twice its size,
  played for ever. Its transparent pixels take the color of the paper.
- **A bouncing ball**, 8 bits, a ball squashed where it lands and its
  shadow, twice its size, for ever.
- **A bar filling**, 4 bits, played 3 times as its GIF says, then standing
  on its last frame. The line beside it tells the round. It starts again
  every 20 seconds.

The frames go on the paper with `DGX_IMAGE_OPAQUE`, so a frame drawn over
the one before writes every pixel of its box once, in one window, and does
not blink. Under each animation stand its frame and the time that frame
took to draw; the top line shows the build number.

The three lines of `main/images.txt` are the needs:

```
spinner GIF spinner.gif BITS 4
ball    GIF ball.gif BITS 8
bar     GIF bar.gif BITS 4
```

and `main/CMakeLists.txt` names the file with `dgx_image_needs_file()`. The
files are embedded as `dgx_asset_spinner` and the like, declared in
`dgx_assets.h`; `dgx_image_file_frame()` gives the frame of a time.

The GIFs are drawn by `make_gifs.py`, which writes them with the standard
library of Python alone; run it after changing an animation.

CYD wiring:

- MOSI GPIO 13, MISO GPIO 12, SCLK GPIO 14
- CS GPIO 15, DC GPIO 2, no reset pin, backlight GPIO 21
- SPI2 at 40 MHz

Build and flash from this directory:

```sh
idf.py set-target esp32
idf.py flash monitor
```

On the computer, in a window of SDL2 in place of the CYD, with the same
`main/main.c`, from a shell without ESP-IDF activated:

```sh
make run
```

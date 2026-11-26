# Pictures from PNG files

A program keeps its pictures as PNG files beside its sources, and the build
makes each set of them into a file of bytes, `.dgxi`, embedded in the
firmware. At run time DGX reads the pictures where they lie: the pixels stay
in flash, and only a small structure is held in RAM. The same file read from
a file system or from the network works the same way.

- [A need](#a-need)
- [Needs in a file](#needs-in-a-file)
- [Drawing the pictures](#drawing-the-pictures)
- [Animations](#animations)
- [Colors](#colors)
- [What the build makes](#what-the-build-makes)
- [A file from elsewhere](#a-file-from-elsewhere)
- [The tool by hand](#the-tool-by-hand)
- [The format](#the-format)

## A need

A need names a set of pictures, its PNG files and how to keep them. It is
declared in the `CMakeLists.txt` of the program or of any component:

```cmake
dgx_image_need(wifi PNG art/wifi0.png art/wifi1.png art/wifi2.png BITS 1)  # three states of an icon, 1 bit
dgx_image_need(logo PNG art/logo.png BITS 16)                              # RGB565, its transparency kept
dgx_image_need(gauge PNG art/dial.png BITS 8 REDUCE DITHER)                # 255 colors at most, for a table
```

Paths are resolved from the `CMakeLists.txt` that gives them. The words:

| Word | What it does |
| --- | --- |
| `PNG <file>...` | the pictures of the set, in this order |
| `GIF <file>` | or the frames of one GIF, each whole, with their delays: an animation, see [Animations](#animations) |
| `BITS <n>` | 1, 4, 8, 12, 16, 18, 24 or 32 bits a pixel, every depth of a virtual screen; 16 if not given |
| `INDEXED`, `PIXELS` or `MASK` | the kind of the set, see below; 1, 4 and 8 bits are `INDEXED` unless another is given, the deeper ones are `PIXELS` |
| `REDUCE` | an indexed set with more colors than its depth holds is reduced to them; without it, such a set is refused |
| `DITHER` | the error of every pixel is spread over its neighbours (Floyd-Steinberg), at any depth |
| `INK DARK` or `INK LIGHT` | at 1 bit, and for a mask of a PNG without transparency: whether the dark or the light pixels are the ink; `LIGHT` if not given |
| `ALPHA` | an indexed set keeps the opacity of the PNG in its palette, for an 8-bit texture |
| `BACKGROUND <rrggbb>` | the transparent places are painted in this color, mixed by their opacity |
| `ORIGIN <x> <y>` | the origin of every picture of the set; 0 0 if not given |
| `DELAY <ms>` | a set of PNG pictures as an animation, every picture shown so long |
| `LOOPS <n>` | how many times an animation is played, 0 for ever; a GIF's own, or 0, if not given |

The pictures of a set share one depth and one palette, and may differ in
size: the states of an icon, the frames of a sprite. A picture is at most
65535 pixels a side.

**The three kinds.** A set is one of three kinds, by the way DGX draws it:

- `INDEXED`, 1, 4 or 8 bits: the layout of `dgx_image_t`, drawn by
  `dgx_image_blit()`. Its palette holds the colors of the source. With fewer
  than 2^bits colors, the value 0 is transparent and the palette gives the
  colors of the values from 1; with exactly 2, 16 or 256 colors nothing is
  transparent and the palette starts at the value 0.
- `PIXELS`, any depth: the layout of a virtual screen, drawn as a virtual
  screen over the bytes of the file. The values are those a virtual screen
  of the depth holds: the brightness at 1, 4 and 8 bits, as
  `dgx_screen_rgb()` gives it (for a monochrome display, the 16 grays of an
  SSD1322); RGB at 12, 16, 18 and 24 bits; `0xAARRGGBB` at 32 bits, `AA` the
  opacity as in a PNG. A picture of 1 to 24 bits whose PNG has transparent
  places keeps them in a plane of opacity beside it, a byte a pixel. A
  picture of 16 bits is a `dgx_image_t` too, drawn by `dgx_image_blit()`.
- `MASK`, 4 or 8 bits: how much of one color covers each pixel, as the
  letters of a soft font are kept, taken from the opacity of the PNG, or
  from its brightness when it has no transparency. An icon drawn in one
  color with soft edges. Its palette has one entry, the mean color of the
  PNG where it covers.

**The origin** is the pixel of a picture that `dgx_image_blit()` puts at the
point it is given, and mirrors and rotates the picture around. An icon needs
none: 0 0 is its top left corner. The frames of a sprite need one, so that
frames of different sizes keep their feet in one place and a sprite turned
round stays where it stands.

## Needs in a file

The needs can be kept in a file of their own, one need a line, with the same
words:

```cmake
dgx_image_needs_file(images.txt)
```

```
# the pictures of the program
wifi PNG art/wifi0.png art/wifi1.png art/wifi2.png BITS 1
logo PNG "art/logo big.png" BITS 16 BACKGROUND 000000
needs more/images.txt
```

Paths are resolved from the directory of the file; a path with a space goes
in quotes, and `#` starts a comment. `needs <file>` takes the needs of
another file. A file of needs that changes configures the project again, and
a PNG named in it that changes makes its file again. A tool that writes the
needs, such as the builder of dgx-ui, writes this file and never the
`CMakeLists.txt`.

A mistake stops the configure step with the file and the line, or with the
`CMakeLists.txt` and the need:

```
images.txt:3: BITS 2: a picture takes 1, 4, 8, 12, 16, 18, 24 or 32 bits
images.txt:5: /path/art/off.png does not exist
CMakeLists.txt: dgx_image_need(logo): INK at 16 bits: ink says which pixels are 1, at 1 bit
```

## Drawing the pictures

Every set is embedded as `dgx_asset_<name>`, its length
`dgx_asset_<name>_size`, declared in `dgx_assets.h`, and read with
`dgx_image_file.h`:

```c
#include "dgx_assets.h"
#include "dgx_image_file.h"

dgx_image_file_t file;
dgx_image_file_open(dgx_asset_wifi, dgx_asset_wifi_size, &file);
```

`dgx_image_file_open()` checks the whole file and tells its depth, its kind,
how many pictures it holds and its palette. Nothing is copied or allocated:
`file` lives wherever the program keeps it, and the pictures are read from
the embedded bytes.

**An indexed picture** is a `dgx_image_t`. A 1-bit icon goes on any screen
in one color:

```c
dgx_image_t icon;
dgx_image_file_image(&file, state, &icon);   /* state: 0, 1 or 2 */
dgx_image_blit(lcd, 10, 10, &icon, &(dgx_image_blit_t){.color = DGX_WHITE(dgx_rgb_to_16)});
```

A set of 4 or 8 bits goes into an 8-bit virtual screen through a map of its
values to the indices of the table of colors of that screen. The palette of
the file gives the colors of the values, R, G, B and the opacity, 4 bytes
each; `file.first` is the value of the first entry:

```c
uint8_t map[255];
for (unsigned i = 0; i < file.colors; ++i) {
    const uint8_t *c = file.palette + 4 * i;
    map[file.first + i - 1] = nearest_index_of_my_table(c[0], c[1], c[2]);  /* map[p - 1] is the index of the value p */
}
dgx_image_blit(frame, x, y, &picture, &(dgx_image_blit_t){.map = map});
```

A picture whose palette is full, 2, 16 or 256 colors, has no transparent
value: it is drawn with `DGX_IMAGE_OPAQUE` and `bg` the index of the color
of the entry 0. An 8-bit indexed picture is also a texture of
`dgx_draw_texture_quad_with()`: a virtual screen of 8 bits over
`picture.pixels`, with a `lut` and the opacities of the palette as `alpha`.

**A picture of pixels** is drawn as a virtual screen over its bytes, and its
plane of opacity is the `alpha_map` of a texture quad:

```c
dgx_image_file_picture_t p;
dgx_image_file_picture(&file, 0, &p);
dgx_screen_t *logo = dgx_vscreen_from_array((void *)p.pixels, p.width, p.height, file.bits, DgxScreenRGB);
dgx_vscreen_to_screen(lcd, 10, 10, logo, 0);                 /* opaque */
dgx_screen_t *opacity = p.alpha ? dgx_vscreen_from_array((void *)p.alpha, p.width, p.height, 8, DgxScreenRGB) : NULL;
/* or soft over what lies under it: dgx_draw_texture_quad_with() with .texture = logo, .alpha_map = opacity */
```

Such a screen is only read: never draw on it. A picture of 16 bits is also
a `dgx_image_t`, with its plane in `alpha`, and goes by `dgx_image_blit()`
with its turns, scale and clip, see below.

### Soft edges

`dgx_image_blit()` with `DGX_IMAGE_SOFT` mixes the edges of a picture by the
opacity of every pixel, as soft text mixes its letters:

```c
/* a mask: a soft icon in any color, its box written whole over the paper, so it does not blink */
dgx_image_file_image(&wifis, state, &icon);
dgx_image_blit(lcd, 10, 10, &icon, &(dgx_image_blit_t){
    .flags = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_SOFT | DGX_IMAGE_OPAQUE, .color = white, .bg = back});

/* an indexed picture with its opacity in the palette (ALPHA): through the colors of the file */
uint32_t colors[256];
uint8_t  opacity[256];
dgx_image_file_colors(&arrows, lcd, colors, opacity);
dgx_image_blit(lcd, x, y, &arrow, &(dgx_image_blit_t){
    .flags = DGX_IMAGE_SOFT | DGX_IMAGE_OPAQUE, .colors = colors, .alpha = opacity, .bg = back});

/* a picture of 16 bits with its plane of opacity, over what lies under it */
dgx_image_blit(frame, x, y, &ball, &(dgx_image_blit_t){.flags = DGX_IMAGE_SOFT});
```

What a partly covered pixel is mixed with:

- with what lies under it, on a screen that can be read: a virtual screen
  of 4 to 24 bits, a display on MIPI-DSI;
- with `bg` with `DGX_IMAGE_OPAQUE`, on any screen: the box of the picture
  is written whole, every pixel once, so a soft icon drawn again over itself
  on a display behind a bus does not blink;
- with nothing on a display behind a bus without `DGX_IMAGE_OPAQUE`, which
  cannot be read: a pixel is put whole from half its opacity on. DGX does
  not read a display back.

Without `DGX_IMAGE_SOFT` a picture of 16 bits, or one drawn through
`colors`, puts every pixel of an opacity from 128 on whole and leaves the
others. `colors` takes an indexed picture to any screen, not only to an
8-bit virtual screen through a map, and `dgx_image_file_colors()` fills it
from the palette of the file, converted to the colors of the screen.

**Fonts too.** With `dgx_fonts_files()` every font file of the program is
embedded the same way, by the name of its file, and read in place by
`dgx_font_file_load()`:

```c
size_t need;
dgx_font_file_memory(dgx_asset_TerminusTTFMedium12, dgx_asset_TerminusTTFMedium12_size, &need);  /* 40 bytes */
```

## Animations

A GIF is made into a set of its frames, each whole, as large as the GIF,
each frame drawn over the one before as the GIF says: its transparent color,
and what is left of a frame for the next one. The file keeps the delay of
every frame and how many times the animation is played:

```cmake
dgx_image_need(spinner GIF art/spinner.gif BITS 16)          # every frame RGB565 with its transparency
dgx_image_need(blink PNG art/on.png art/off.png BITS 1 DELAY 500)   # or PNG pictures, half a second each
```

`dgx_image_file_frame()` tells the frame to show a time after the animation
began, as many times round as it is played and then its last frame for good;
`dgx_image_file_delay()` the delay of one:

```c
dgx_image_file_t spinner;
dgx_image_file_open(dgx_asset_spinner, dgx_asset_spinner_size, &spinner);
const int64_t start = esp_timer_get_time();
for (;;) {
    dgx_image_t frame;
    dgx_image_file_image(&spinner, dgx_image_file_frame(&spinner, (esp_timer_get_time() - start) / 1000), &frame);
    dgx_image_blit(lcd, x, y, &frame, &(dgx_image_blit_t){.flags = DGX_IMAGE_SOFT | DGX_IMAGE_OPAQUE, .bg = back});
    vTaskDelay(pdMS_TO_TICKS(20));
}
```

A delay of 0 or 10 ms is kept as 100 ms, as browsers play it. A DGX before
0.12.12 reads such a file as a set of pictures without their delays.

## Colors

One rule for every depth, in two branches.

- **Indexed.** The colors are kept exactly when they fit: in 2^bits - 1
  entries if the set has transparent pixels, in 2^bits if it has none. A set
  without transparent pixels that needs fewer than 2^bits colors keeps them
  from the value 1 too, so it draws without `DGX_IMAGE_OPAQUE`. A set with
  more colors is refused with their number:
  `37 colors, 4 bits hold 15 with transparency: REDUCE, or fewer colors in
  the picture`. With `REDUCE` they are cut by median cut to as many as fit.
  A pixel with an opacity under 128 is transparent and every other one is
  opaque, since `dgx_image_blit()` does not mix; with `ALPHA` the opacity
  goes into the palette, and colors of different opacities are different
  entries.
- **1 bit.** A set with transparent pixels has its opaque pixels 1, in the
  mean of their colors. A set without them has its light pixels 1, from the
  middle gray on as `dgx_screen_rgb()` counts it, or its dark ones with
  `INK DARK`; its two entries are the mean colors of the 0s and the 1s.
- **Mask.** The opacity of the PNG, or its brightness when nothing in the
  set is transparent (the light the ink, or the dark with `INK DARK`),
  rounded to the nearest of 16 or 256 levels; `DITHER` spreads the error.
- **Pixels.** Every color is rounded to the nearest of the depth; nothing is
  refused. The brightness at 1, 4 and 8 bits is
  `(77 r + 150 g + 29 b) / 256`, as `dgx_screen_rgb()` takes it.
- **`DITHER`** spreads the error of every pixel over its neighbours, at any
  depth and of either kind: the error of the rounding or of the entry
  chosen.

The colors of a PNG are taken as they are written: its gamma and color
profile are not applied, as most programs do not apply them either. A PNG of
16 bits a channel is rounded to 8.

## What the build makes

At the end of the configure step one build rule makes all the needs, with
`tools/dgx_images.py` of DGX, into `build/dgx_images/`:

- `<name>.dgxi` of every need;
- `dgx_images-files.txt`, a line `<name> <file>` a need, by which a tool
  finds the file of a need.

They are made again when a PNG, a need or the tool changes. A second rule
writes `build/dgx_assets/dgx_assets.S` and `dgx_assets.h`, which embed every
file of a picture, and every font file with `dgx_fonts_files()`, by
`.incbin`. Every file lies at an address divisible by 4 and in a section of
its own, which the linker leaves out when the program does not name it.

The address divisible by 4 is made by the build itself, on every version of
ESP-IDF. Every table of a file and the pixels of every picture start at an
offset divisible by 4 too, so 16-bit pixels are always aligned, as the
texture quad, which reads them as 16-bit words, needs. `EMBED_FILES` of
ESP-IDF does not align what it embeds, on any version, and
`target_add_binary_data(... ALIGN 4)` came only with ESP-IDF 6.0.3 and
6.1.1: a file embedded so may lie at an odd address, and
`dgx_draw_texture_quad_with()` refuses a 16-bit texture there. Embed the
pictures by their needs.

## A file from elsewhere

A file read from a file system or received over the network is read the
same way, from a buffer of `malloc()`, which is aligned:

```c
uint8_t *bytes = malloc(length);
/* ... read length bytes into it ... */
dgx_image_file_t file;
if (dgx_image_file_open(bytes, length, &file) == ESP_OK) { /* ... the pictures, while bytes lives ... */ }
```

Every offset and length of a file is held to its bounds before the file is
given, so a file that is spoiled or cut is refused with
`ESP_ERR_INVALID_ARG` and what is wrong in the log, never read past its end.
A file of a format of a later DGX is refused with `ESP_ERR_INVALID_VERSION`.
The numbers of a file are read byte by byte, so a file is read on a
processor of any byte order.

## The tool by hand

`tools/dgx_images.py` needs only Python 3.8 or later and its own library;
ESP-IDF brings one. It makes one file on the command line, with the words of
a need:

```sh
python tools/dgx_images.py make logo.dgxi PNG art/logo.png BITS 16
python tools/dgx_images.py build images.txt out/      # every need of a file, for a build on the host
```

A program that has the PNG files itself, such as the server of the builder
of dgx-ui, takes the same writer as a module:

```python
from dgx_images import make, DgxiError
data = make(["on.png", "off.png"], bits=1)          # paths, or the bytes of every PNG
```

A refusal is a `DgxiError` with the words the build prints. The writer
reads nothing but the PNG files it is given.

## The format

Numbers are little-endian; the pixels are laid out as the call that draws
them reads them, the high byte of a pixel first.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | `DGXI` |
| 4 | 2 | the version of the format: 1, or 2 for a mask, which a DGX before 0.12.11 refuses as a later format |
| 6 | 1 | bits a pixel |
| 7 | 1 | the kind: 1 indexed, 2 pixels, 3 a mask |
| 8 | 2 | pictures |
| 10 | 2 | entries of the palette: 1 to 2^bits for an indexed set, 1 for a mask, 0 for pixels |
| 12 | 4 | the length of the file |
| 16 | 4 | where the table of pictures starts |
| 20 | 4 | where the palette starts, 0 for none |
| 24 | 4 | where the text about the pictures starts, 0 for none |
| 28 | 4 | the length of the text |
| 32 | 4 | an animation: where the delays of its pictures start, 2 bytes each in milliseconds; 0 for a set |
| 36 | 2 | an animation: how many times it is played, 0 for ever |
| 38 | 26 | 0, reserved |

A picture of the table takes 16 bytes: width and height, 2 each; the origin,
x and y with a sign, 2 each; where its pixels start and where its plane of
opacity starts, 0 for none, 4 each. An entry of the palette is R, G, B and
the opacity. The text is UTF-8, lines `key: value`: the name of the need,
the PNG files, and the author, copyright and description a PNG carries.
[src/vscreen/image_file.c](../src/vscreen/image_file.c), the reader, tells
the format the same way, and `make -C test/images` checks the writer and
the reader together, on the test pictures of PngSuite among others.

# DGX Game of Life Morph Demo

Conway's Game of Life on a CYD with an ILI9341 display, where a generation does not replace the previous one but morphs into it over one second. It is [cyd-life-morphing](https://github.com/jef-sure/cyd-life-morphing) rewritten on the DGX morphing framework:

1. A generation is a `dgx_bit_matrix_t`; the rules read and write it with `dgx_matrix_get_point()` / `dgx_matrix_set_point()`.
2. `dgx_morph_create(now, next, dgx_morph_sources_life, NULL)` plans the transition once per generation: surviving cells stay, a newborn cell collects dots from all its live neighbors, and a dying cell fades unless it became a parent.
3. `dgx_morph_draw()` emits the dots of a frame into a `dgx_morph_glow_t`, and `dgx_morph_glow_present()` blends them with the phosphor and sends the picture to the screen.

The field is finite and does not wrap. When a pattern dies out or freezes into a still life, it morphs back to its seed.

| Pattern | Field | What it does |
| --- | :---: | --- |
| Gliders | 10x11 | Four gliders collide into four blocks |
| Navy (T-tetromino) | 9x9 | Grows, oscillates and settles |
| Beacon | 6x6 | Period 2 |
| Toad | 6x4 | Period 2 |
| Pulsar | 15x15 | Period 3 |
| R-pentomino | 30x25 | A long cascade cut short by the field edges |

Press the BOOT button (GPIO 0) to switch to the next pattern. The cell size is the largest odd one that fits the screen; if the renderer does not fit the heap, the demo retries with smaller cells. FPS and the generation number go to the serial log once a second.

CYD wiring:

- MOSI GPIO 13, MISO GPIO 12, SCLK GPIO 14
- CS GPIO 15, DC GPIO 2, no reset pin, backlight GPIO 21
- SPI2 at 40 MHz

Build and flash from this directory:

```sh
idf.py set-target esp32
idf.py flash monitor
```

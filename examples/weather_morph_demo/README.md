# DGX Weather Morph Demo

Shows the weather symbols of `WeatherIconsRegular27` one large symbol at a time on a CYD with an ILI9341 display, each morphing into the next, in two orders: the way weather itself may take, and each weather by day and then by night.

The orders are shown in turn, a lap of twenty symbols each.

By the way of the weather:

1. by day: clear sky, few clouds, clouds, drizzle, heavy drizzle, rain, thunderstorm;
2. by night: thunderstorm, rain, sleet, snow;
3. by day: snow, sleet, fog;
4. by night: fog, heavy drizzle, drizzle, clouds, few clouds, clear sky.

By day and by night: each weather by day and then by night, so that every second transition is between the two alike symbols of one weather, the sun turning into the moon. The weathers go clear sky, few clouds, clouds, fog, drizzle, heavy drizzle, thunderstorm, rain, sleet, snow.

The first symbol of a lap gathers from nothing, dots flying out of the center, and the last one goes back into it; after a second of an empty screen the other lap begins. A morph takes one second and a symbol is held for another one, so a lap is about 43 seconds. The name of a lap and of each symbol goes to the serial log as it begins.

The durations, and what becomes of an old cell nobody took (`MORPH_ORPHANS`, `MORPH_MERGE_RADIUS`), are defines at the top of `main/main.c`; the two orders are the tables `by_weather` and `day_and_night` below them. The planning and the drawing are the same as in `examples/glyph_morph_demo`, which explains them.

CYD wiring:

- MOSI GPIO 13, MISO GPIO 12, SCLK GPIO 14
- CS GPIO 15, DC GPIO 2, no reset pin, backlight GPIO 21
- SPI2 at 40 MHz

Build and flash from this directory:

```sh
idf.py set-target esp32
idf.py flash monitor
```

# The window on the computer

What the window of `host/window` does beyond the recipe [An effect on the
computer](../README.md#an-effect-on-the-computer) of the README, which shows
how a program makes it and how it is built: several windows, the mouse and
the keyboard, what the title tells of the board, the options, the variables
of the environment and scripts, and the build by ESP-IDF for its linux
target.

## Several windows, the mouse and the keyboard

Every call of `dgx_host_screen_init()` makes a window with a bus of its
own, so a board with two displays is two windows; those made before the
first wait are put side by side. The mouse stands for a finger on the
touch panel: `dgx_host_screen_fingers()` gives it while its left button,
pressed on the window, is held, in pixels of the glass upright, before
`turn` and before the orientation of the screen, as a touch controller
reports them. The keyboard stands for the buttons of the board:
`dgx_host_keys()` gives the keys held down by their numbers in USB HID,
which are those of `SDL_Scancode` (`SDL_SCANCODE_LEFT` is 80). A key goes
down on whatever window has the keyboard, as a board has one set of
buttons, and is let go of when the keyboard leaves the windows of the
program. Any task may ask for both.

## What the title tells

A PC draws much faster than the chip and has far more memory, so the title
of the window shows what the board would make of the effect:

- `bus`: how long what is sent from one wait of the program to the next
  would keep the bus of the board busy, on average and at most over the
  last half second, and in how many transfers. The window cuts and sends
  it as the SPI and QSPI buses of DGX do: on QSPI every transfer starts
  with an opcode and an address on one line, and its parameters go on one
  line too, only its pixels on four. To the time of the bits it adds what
  starting a transfer costs: 0.75 us by the registers of the host, as on
  the CYD, 25 us for a piece of pixels by DMA. The time the chip spends
  drawing comes on top of it and can only be measured on the board. Two
  displays on one SPI host share its wire: add their times.
- `heap`: the memory the program and DGX hold now, and the most they have
  held at once. The window's own memory and SDL's are not counted.

The same figures for the whole run are printed when the program ends, the
wire and the transfers apart. `dgx_host_screen_options_t` says what the
board has:

| Field | What it is |
| --- | --- |
| `bus_hz` | the clock the bus runs at, which `spi_device_get_actual_freq()` tells on the board: it may be below the one asked for; 0 for 40 MHz |
| `bus_lines` | 1 for SPI, 4 for QSPI |
| `buffer_max` | as in `dgx_spi_options_t`: the buffer of the bus, and so its pieces, no longer than this |
| `transfer_us`, `piece_us` | what starting a transfer by the registers and a piece by DMA cost; 0 for 0.75 and 25 us, below 0 for nothing |
| `turn` | how the board is held: the glass turned clockwise by 0, 90, 180 or 270 degrees in the window |
| `no_bus` | the display is on no bus the window stands for, as one on MIPI-DSI: no time on a wire is counted, and the title and the report say nothing of a bus. Such a window stands for a DSI display in the rest too: it takes 24 bits, and its screen is read, as a frame buffer is |
| `title` | the title of the window |

## Variables of the environment and scripts

Variables of the environment:

| Variable | What it does |
| --- | --- |
| `DGX_HOST_HEAP_MAX` | bytes the program and DGX may hold at once; an allocation over it gets NULL, as on the board |
| `DGX_HOST_BLOCK_MAX` | the largest block they may get, what `heap_caps_get_largest_free_block()` says on the board |
| `DGX_HOST_QUIT_MS` | the program ends so many milliseconds after the first window was made |
| `DGX_HOST_SHOT` | the glass is written to that file, as a PPM, when the program ends; a second window's to the same name with `-2` before the extension |
| `DGX_HOST_SCRIPT` | a file of keys and mouse presses and the times they come, for runs with nobody at the computer |

A script has a step a line: the time in milliseconds after the first window
was made, as for `DGX_HOST_QUIT_MS`, then what happens. A line comes no
earlier than the one above it; a line that starts with `#` and a blank one
are nothing:

```
# Right, then Shift with it
 500 key down Right
 800 key down Left Shift
 900 key up Right
 900 key up Left Shift
1000 mouse down 120 40
1100 mouse move 140 40
1200 mouse up
1500 window 2 mouse down 5 5
1600 window 2 mouse up
```

Keys take the names SDL gives them: `Left`, `Space`, `A`, `Left Shift`,
`Return`. They are held with those of the keyboard. The mouse of a script
is a finger on the first window, or on the one named, at a pixel of the
glass upright, as `dgx_host_screen_fingers()` tells it; it moves and goes
up on the window it went down on. The steps are read in the waits of the
program, as the keyboard and the mouse are, and a window that loses the
keyboard or the mouse lets go of none of them. A script that is wrong ends
the program and names its line, and so does a mouse on a window not made
by its time.

`heap_caps_get_free_size()`, `heap_caps_get_largest_free_block()`,
`esp_get_free_heap_size()` and their minimums tell the program what these
limits leave, and of a gigabyte without them. With `SDL_VIDEODRIVER=dummy`
no window opens. The panel of the window takes 16 and 18 bits, as a panel
on SPI does, and is not read: `dgx_screen_is_readable()` is false, soft
text without a paper is cut to one bit, as on such a panel. A window with
`no_bus` stands for a display on MIPI-DSI: it takes 24 bits too, and its
screen is read as a frame buffer is, so soft edges are mixed with what it
shows, as on the board.

## The linux target of ESP-IDF (host-esp-idf)

The same window comes with a build by ESP-IDF itself for its `linux`
target. The program is then the ESP-IDF project as it is, with FreeRTOS,
its tasks, queues and log; DGX builds the window in place of its buses and
drivers, defines `DGX_HOST_WINDOW` for the program, and links SDL2, found
by `pkg-config`. The project takes only what `main` needs, as not every
component of ESP-IDF builds for that target, and the task that makes the
windows needs a deep stack for SDL:

```cmake
# CMakeLists.txt of the project, before project.cmake
if(IDF_TARGET STREQUAL "linux")
    set(COMPONENTS main)
endif()
```

```
# sdkconfig.defaults.linux: a word of a stack is 8 bytes there, so this is 128 KB
CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384
```

```sh
idf.py --preview set-target linux
idf.py build
./build/<project>.elf
```

The task that makes the first window shows them all, in its `vTaskDelay()`
and while it sends pixels; it makes the other windows too, and
`dgx_host_screen_init()` refuses a task with less than 48 KB of stack left.
A `vTaskDelay()` of another task ends the passes of the buses that task has
sent to. The memory counted is that of the program, DGX and the components
of ESP-IDF; what FreeRTOS takes for tasks and queues is not, as a stack
there is 8 times that of the board. The linux target has no
`esp_timer_get_time()` of its own, and the window gives it.

If CMake finds the C compiler broken, with `ld: unrecognised emulation mode:
elf_x86_64`, the activated shell has put the inner `bin` directories of the
ESP toolchains, which have an `ld` of their own, in `PATH`. Leave them out
for this build:

```sh
PATH=$(printf %s "$PATH" | tr : '\n' | grep -v -- '-elf/[^/]*-elf/bin$' | paste -sd:) idf.py build
```

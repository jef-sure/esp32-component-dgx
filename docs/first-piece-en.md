# A shorter first piece of pixels

When DGX sends a picture to an SPI or QSPI display, the pixels go through
the bus buffer in pieces. While the CPU fills the first piece, the bus sits
idle. After that both work at once: one piece goes out by DMA while the CPU
fills the next. The shorter the first piece, the sooner the bus starts.

There are two ways to make the first piece shorter:

- **Cut a short start off the first piece** and send it on its own. That
  adds one piece. It was tried on two boards: the gain was tiny, and on the
  CYD it was a loss, so DGX does not do this.
- **Send the leftover first**: the last piece, which is shorter than the
  others anyway. The number of pieces stays the same. This was measured on
  the same two boards too and won in every case, so since DGX 0.9.1 fills
  and virtual screens sent through the halves do it.

## A short start as a piece of its own

### How it was built

- **A virtual screen sent through the buffer halves.** The first piece was
  half of a half, cut by the same rules as the halves: whole rows where a
  row fits, and a multiple of 4 bytes. The halves followed as usual.
- **A fill larger than the buffer.** An eighth of the buffer (a whole
  number of 8-pixel groups) went first, and the CPU filled the rest of the
  buffer while it was on the bus. A fill that fits in the buffer still went
  as one piece.

### Boards and conditions

Table 1. The boards used for the measurements.

| Board | Chip | Display | ESP-IDF | Bus buffer |
| --- | --- | --- | --- | --- |
| CYD, ESP32-2432S028R | ESP32-D0WD-V3 revision v3.1, 240 MHz, no PSRAM | ILI9341, 320x240, SPI at 40 MHz | 5.5.4 | 4000 bytes, DGX's choice |
| Guition JC4827W543 | ESP32-S3 revision v0.2, 240 MHz, octal PSRAM at 80 MHz | NV3041A, 480x272, QSPI at 40 MHz | 6.1 | 15,640 bytes, DGX's choice at the time, and 32,000 |

The virtual screen was the size of the screen: in internal memory on the
CYD, in PSRAM on the JC4827W543. It was sent through a copy into the bus
buffer, not straight from its own memory. The fill covered the whole
screen. Each measurement is 60 runs back to back with nothing else
running; the tables give the average.

### Results

Table 2. Sending one frame of a virtual screen through the halves, ms.

| Board, bus buffer | Without | With the short start | Difference |
| --- | --- | --- | --- |
| CYD, 4000 bytes | 32.048 | 32.057 | +0.009 |
| JC4827W543, 15,640 bytes | 13.882 | 13.837 | −0.045 |
| JC4827W543, 32,000 bytes | 13.673 | 13.566 | −0.107 |

Table 3. A fill of the whole screen, ms.

| Board, bus buffer | Without | With the short start | Difference |
| --- | --- | --- | --- |
| CYD, 4000 bytes | 31.385 | 31.394 | +0.009 |
| JC4827W543, 15,640 bytes | 13.450 | 13.442 | −0.008 |
| JC4827W543, 32,000 bytes | 13.328 | 13.289 | −0.039 |

Repeated runs agreed to within 0.01 ms, so the differences are real, but
small: at most 0.8 % of a frame, and negative on the CYD.

### Why the gain is so small

The short start saves at most the time the CPU would take to fill the rest
of the first piece. In return it adds a piece, and starting each piece
costs about 25 µs (ESP32-S3, ESP-IDF 6.1). How much is saved depends on how
fast the CPU fills the buffer:

- On the JC4827W543 the copy reads PSRAM. A 7.8 KB half takes about 170 µs
  to copy (an estimate: the copy takes 41 % of a 13.9 ms frame of 261 KB).
  But the next half has to be ready when the short start is out, so not all
  of that is saved: 45 µs, and 107 µs with halves twice as long.
- On the CYD the copy reads internal memory. A 2 KB half takes about 10 µs
  to copy (an estimate, not measured), less than starting one more piece
  costs. The result is a 9 µs loss.
- A fill writes the same color over and over, which is faster than any
  copy, so it gains the least.

### Why DGX does not do it

- The gain is at most 0.1 ms a frame, and only where the copy is slow.
- Whether there is a gain at all depends on the speed of the CPU, of the
  memory the pixels come from, and of the bus. A threshold such as "only
  for buffers above 8000 bytes" would be a magic number that suits one
  chip.
- The fill time could be measured on the chip and the choice made at run
  time. That would not be arbitrary, but it is a lot of machinery for a
  fraction of a percent.
- Where DMA can read a virtual screen straight from its memory, DGX 0.9.1
  sends it that way, with no copy, and there is no first piece to wait for.

## The leftover first

All pieces have the same length except the last, which is shorter when the
pixels do not divide evenly. For example, a fill of the whole CYD screen is
153,600 bytes: 38 pieces of 4000 bytes and a last one of 1600 bytes. Send
those 1600 bytes first, and the bus starts as soon as the CPU has filled
1600 bytes instead of 4000. There are still 39 pieces.

Since DGX 0.9.1, fills and virtual screens sent through the halves do
this. A leftover that ends in the middle of a byte, such as an odd number
of 12-bit pixels, stays last: otherwise the pieces after it would start in
the middle of a byte. A whole frame usually has no leftover: 272 rows are
34 halves of 8 rows on the JC4827W543 and 240 rows are 80 halves of 3 rows
on the CYD, so nothing changes for it.

### Why it is not slower

**Fills.** The buffer is filled once and sent as many times as needed.
While the leftover is on the bus, the CPU fills the rest of the buffer, and
after that it only waits for the bus. The gain is the shorter of two
times, sending the leftover and filling the rest of the buffer, so a fill
cannot lose.

**Virtual screens.** The leftover is the top rows of the rectangle: they go
first in the same window, and the full halves follow. The CPU fills the
halves by copying pixels. As long as the bus is slower than the copy, the
gain is the time to copy the part of a half that the leftover does not
have. If the CPU copied more slowly than the bus sends, the bus would end
by sending a full half instead of a short leftover, and the transfer would
take longer, though by no more than sending one half: 0.4 ms on the CYD at
40 MHz. That did not happen on either board, not even with a mirrored
picture, where each pixel takes more work.

### Measurements

The boards are those in Table 1. The bus buffer is what DGX 0.9.1 picks:
4000 bytes on the CYD and 15,632 bytes on the JC4827W543. The part of the
virtual screen went through the copy, from PSRAM on the JC4827W543 and
from internal memory on the CYD. The mirrored picture used
`DGX_VSCREEN_ORIENTED(DgxOutputMirrorX)`. Each case is 200 runs back to
back with nothing else running; the tables give the average. The fastest
and the slowest run differ by a few microseconds, and both orders send the
same number of pieces.

Table 4. JC4827W543, QSPI at 40 MHz, µs per call.

| What | Leftover | Last | First | Difference |
| --- | --- | --- | --- | --- |
| Fill of the whole screen | 11,008 bytes | 13,449.5 | 13,439.8 | −9.7 |
| Fill of 480x35 | 2336 bytes | 1792.1 | 1764.1 | −28.0 |
| Part of 460x100 through the copy | 4 rows, 3680 bytes | 5003.6 | 4941.2 | −62.4 |
| The same part mirrored | 4 rows, 3680 bytes | 5094.1 | 5029.8 | −64.3 |

Table 5. CYD, SPI at 40 MHz, µs per call.

| What | Leftover | Last | First | Difference |
| --- | --- | --- | --- | --- |
| Fill of the whole screen | 1600 bytes | 31,383.7 | 31,378.5 | −5.2 |
| Fill of 320x26 | 640 bytes | 3449.2 | 3442.4 | −6.8 |
| Part of 300x100 through the copy | 1 row, 600 bytes | 12,582.6 | 12,579.2 | −3.4 |
| The same part mirrored | 1 row, 600 bytes | 12,600.8 | 12,585.4 | −15.4 |

The leftover first won in every case. It gained most for the part of the
virtual screen on the JC4827W543, where the copy reads PSRAM: 62 to 64 µs,
about 1.3 % of the transfer. Fills gain less, because the buffer fills
quickly. On the CYD the gain is a few microseconds, 15 µs for the mirrored
picture.

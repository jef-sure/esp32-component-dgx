#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_bits.h"

/*
 * A run of pixels filled at once has to leave the bytes a pixel set after
 * a pixel leaves: every depth, every place a run can start at in a word and
 * in a byte, short runs and long ones, and nothing touched outside the run.
 */

#define ROOM 2048

static uint8_t at_once[ROOM], one_by_one[ROOM];

static void compare(uint8_t bits, int start, uint32_t first, uint32_t count, uint32_t color, uint8_t before)
{
    memset(at_once, before, ROOM);
    memset(one_by_one, before, ROOM);
    dgx_fill_pixels(bits, at_once + 8 + start, first, count, color);
    uint8_t *lp = one_by_one + 8 + start;
    for (uint32_t i = 0; i < count; ++i) lp = dgx_fill_buf_value(bits, lp, (int)(first + i), color);
    if (memcmp(at_once, one_by_one, ROOM) != 0) {
        fprintf(stderr, "%u bits, start %d, first %u, count %u, color %06x differ\n", bits, start, first, count, color);
        ++check_failures;
    }
}

int main(void)
{
    static const uint8_t  depths[] = {1, 4, 8, 12, 16, 18, 24, 32};
    static const uint32_t colors[] = {0xa1b2c3d4u, 0x00000000u, 0xffffffffu, 0x0000f0f0u, 0x00123456u, 0x80u};
    for (size_t d = 0; d < sizeof(depths); ++d) {
        for (size_t c = 0; c < sizeof(colors) / sizeof(colors[0]); ++c) {
            for (int start = 0; start < 4; ++start) {
                for (uint32_t first = 0; first < 8; ++first) {
                    for (uint32_t count = 0; count <= 70; ++count) {
                        compare(depths[d], start, first, count, colors[c], 0x00);
                        compare(depths[d], start, first, count, colors[c], 0xff);
                    }
                    compare(depths[d], start, first, 300, colors[c], 0x5a);
                }
            }
        }
    }
    /* a depth there is no layout for: nothing is written, nothing breaks */
    memset(at_once, 0x77, ROOM);
    dgx_fill_pixels(7, at_once + 8, 0, 100, 0x123456);
    for (int i = 0; i < ROOM; ++i) CHECK(at_once[i] == 0x77);
    CHECK_DONE();
}

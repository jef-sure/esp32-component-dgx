#include <stdint.h>
#include <stdlib.h>

static void dgx_set_bits8stream(uint8_t *array, size_t idx, uint8_t msb_value, uint8_t len)
{
    size_t ai  = idx >> 3;
    size_t bi  = idx & 7;
    size_t aei = (idx + len - 1) >> 3;
    if (ai == aei) {
        if (bi == 0 && len == 8) {
            array[ai] = msb_value;
        } else {
            msb_value >>= bi;
            uint8_t mask = ((uint8_t)0xff << (8 - len - bi)) & ((uint8_t)0xff >> bi);
            array[ai] &= ~mask;
            array[ai] |= msb_value & mask;
        }
    } else {
        uint8_t evalue = msb_value >> bi;
        uint8_t mask   = (uint8_t)0xff << (8 - bi);
        array[ai] &= mask;
        array[ai] |= evalue & ~mask;
        evalue     = msb_value << (8 - bi);
        size_t bei = (idx + len) & 7;
        mask       = (uint8_t)0xff >> bei;
        array[aei] &= mask;
        array[aei] |= evalue & ~mask;
    }
}

void dgx_bw_bitmap_set_pixel(uint8_t *array, int32_t width, int32_t x, int32_t y, uint32_t value) {
    uint32_t pitch = (width + 7) / 8;
    uint8_t *bc = array + y * pitch + x / 8;
    uint8_t bmask = 0x80 >> (x & 7);
    if (value)
        *bc |= bmask;
    else
        *bc &= ~bmask;
}

uint32_t dgx_bw_bitmap_get_pixel(uint8_t *array, int32_t width, int32_t x, int32_t y) {
    uint32_t pitch = (width + 7) / 8;
    uint8_t *bc = array + y * pitch + x / 8;
    uint8_t bmask = 0x80 >> (x & 7);
    return !!(*bc & bmask);
}

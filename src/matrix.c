#include <stdlib.h>
#include <string.h>

#include "dgx_matrix_morph.h"

dgx_bit_matrix_t *dgx_matrix_init(uint16_t width, uint16_t height)
{
    if (width == 0 || height == 0) {
        return NULL;
    }
    size_t bits = (size_t)width * (size_t)height;
    dgx_bit_matrix_t *matrix =
        (dgx_bit_matrix_t *)calloc(1, sizeof(*matrix) + (bits + 7u) / 8u);
    if (!matrix) {
        return NULL;
    }
    matrix->width = width;
    matrix->height = height;
    matrix->bits = (uint8_t *)(matrix + 1);
    matrix->number_of_set_cells = 0;
    return matrix;
}

void dgx_matrix_destroy(dgx_bit_matrix_t **matrix)
{
    if (matrix && *matrix) {
        free(*matrix);
        *matrix = NULL;
    }
}

void dgx_matrix_foreach_set(
    const dgx_bit_matrix_t *matrix,
    dgx_matrix_cell_func_t  func,
    void                   *user_data
)
{
    if (!matrix || !matrix->bits) {
        return;
    }
    size_t total = (size_t)matrix->width * (size_t)matrix->height;
    size_t idx = 0;
    while (idx < total) {
        /* Byte-skip: whole zero bytes advance 8 cells at once. */
        if ((idx & 7u) == 0 && total - idx >= 8 && matrix->bits[idx >> 3] == 0) {
            idx += 8;
            continue;
        }
        if ((matrix->bits[idx >> 3] & (uint8_t)(1u << (idx & 7u))) != 0) {
            if (func && !func(user_data, (int)(idx % matrix->width), (int)(idx / matrix->width))) {
                return;
            }
        }
        idx++;
    }
}

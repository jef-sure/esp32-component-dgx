#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Packed 1-bit-per-cell grid used as a morphing "scene".
 *
 * Cell (x, y) is stored in bit (y * width + x) & 7 of byte
 * (y * width + x) / 8 (LSB-first). number_of_set_cells is kept in sync by
 * dgx_matrix_set_point().
 */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t *bits;
    size_t number_of_set_cells;
} dgx_bit_matrix_t;

/**
 * @brief Allocate an all-zero bit matrix.
 * @param width  Grid width in cells.
 * @param height Grid height in cells.
 * @return Owned matrix, or NULL on failure. Release with dgx_matrix_destroy().
 */
dgx_bit_matrix_t *dgx_matrix_init(
    uint16_t width,
    uint16_t height
);

/**
 * @brief Release a matrix and set its pointer to NULL.
 * @param matrix Address of a matrix pointer. NULL and pointers to NULL are valid.
 */
void dgx_matrix_destroy(dgx_bit_matrix_t **matrix);

/**
 * @brief Read one cell.
 * @param matrix Source matrix.
 * @param x,y    Cell coordinates.
 * @return Cell value; false for out-of-range coordinates or NULL matrix.
 */
static inline bool dgx_matrix_get_point(
    const dgx_bit_matrix_t *matrix,
    int                     x,
    int                     y
)
{
    if (matrix == NULL || x < 0 || x >= matrix->width || y < 0 || y >= matrix->height) {
        return false;
    }
    size_t idx = (size_t)y * matrix->width + (size_t)x;
    return (matrix->bits[idx / 8u] & (uint8_t)(1u << (idx % 8u))) != 0;
}

/**
 * @brief Write one cell and update number_of_set_cells.
 * @param matrix Target matrix.
 * @param x,y    Cell coordinates.
 * @param value  New cell value.
 * @return true when written; false for out-of-range coordinates or NULL matrix.
 */
static inline bool dgx_matrix_set_point(
    dgx_bit_matrix_t *matrix,
    int               x,
    int               y,
    bool              value
)
{
    if (matrix == NULL || x < 0 || x >= matrix->width || y < 0 || y >= matrix->height) {
        return false;
    }
    size_t  idx  = (size_t)y * matrix->width + (size_t)x;
    uint8_t mask = (uint8_t)(1u << (idx % 8u));
    bool    old  = (matrix->bits[idx / 8u] & mask) != 0;
    if (value != old) {
        if (value) {
            matrix->bits[idx / 8u] |= mask;
            matrix->number_of_set_cells++;
        } else {
            matrix->bits[idx / 8u] &= (uint8_t)~mask;
            matrix->number_of_set_cells--;
        }
    }
    return true;
}

/**
 * @brief Callback for dgx_matrix_foreach_set().
 * @param user_data Caller context.
 * @param x,y       Set cell coordinates.
 * @return false to stop iteration early.
 */
typedef bool (*dgx_matrix_cell_func_t)(
    void *user_data,
    int   x,
    int   y
);

/**
 * @brief Invoke a callback for every set cell in scan order.
 * @param matrix    Source matrix.
 * @param func      Callback; NULL is a no-op.
 * @param user_data Opaque context passed to the callback.
 */
void dgx_matrix_foreach_set(
    const dgx_bit_matrix_t *matrix,
    dgx_matrix_cell_func_t  func,
    void                   *user_data
);

#ifdef __cplusplus
}
#endif

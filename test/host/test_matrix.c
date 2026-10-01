#include <stdint.h>

#include "check.h"
#include "dgx_bitmap.h"
#include "dgx_matrix_morph.h"
#include "dgx_morph_sources.h"

static bool count_cell(void *user_data, int x, int y)
{
    (void)x;
    (void)y;
    ++*(int *)user_data;
    return true;
}

static void test_basics(void)
{
    CHECK(dgx_matrix_init(0, 3) == NULL);
    dgx_bit_matrix_t *m = dgx_matrix_init(13, 5);
    CHECK(m && m->number_of_set_cells == 0);
    CHECK(dgx_matrix_set_point(m, 12, 4, true));
    CHECK(dgx_matrix_set_point(m, 0, 0, true));
    CHECK(dgx_matrix_set_point(m, 0, 0, true));
    CHECK(!dgx_matrix_set_point(m, 13, 0, true));
    CHECK(!dgx_matrix_set_point(m, -1, 0, true));
    CHECK(m->number_of_set_cells == 2);
    CHECK(dgx_matrix_get_point(m, 12, 4) && !dgx_matrix_get_point(m, 11, 4));
    CHECK(!dgx_matrix_get_point(m, 100, 100));
    int n = 0;
    dgx_matrix_foreach_set(m, count_cell, &n);
    CHECK(n == 2);
    dgx_matrix_destroy(&m);
    CHECK(m == NULL);
    dgx_matrix_destroy(&m);
}

static void test_helpers(void)
{
    dgx_bit_matrix_t *a = dgx_matrix_init(9, 3);
    dgx_matrix_set_point(a, 8, 2, true);
    dgx_matrix_set_point(a, 4, 1, true);

    dgx_bit_matrix_t *b = dgx_matrix_clone(a);
    CHECK(b && b != a && dgx_matrix_equals(a, b));
    CHECK(b->number_of_set_cells == 2);

    dgx_matrix_set_point(b, 4, 1, false);
    CHECK(!dgx_matrix_equals(a, b));
    CHECK(dgx_matrix_copy(b, a) && dgx_matrix_equals(a, b));

    dgx_bit_matrix_t *other = dgx_matrix_init(3, 9);
    CHECK(!dgx_matrix_copy(other, a));
    CHECK(!dgx_matrix_equals(other, a));
    CHECK(other->number_of_set_cells == 0);

    dgx_matrix_clear(b);
    CHECK(b->number_of_set_cells == 0 && !dgx_matrix_get_point(b, 8, 2));
    dgx_matrix_clear(NULL);

    CHECK(dgx_matrix_equals(NULL, NULL));
    CHECK(!dgx_matrix_equals(a, NULL));
    CHECK(dgx_matrix_clone(NULL) == NULL);

    dgx_matrix_destroy(&a);
    dgx_matrix_destroy(&b);
    dgx_matrix_destroy(&other);
}

static void test_from_bw_bitmap(void)
{
    /* 10x2, MSB-first rows padded to 2 bytes */
    uint8_t bits[] = { 0x80, 0x40, 0x01, 0x00 };
    dgx_bw_bitmap_t bmap = { .bitmap = bits, .width = 10, .height = 2, .is_stream = false };
    dgx_bit_matrix_t *m = dgx_matrix_from_bw_bitmap(&bmap);
    CHECK(m && m->width == 10 && m->height == 2);
    CHECK(m->number_of_set_cells == 3);
    CHECK(dgx_matrix_get_point(m, 0, 0));
    CHECK(dgx_matrix_get_point(m, 9, 0));
    CHECK(dgx_matrix_get_point(m, 7, 1));
    dgx_matrix_destroy(&m);

    dgx_bw_bitmap_t empty = { .bitmap = bits, .width = 0, .height = 2 };
    CHECK(dgx_matrix_from_bw_bitmap(&empty) == NULL);
    CHECK(dgx_matrix_from_bw_bitmap(NULL) == NULL);
}

int main(void)
{
    test_basics();
    test_helpers();
    test_from_bw_bitmap();
    CHECK_DONE();
}

/*
 * dgxi_dump FILE [SHIFT]: the pictures of a .dgxi file as DGX reads and draws them, printed for test_images.py.
 *
 * The file is read into a buffer SHIFT bytes past an address divisible by 8, so a file at an odd address is read too.
 * An indexed picture is drawn by dgx_image_blit() into an 8-bit virtual screen of its size, its origin put at its
 * own origin, so the picture lands at (0, 0): with no map a value p != 0 puts p, at 1 bit the color 1, and a
 * transparent 0 leaves the 0 the screen was filled with. A picture of pixels is read as a virtual screen over its
 * bytes, by dgx_get_pixel(), and its plane of opacity as an 8-bit virtual screen over the plane. Printed:
 *
 *   file <bits> <kind> <count> <colors> <first>
 *   entry <r> <g> <b> <a>                 for every entry of the palette
 *   picture <w> <h> <origin_x> <origin_y> <has a plane>
 *   row <value>...                        for every row; values as dgx_get_pixel() gives them
 *   alpha <value>...                      for every row of the plane, if there is one
 *
 * An error of the reader prints "refused <code>" and exits with 1; 2 for a wrong command line.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_draw.h"
#include "dgx_image.h"
#include "dgx_image_file.h"
#include "dgx_vscreen.h"

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "dgxi_dump FILE [SHIFT]\n");
        return 2;
    }
    const size_t shift = argc == 3 ? (size_t)atoi(argv[2]) : 0;
    FILE        *f     = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    const long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buffer = malloc((size_t)length + shift + 8);
    uint8_t *bytes  = buffer + shift;
    if (!buffer || fread(bytes, 1, (size_t)length, f) != (size_t)length) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    fclose(f);

    dgx_image_file_t file;
    esp_err_t        e = dgx_image_file_open(bytes, (size_t)length, &file);
    if (e != ESP_OK) {
        printf("refused %d\n", e);
        free(buffer);
        return 1;
    }
    printf("file %u %u %u %u %u\n", file.bits, file.kind, file.count, file.colors, file.first);
    for (unsigned i = 0; i < file.colors; ++i) {
        const uint8_t *c = file.palette + 4 * i;
        printf("entry %u %u %u %u\n", c[0], c[1], c[2], c[3]);
    }
    for (unsigned k = 0; k < file.count; ++k) {
        dgx_image_file_picture_t p;
        if (dgx_image_file_picture(&file, k, &p) != ESP_OK) return 3;
        printf("picture %u %u %d %d %d\n", p.width, p.height, p.origin_x, p.origin_y, p.alpha != NULL);
        dgx_screen_t *scr;
        if (file.kind == DGX_IMAGE_FILE_INDEXED) {
            dgx_image_t image;
            if (dgx_image_file_image(&file, k, &image) != ESP_OK || image.data != p.pixels) return 3;
            scr = dgx_vscreen_init(p.width, p.height, 8, DgxScreenRGB);
            if (!scr) return 3;
            dgx_fill_rectangle(scr, 0, 0, p.width, p.height, 0);
            const dgx_image_blit_t how = {.color = 1};
            if (dgx_image_blit(scr, p.origin_x, p.origin_y, &image, &how) != ESP_OK) return 3;
        } else {
            dgx_image_t not_indexed;
            if (dgx_image_file_image(&file, k, &not_indexed) != ESP_ERR_INVALID_ARG) return 3;
            scr = dgx_vscreen_from_array((void *)p.pixels, p.width, p.height, file.bits, DgxScreenRGB);
            if (!scr) return 3;
        }
        for (int y = 0; y < p.height; ++y) {
            printf("row");
            for (int x = 0; x < p.width; ++x) printf(" %u", (unsigned)dgx_get_pixel(scr, x, y));
            printf("\n");
        }
        dgx_screen_destroy(&scr);
        if (p.alpha) {
            dgx_screen_t *plane = dgx_vscreen_from_array((void *)p.alpha, p.width, p.height, 8, DgxScreenRGB);
            if (!plane) return 3;
            for (int y = 0; y < p.height; ++y) {
                printf("alpha");
                for (int x = 0; x < p.width; ++x) printf(" %u", (unsigned)dgx_get_pixel(plane, x, y));
                printf("\n");
            }
            dgx_screen_destroy(&plane);
        }
    }
    free(buffer);
    return 0;
}

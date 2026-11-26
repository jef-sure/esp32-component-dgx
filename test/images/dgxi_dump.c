/*
 * dgxi_dump FILE [SHIFT]: the pictures of a .dgxi file as DGX reads and draws them, printed for test_images.py.
 *
 * The file is read into a buffer SHIFT bytes past an address divisible by 8, so a file at an odd address is read too.
 * An indexed picture is drawn by dgx_image_blit() into an 8-bit virtual screen of its size, its origin put at its
 * own origin, so the picture lands at (0, 0): with no map a value p != 0 puts p, at 1 bit the color 1, and a
 * transparent 0 leaves the 0 the screen was filled with. A mask is drawn so too, soft, in the color 255 over 0: a
 * value is how much it covers, 0 to 255. A picture of pixels is read as a virtual screen over its bytes, by
 * dgx_get_pixel(), and its plane of opacity as an 8-bit virtual screen over the plane; one of 16 bits is also drawn
 * by dgx_image_blit() as a dgx_image_t, which must put the same pixels where its plane is from 128 on. Printed:
 *
 *   file <bits> <kind> <count> <colors> <first>
 *   entry <r> <g> <b> <a>                 for every entry of the palette
 *   animation <loops> <delay>...          for an animation: as dgx_image_file_loops and dgx_image_file_delay() read them
 *   picture <w> <h> <origin_x> <origin_y> <has a plane>
 *   row <value>...                        for every row; values as dgx_get_pixel() gives them
 *   alpha <value>...                      for every row of the plane, if there is one
 *
 * An error of the reader prints "refused <code>" and exits with 1; 2 for a wrong command line.
 *
 * dgxi_dump --snapshot prints two snapshots as dgx_screen_snapshot_print() puts them on a console, among lines of a
 * log: "sixteen", 7x5 of 16 bits, the pixel x, y of the value (x * 0x0841 + y * 0x1000) & 0xffff, and "four", 9x3 of
 * 4 bits, the level (x + 2 * y) % 16; for tools/dgx_snapshot.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_draw.h"
#include "dgx_image.h"
#include "dgx_image_file.h"
#include "dgx_snapshot.h"
#include "dgx_vscreen.h"

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--snapshot")) {
        dgx_screen_t *a = dgx_vscreen_init(7, 5, 16, DgxScreenRGB), *b = dgx_vscreen_init(9, 3, 4, DgxScreenRGB);
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 7; ++x) dgx_set_pixel(a, x, y, (uint32_t)(x * 0x0841 + y * 0x1000) & 0xffffu);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 9; ++x) dgx_set_pixel(b, x, y, (uint32_t)((x + 2 * y) % 16) << 4);
        printf("I (10) app: a line of the log before\n");
        if (dgx_screen_snapshot_print(a, 0, 0, 0, 0, "sixteen", NULL) != ESP_OK) return 3;
        printf("I (20) app: a line between\n");
        if (dgx_screen_snapshot_print(b, 0, 0, 0, 0, "four", NULL) != ESP_OK) return 3;
        dgx_screen_destroy(&a);
        dgx_screen_destroy(&b);
        return 0;
    }
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
    if (file.delays) {
        printf("animation %u", file.loops);
        for (unsigned i = 0; i < file.count; ++i) printf(" %u", (unsigned)dgx_image_file_delay(&file, i));
        printf("\n");
    }
    for (unsigned k = 0; k < file.count; ++k) {
        dgx_image_file_picture_t p;
        if (dgx_image_file_picture(&file, k, &p) != ESP_OK) return 3;
        printf("picture %u %u %d %d %d\n", p.width, p.height, p.origin_x, p.origin_y, p.alpha != NULL);
        dgx_screen_t *scr;
        if (file.kind != DGX_IMAGE_FILE_PIXELS) {
            dgx_image_t image;
            if (dgx_image_file_image(&file, k, &image) != ESP_OK || image.data != p.pixels) return 3;
            scr = dgx_vscreen_init(p.width, p.height, 8, DgxScreenRGB);
            if (!scr) return 3;
            dgx_fill_rectangle(scr, 0, 0, p.width, p.height, 0);
            dgx_image_blit_t how = {.color = 1};
            if (file.kind == DGX_IMAGE_FILE_MASK) how = (dgx_image_blit_t){.flags = DGX_IMAGE_SILHOUETTE | DGX_IMAGE_SOFT, .color = 255};
            if (dgx_image_blit(scr, p.origin_x, p.origin_y, &image, &how) != ESP_OK) return 3;
        } else {
            dgx_image_t image;
            const esp_err_t as_image = dgx_image_file_image(&file, k, &image);
            if (file.bits == 16 ? as_image != ESP_OK || image.alpha != p.alpha : as_image != ESP_ERR_INVALID_ARG) return 3;
            scr = dgx_vscreen_from_array((void *)p.pixels, p.width, p.height, file.bits, DgxScreenRGB);
            if (!scr) return 3;
            if (file.bits == 16) {
                /* the same picture by dgx_image_blit(): its pixels where the plane is from 128 on, the paper elsewhere */
                dgx_screen_t *blit = dgx_vscreen_init(p.width, p.height, 16, DgxScreenRGB);
                if (!blit) return 3;
                dgx_fill_rectangle(blit, 0, 0, p.width, p.height, 0x1234);
                if (dgx_image_blit(blit, p.origin_x, p.origin_y, &image, NULL) != ESP_OK) return 3;
                for (int y = 0; y < p.height; ++y)
                    for (int x = 0; x < p.width; ++x) {
                        const bool shown = !p.alpha || p.alpha[y * p.width + x] >= 128;
                        if (dgx_get_pixel(blit, x, y) != (shown ? dgx_get_pixel(scr, x, y) : 0x1234u)) return 4;
                    }
                dgx_screen_destroy(&blit);
            }
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

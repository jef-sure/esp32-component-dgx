#pragma once
/*
 * dgx_image_file.h: pictures given as bytes, a .dgxi file, read in place.
 *
 *  Copyright (c) 2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 * tools/dgx_images.py makes a file of one or more PNG pictures, and dgx_image_need() in CMake makes the files of a
 * program and embeds them, each as dgx_asset_<name> of dgx_assets.h. A program that has the bytes of a file, embedded,
 * from a file system or from the network, takes its pictures where they lie:
 *
 *     dgx_image_file_t file;
 *     dgx_image_t      icon;
 *     if (dgx_image_file_open(dgx_asset_wifi, dgx_asset_wifi_size, &file) == ESP_OK &&
 *         dgx_image_file_image(&file, 0, &icon) == ESP_OK)
 *         dgx_image_blit(lcd, 10, 10, &icon, &(dgx_image_blit_t){.color = DGX_WHITE(dgx_rgb_to_16)});
 *
 * Nothing is copied and nothing is allocated: the structures live where the caller keeps them, and the pixels are read
 * in place as long as the bytes live. A file holds a set of pictures of one depth and one kind:
 *
 * - DGX_IMAGE_FILE_INDEXED, 1, 4 or 8 bits a pixel, laid out as dgx_image_t takes them, drawn by dgx_image_blit().
 *   Its palette has the colors of the source, R, G, B and the opacity, 4 bytes each. A palette of fewer than 2^bits
 *   colors is that of the values 1..colors, and 0 is transparent; one of exactly 2^bits colors (2, 16, 256) is that of
 *   the values 0..2^bits-1, and nothing is transparent: such a picture is drawn with DGX_IMAGE_OPAQUE, bg the color of
 *   the entry 0. An 8-bit picture is also an 8-bit virtual screen, a texture with a lut, the layouts being the same.
 * - DGX_IMAGE_FILE_PIXELS, any depth of a virtual screen, 1, 4, 8, 12, 16, 18, 24 or 32 bits, laid out as a virtual
 *   screen keeps them: drawn as dgx_vscreen_from_array((void *)picture.pixels, picture.width, picture.height,
 *   file.bits, cbo). The values are what a virtual screen of the depth holds: the brightness at 1, 4 and 8 bits, as
 *   dgx_screen_rgb() gives it; the colors of dgx_rgb_to_12() .. dgx_rgb_to_24() at 12 to 24 bits; 0xAARRGGBB at 32
 *   bits, AA the opacity, 0 transparent, as the alpha of a PNG. A picture of 1 to 24 bits with transparent places has
 *   a plane of opacity beside it, a byte a pixel, rows one after another: the alpha_map of a texture quad.
 *
 * The numbers of the head and the tables of a file are read byte by byte, so a file is read at any address and on a
 * processor of any byte order. Every table and the pixels of every picture start at an offset divisible by 4: the
 * files embedded by the build of DGX and the buffers of malloc() lie at an address divisible by 4, so 16-bit pixels,
 * which a texture quad reads by words, are aligned. Every offset and length is held to its bounds before the file is
 * given, so a file that is spoiled or cut is refused, never read past its end. A file may be followed by more bytes
 * in the buffer: they are not read.
 *
 * The code of this file is in a source of its own: a program that does not call it does not carry it.
 */
#include <stddef.h>
#include <stdint.h>

#include "dgx_image.h"
#include "esp_err.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/** The version of the format of a file this DGX reads. */
#define DGX_IMAGE_FILE_VERSION 1

/** Pictures of 1, 4 or 8 bits, drawn by dgx_image_blit() through the palette of the file. */
#define DGX_IMAGE_FILE_INDEXED 1
/** Pictures of any depth of a virtual screen, drawn as a virtual screen over their bytes. */
#define DGX_IMAGE_FILE_PIXELS 2

/** A file of pictures, as dgx_image_file_open() gives it. */
typedef struct {
    uint8_t        bits;         /**< bits a pixel: 1, 4, 8, 12, 16, 18, 24 or 32 */
    uint8_t        kind;         /**< DGX_IMAGE_FILE_INDEXED or DGX_IMAGE_FILE_PIXELS */
    uint16_t       count;        /**< pictures in the file, at least 1 */
    uint16_t       colors;       /**< entries of the palette of an indexed file, 1 to 2^bits; 0 for pixels */
    uint8_t        first;        /**< the value of the first entry: 1, and 0 is transparent; 0 for a full palette, of 2^bits */
    const uint8_t *palette;      /**< R, G, B and the opacity of the values first .. first + colors - 1; NULL for pixels */
    const char    *about;        /**< a text about the pictures, lines "key: value", with no 0 at its end; NULL if none */
    uint32_t       about_length; /**< bytes of the text */
    const uint8_t *bytes;        /**< the file, for dgx_image_file_picture() */
    uint32_t       length;       /**< bytes of the file */
} dgx_image_file_t;

/** A picture of a file, as dgx_image_file_picture() gives it. */
typedef struct {
    uint16_t       width;
    uint16_t       height;
    int16_t        origin_x; /**< the pixel of the picture dgx_image_blit() puts at the point it is given */
    int16_t        origin_y;
    const uint8_t *pixels;   /**< in the layout of the kind of the file */
    const uint8_t *alpha;    /**< the plane of opacity of a picture of pixels, a byte a pixel; NULL if none */
} dgx_image_file_picture_t;

/**
 * @brief Check a file of pictures and tell what it holds.
 * @param bytes  The file; it lives as long as its pictures are used.
 * @param length Bytes of the buffer, the file and anything after it.
 * @param file   Out: the file.
 * @return ESP_OK; ESP_ERR_INVALID_VERSION for a file of a format of a later DGX; ESP_ERR_INVALID_ARG for a file that
 *         is not one of pictures, is spoiled or cut, with what is wrong in the log.
 */
esp_err_t dgx_image_file_open(const void *bytes, size_t length, dgx_image_file_t *file);

/**
 * @brief A picture of a file: its size, its origin, its pixels and its plane of opacity.
 * @param file    A file of dgx_image_file_open().
 * @param index   The picture, from 0 to file->count - 1.
 * @param picture Out: the picture.
 * @return ESP_OK; ESP_ERR_INVALID_ARG for a NULL argument or an index past the last picture, with a line in the log.
 */
esp_err_t dgx_image_file_picture(const dgx_image_file_t *file, unsigned index, dgx_image_file_picture_t *picture);

/**
 * @brief A picture of an indexed file as a dgx_image_t, for dgx_image_blit().
 * @param file  A file of dgx_image_file_open().
 * @param index The picture, from 0 to file->count - 1.
 * @param image Out: the picture, its data in the file.
 * @return ESP_OK; ESP_ERR_INVALID_ARG for a NULL argument, an index past the last picture or a file of pixels, with a
 *         line in the log.
 */
esp_err_t dgx_image_file_image(const dgx_image_file_t *file, unsigned index, dgx_image_t *image);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

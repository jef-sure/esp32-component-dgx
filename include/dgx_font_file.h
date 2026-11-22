#pragma once
/*
 * dgx_font_file.h: a font given as bytes, a .dgxf file, read into a font of DGX.
 *
 *  Copyright (c) 2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 * font2c -bin writes a font as a file of bytes beside its C, and dgx_fonts_files() in CMake makes such files of the
 * fonts of a project; font2c/dgxf_write.h says what is in one. A program that has the bytes of a file, from a file
 * system, from the network or embedded in its image, takes a font of them that every call of dgx_font.h and
 * dgx_hw_font.h draws with:
 *
 *     size_t need;
 *     if (dgx_font_file_memory(bytes, length, &need) == ESP_OK) {
 *         void *memory = malloc(need);
 *         const dgx_font_t *font;
 *         if (memory && dgx_font_file_load(bytes, length, memory, need, &font) == ESP_OK) ... draw with font ...
 *     }
 *
 * The font reads the bytes where they lie: they and the memory live as long as the font is used, and a font is let
 * go by freeing both, when no writing or morph of dgx_hw_font.h made of it is alive. At an address divisible by 4
 * the tables of the font are read in place, and the memory holds only its dgx_font_t, and the dgx_hw_font_t of a
 * handwritten font: some 70 bytes. At another address the tables are copied into the memory; the elements of a
 * handwritten font too at an odd one. The pictures and the dots are always read in place. Fonts do not share
 * anything: any number of them may be used at once.
 *
 * Every offset and length of the file is held to its bounds before a font is given, so a file that is spoiled or
 * cut is refused, never read past its end. The memory is aligned as malloc() gives it. A file may be followed by
 * more bytes in the buffer: they are not read.
 *
 * The code of this file is in a source of its own: a program that does not call it does not carry it.
 */
#include <stddef.h>
#include <stdint.h>

#include "dgx_font.h"
#include "esp_err.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/** The version of the format of a file this DGX reads. */
#define DGX_FONT_FILE_VERSION 1

/**
 * @brief How much memory a font of these bytes takes, for the address they lie at.
 * @param bytes  The file, at the address it is going to be read at.
 * @param length Bytes of the buffer, the file and anything after it.
 * @param memory Out: bytes of memory dgx_font_file_load() is to be given.
 * @return ESP_OK; ESP_ERR_INVALID_VERSION for a file of a format of a later DGX; ESP_ERR_INVALID_ARG for a file
 *         that is not a font, is spoiled or cut, with what is wrong in the log; ESP_ERR_NOT_SUPPORTED on a processor
 *         of the other byte order.
 */
esp_err_t dgx_font_file_memory(const void *bytes, size_t length, size_t *memory);

/**
 * @brief A font of the bytes of a file, its tables laid in memory.
 * @param bytes  The file; it lives as long as the font.
 * @param length Bytes of the buffer, the file and anything after it.
 * @param memory size bytes, aligned as malloc() gives it, which live as long as the font.
 * @param size   At least what dgx_font_file_memory() said for these bytes.
 * @param font   Out: the font.
 * @return ESP_OK; ESP_ERR_INVALID_SIZE when the memory is too small or not aligned; the rest as
 *         dgx_font_file_memory() returns them, the file being checked again.
 */
esp_err_t dgx_font_file_load(const void *bytes, size_t length, void *memory, size_t size, const dgx_font_t **font);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

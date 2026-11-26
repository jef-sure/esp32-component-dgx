#pragma once
/*
 * dgx_snapshot.h: a snapshot of a screen, the picture it holds, as a .dgxi file: what a real device shows, taken off
 * it, not a picture the computer draws in its place.
 *
 *  Copyright (c) 2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 * A snapshot is a file of pictures of DGX (dgx_image_file.h) of one picture of pixels, of the depth of the screen,
 * which tools/dgx_images.py turns into a PNG. dgx_screen_snapshot() hands its bytes to a function of the program, a
 * piece at a time, to write to a file system, a socket or a port; dgx_screen_snapshot_print() writes them to the
 * console as lines of base64 between marks, which tools/dgx_snapshot.py reads off the serial port, or off a log
 * saved by any terminal, and makes into a .dgxi file and a PNG:
 *
 *     dgx_screen_snapshot_print(frame, 0, 0, 0, 0, "dial", NULL);   // the whole screen, to stdout
 *
 *     python tools/dgx_snapshot.py /dev/ttyUSB0 --out shots/        // shots/dial.dgxi and shots/dial.png
 *
 * Only a screen that can be read is taken: a virtual screen, a display on MIPI-DSI, a display that keeps its picture
 * in memory (CO5300, AXS15231B). A display behind a bus is not read by DGX: a program that draws on one takes the
 * snapshot of the virtual screen it composes its frames in. Nothing of the size of the picture is allocated; the
 * pixels are read and sent row by row.
 *
 * The code of this file is in a source of its own: a program that does not call it does not carry it.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "dgx_screen.h"
#include "esp_err.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Where the bytes of a snapshot go: n bytes at a time, in order; false stops the snapshot.
 */
typedef bool (*dgx_snapshot_write_t)(void *ctx, const void *bytes, size_t n);

/**
 * @brief A snapshot of a rectangle of a screen as a .dgxi file, handed to `write` a piece at a time.
 *
 * The file holds one picture of pixels of the depth of the screen, the rectangle cut by the screen, laid out as a
 * virtual screen keeps its pixels, its text naming the snapshot.
 *
 * @param scr   A screen that can be read (dgx_screen_is_readable()).
 * @param x,y   The top left corner of the rectangle.
 * @param w,h   Its size; 0 or less in either takes the whole screen.
 * @param name  The name of the snapshot, put into the text of the file; NULL for none.
 * @param write Where the bytes go.
 * @param ctx   Given to write.
 * @return ESP_OK; ESP_ERR_INVALID_ARG for a NULL screen or writer, or a rectangle outside the screen;
 *         ESP_ERR_NOT_SUPPORTED for a screen that cannot be read or of a depth a file does not hold;
 *         ESP_FAIL when write stopped it.
 */
esp_err_t dgx_screen_snapshot(dgx_screen_t *scr, int x, int y, int w, int h, const char *name, dgx_snapshot_write_t write, void *ctx);

/**
 * @brief A snapshot written to the console, or to another stream, for tools/dgx_snapshot.py.
 *
 * The lines are
 *
 *     DGXSNAP BEGIN <name>
 *     <the bytes of the file in base64, 76 characters a line>
 *     DGXSNAP END <name> <its length> <its CRC-32, 8 hexadecimal digits>
 *
 * and lines of other text in between, of the log of another task, are skipped by the tool; the length and the CRC
 * tell a snapshot spoiled on its way.
 *
 * @param scr     A screen that can be read.
 * @param x,y,w,h The rectangle, as for dgx_screen_snapshot().
 * @param name    The name, of letters, digits, '-', '_' and '.' only: the tool names the files by it.
 * @param out     The stream; NULL for stdout, the console.
 * @return As dgx_screen_snapshot(); ESP_ERR_INVALID_ARG for a name of other characters or none.
 */
esp_err_t dgx_screen_snapshot_print(dgx_screen_t *scr, int x, int y, int w, int h, const char *name, FILE *out);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif

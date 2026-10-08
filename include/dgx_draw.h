#pragma once
/*
 * dgx_draw.h
 *
 *  Created on: Apr 2, 2023
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */


#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Draws a filled rectangle on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the top-left corner of the rectangle.
 * @param y The y coordinate of the top-left corner of the rectangle.
 * @param w The width of the rectangle.
 * @param h The height of the rectangle.
 * @param color The color of the rectangle.
*/
void dgx_fill_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color);

/**
 * @brief Draws a circle outline on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the center of the circle.
 * @param y The y coordinate of the center of the circle.
 * @param r The radius of the circle.
 * @param color The color of the circle.
 */
void dgx_draw_circle(dgx_screen_t *scr, int x, int y, int r, uint32_t color);

/**
 * @brief Draws a line on the screen.
 *
 * @param scr The screen to draw on.
 * @param x1 The x coordinate of the first point of the line.
 * @param y1 The y coordinate of the first point of the line.
 * @param x2 The x coordinate of the second point of the line.
 * @param y2 The y coordinate of the second point of the line.
 * @param color The color of the line.
 */
void dgx_draw_line(dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color);

/**
 * @brief Draws a filled circle on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the center of the circle.
 * @param y The y coordinate of the center of the circle.
 * @param r The radius of the circle.
 * @param color The color of the circle.
 */
void dgx_solid_circle(dgx_screen_t *scr, int x, int y, int r, uint32_t color);

/**
 * @brief Set a pixel on the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the pixel.
 * @param y The y coordinate of the pixel.
 * @param color The color of the pixel.
*/
void dgx_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color);

/**
 * @brief Get a pixel from the screen.
 *
 * @param scr The screen to draw on.
 * @param x The x coordinate of the pixel.
 * @param y The y coordinate of the pixel.
 * @return The color of the pixel.
 * @note Works only on virtual screens and on physical screens that draw into a
 *       virtual back screen first (ST7920, SSD1306, ST7565R). Direct physical
 *       panels (ILI9341, ST7789, ...) do not read back and return 0.
*/
uint32_t dgx_get_pixel(dgx_screen_t *scr, int x, int y);

/**
 * @brief Draws a filled triangle on the screen.
 *
 * @param scr The screen to draw on.
 * @param x0,y0 First vertex.
 * @param x1,y1 Second vertex.
 * @param x2,y2 Third vertex.
 * @param color Fill color.
 */
void dgx_draw_triangle_solid(dgx_screen_t *scr, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);

/**
 * @brief Draws a filled simple quadrilateral on the screen.
 *
 * Vertices must be supplied in polygon order, either clockwise or
 * counterclockwise. Convex and concave simple quadrilaterals are supported.
 * Self-intersecting input is not supported.
 *
 * @param scr The screen to draw on.
 * @param x0,y0 First vertex.
 * @param x1,y1 Second vertex.
 * @param x2,y2 Third vertex.
 * @param x3,y3 Fourth vertex.
 * @param color Fill color.
 */
void dgx_draw_polygon4_solid(dgx_screen_t *scr, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3,
                             uint32_t color);

/**
 * @brief Draws a thick line on the screen as a parallelogram of the given width,
 * with rounded/triangular caps at the endpoints.
 *
 * @param scr The screen to draw on.
 * @param x1,y1 Line start.
 * @param x2,y2 Line end.
 * @param width Line thickness in pixels (>= 1).
 * @param color Line color.
 */
void dgx_draw_line_thick(dgx_screen_t *scr, int x1, int y1, int x2, int y2, int width, uint32_t color);

/**
 * @brief Draws a quadratic Bezier curve as a thick line with round ends.
 *
 * The curve is split into straight pieces drawn one after another. A piece
 * has a flat start and a round end, which closes the joint with the next one.
 *
 * @param scr    The screen to draw on.
 * @param points Start point, control point, end point.
 * @param pieces How many straight pieces to split the curve into, equal in
 *               its parameter. 0: as many as it takes for the polyline to stay
 *               within half a pixel of the curve, found from the points with
 *               a margin. A curve of a handwritten font comes with its own
 *               number, see dgx_hw_font.h.
 * @param width  Line thickness in pixels (>= 1).
 * @param color  Line color.
 */
void dgx_draw_bezier3(dgx_screen_t *scr, const dgx_point_2d_t points[3], int pieces, int width, uint32_t color);

/**
 * @brief Draws a cubic Bezier curve as a thick line with round ends.
 *
 * @param scr    The screen to draw on.
 * @param points Start point, two control points, end point.
 * @param pieces How many straight pieces to split the curve into, see
 *               dgx_draw_bezier3(); 0 estimates it from the points.
 * @param width  Line thickness in pixels (>= 1).
 * @param color  Line color.
 */
void dgx_draw_bezier4(dgx_screen_t *scr, const dgx_point_2d_t points[4], int pieces, int width, uint32_t color);

/**
 * @brief A Bezier curve being drawn part by part.
 *
 * Filled by dgx_bezier3_begin() or dgx_bezier4_begin() and drawn on by
 * dgx_bezier_draw_to(). It owns no memory and may be dropped at any moment.
 * Only @c pieces and @c done are of use to the caller, and only for reading.
 */
typedef struct {
    int16_t        pieces;         /**< Straight pieces in the whole curve. */
    int16_t        done;           /**< Pieces drawn whole so far. */
    dgx_screen_t  *scr;
    uint32_t       color;
    int            width;
    float          kx[4], ky[4];   /* a point is ((k[0] * t + k[1]) * t + k[2]) * t + k[3] */
    dgx_point_2d_t end;
    float          length;         /* of all the pieces, pixels; < 0: not counted yet */
    float          drawn;          /* the way the pen has gone */
    int            x, y;           /* the start of the piece the pen is on */
    int            bx, by;         /* its end */
    float          piece_length;
    float          piece_drawn;    /* how far along it the pen is */
    bool           in_piece;       /* the four above are set */
    int            ex, ey;         /* where the axis runs from the points, in half pixels */
    bool           started;
    int            rx, ry, rw, rh; /* equal rows not sent yet, rh == 0: none */
} dgx_bezier_t;

/**
 * @brief Prepares a quadratic Bezier curve for drawing part by part; draws nothing.
 *
 * The parameters are those of dgx_draw_bezier3().
 */
void dgx_bezier3_begin(dgx_bezier_t *b, dgx_screen_t *scr, const dgx_point_2d_t points[3], int pieces, int width, uint32_t color);

/**
 * @brief Prepares a cubic Bezier curve for drawing part by part; draws nothing.
 *
 * The parameters are those of dgx_draw_bezier4().
 */
void dgx_bezier4_begin(dgx_bezier_t *b, dgx_screen_t *scr, const dgx_point_2d_t points[4], int pieces, int width, uint32_t color);

/**
 * @brief The length of a prepared curve as it is drawn, in pixels.
 *
 * It is the length of its straight pieces together, the way the pen goes; a
 * curve of several pieces of the same pace takes the time by it.
 *
 * @param b The curve.
 * @return The length; 0 for a curve that is a dot.
 */
float dgx_bezier_length(dgx_bezier_t *b);

/**
 * @brief Draws a prepared curve on, up to the given share of it.
 *
 * @p t is how complete the curve must be, as in morphing: 0 is nothing, 1 the
 * whole curve, counted by the way of the pen, so a pen led by an even t moves
 * at an even speed however the curve is split into pieces. Only what is not
 * drawn yet is drawn: the pieces the pen has passed whole, and the piece it is
 * on as far as it has got. What a call draws reaches the screen before it
 * returns, and the line ends round where the pen stopped. A @p t not greater
 * than the one before draws nothing; a curve is not drawn back.
 *
 * Drawn to 1 in any number of calls, a curve is the same picture as
 * dgx_draw_bezier3() or dgx_draw_bezier4() give.
 *
 * @param b The curve.
 * @param t How complete it must be, 0 .. 1.
 * @return true when the curve is finished.
 */
bool dgx_bezier_draw_to(dgx_bezier_t *b, float t);

/**
 * @brief Draws a dotted/dashed line using a rotating bit pattern. Each bit of @p mask
 * (taken LSB-first as the first pixel) selects either @p color (1) or @p bg (0)
 * for one pixel along the line. After @p mask_bits pixels the pattern repeats.
 *
 * @param scr        The screen to draw on.
 * @param x1,y1      Line start.
 * @param x2,y2      Line end.
 * @param color      Foreground color (mask bit = 1).
 * @param bg         Background color (mask bit = 0).
 * @param mask       Bit pattern (low @p mask_bits bits used).
 * @param mask_bits  Pattern length in bits (1..32).
 * @return           Mask rotated by the number of pixels drawn, suitable to
 *                   continue the same pattern on a follow-up call.
 */
uint32_t dgx_draw_line_mask(dgx_screen_t *scr, int x1, int y1, int x2, int y2, uint32_t color, uint32_t bg, uint32_t mask,
                            uint8_t mask_bits);

/**
 * @brief Stretches a rectangular region of a texture onto a convex quadrilateral.
 *
 * @p quad lists the screen positions of the region's corners in the order
 * top-left, top-right, bottom-right, bottom-left; the quad may be mirrored or
 * rotated. Texture coordinates are interpolated linearly along the edges and
 * along every scanline (affine mapping, no perspective correction), with
 * nearest-texel sampling. Parts outside the screen are clipped.
 *
 * The texture must be readable with get_pixel (a virtual screen or a panel
 * with a virtual back screen) and have the same color depth as @p scr. A
 * 16-bit virtual screen texture is read directly from its buffer.
 *
 * @param scr     The screen to draw on.
 * @param quad    Four vertices of a convex quadrilateral.
 * @param texture Source screen.
 * @param tx,ty   Top-left corner of the texture region.
 * @param tw,th   Size of the texture region; it must lie inside the texture.
 */
void dgx_draw_texture_quad(dgx_screen_t *scr, const dgx_point_2d_t quad[4], dgx_screen_t *texture, int tx, int ty, int tw, int th);

/**
 * @brief Scales a rectangular region of a texture into a rectangle on the screen.
 *
 * Same as dgx_draw_texture_quad() with an axis-aligned quad.
 *
 * @param scr     The screen to draw on.
 * @param x,y     Top-left corner of the destination rectangle.
 * @param w,h     Size of the destination rectangle.
 * @param texture Source screen.
 * @param tx,ty   Top-left corner of the texture region.
 * @param tw,th   Size of the texture region.
 */
void dgx_draw_texture_rect(dgx_screen_t *scr, int x, int y, int w, int h, dgx_screen_t *texture, int tx, int ty, int tw, int th);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif


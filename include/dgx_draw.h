#pragma once
/*
 * dgx_draw.h
 *
 *  Created on: Apr 2, 2023
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */


#include "esp_err.h"

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
 * @brief Draws the outline of an ellipse, a pixel wide.
 *
 * The axes of the ellipse lie along the rows and the columns of the screen.
 * A pixel is taken by the rule of the midpoint algorithm, by which
 * dgx_draw_circle() goes: an ellipse of equal radii is that circle, pixel
 * for pixel. No pixel is further than half a pixel from the curve, along its
 * row or along its column, and a thin ellipse keeps its ends, which the walk
 * of the textbooks spoils. An ellipse with a radius of 0 is a line.
 *
 * @param scr   The screen to draw on.
 * @param x     The x coordinate of the center.
 * @param y     The y coordinate of the center.
 * @param rx    The radius along x: the ellipse is 2 * rx + 1 pixels wide.
 * @param ry    The radius along y: the ellipse is 2 * ry + 1 pixels high.
 * @param color The color of the outline.
 */
void dgx_draw_ellipse(dgx_screen_t *scr, int x, int y, int rx, int ry, uint32_t color);

/**
 * @brief Draws an arc of an ellipse by a pen of a width; with equal radii, an arc of a circle.
 *
 * An angle is counted in degrees from the ray that goes from the center to
 * the right, against the clock as the screen is seen: 90 is up. It is the
 * angle of a ray on the screen, whatever the radii are. The arc is what of
 * the ellipse lies between the rays of @p from and @p to, which of the two
 * is the larger makes no difference. Angles are not brought into a turn: from
 * 350 to 370 is twenty degrees across the ray that goes right, and 360
 * degrees or more between the two is the whole ellipse. With no angle between
 * them nothing is drawn.
 *
 * A hard pen of 1 draws the pixels of the outline of dgx_draw_ellipse() that
 * lie between the rays, a pixel on a ray counted in.
 *
 * Any other pen is the pen of the curves, dgx_draw_bezier4(), led along the
 * arc against the clock: the line is @p width wide with the arc in its
 * middle and has round ends, and with DGX_PEN_SOFT added to @p width its
 * edge is soft where the screen can hold it, see there. What is said there
 * of a pen of an even width and of the ends of a curve, which are whole
 * pixels, holds here. The straight pieces the pen goes by are within half a
 * pixel of the arc for a hard line and within an eighth of a pixel for a
 * soft one.
 *
 * @param scr   The screen to draw on.
 * @param x     The x coordinate of the center.
 * @param y     The y coordinate of the center.
 * @param rx    The radius along x.
 * @param ry    The radius along y.
 * @param from  The angle the arc begins at, degrees.
 * @param to    The angle the arc ends at, degrees.
 * @param width Line thickness in pixels (>= 1), with DGX_PEN_SOFT for a soft
 *              edge.
 * @param color Line color; the amount of ink for a soft pen.
 */
void dgx_draw_arc(dgx_screen_t *scr, int x, int y, int rx, int ry, float from, float to, int width, uint32_t color);

/**
 * @brief Draws a filled sector of an ellipse; with equal radii, of a circle.
 *
 * The sector is what of the ellipse, its outline and all within it, lies
 * between the rays of @p from and @p to; the angles are those of
 * dgx_draw_arc(). A pixel on a ray is of the sector, so two sectors with a
 * common ray both have its pixels. 360 degrees or more between the two angles
 * is the whole ellipse filled, and with equal radii that is the picture of
 * dgx_solid_circle(). With no angle between them nothing is drawn.
 *
 * @param scr   The screen to draw on.
 * @param x     The x coordinate of the center.
 * @param y     The y coordinate of the center.
 * @param rx    The radius along x.
 * @param ry    The radius along y.
 * @param from  The angle the sector begins at, degrees.
 * @param to    The angle the sector ends at, degrees.
 * @param color Fill color.
 */
void dgx_solid_sector(dgx_screen_t *scr, int x, int y, int rx, int ry, float from, float to, uint32_t color);

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
 * @note Works only where pixels are memory of the chip: on virtual screens,
 *       on physical screens that draw into a virtual back screen first
 *       (ST7920, SSD1306, ST7565R) and on a DSI display. Direct physical
 *       panels (ILI9341, ST7789, ...) do not read back and return 0; see
 *       dgx_screen_is_readable().
*/
uint32_t dgx_get_pixel(dgx_screen_t *scr, int x, int y);

/**
 * @brief Tells whether a screen gives back the pixels drawn on it.
 *
 * True for a screen that keeps its pixels in memory: a virtual screen, a
 * display that is drawn into a virtual screen first (ST7920, SSD1306,
 * ST7565R), a DSI display with its frame buffers. False for a display that
 * has its memory in the panel behind a bus (ILI9341, ST7789, ...):
 * dgx_get_pixel() returns 0 there.
 *
 * @param scr The screen.
 * @return true when dgx_get_pixel() reads what was drawn.
 */
bool dgx_screen_is_readable(const dgx_screen_t *scr);

/**
 * @brief Moves what a rectangle of the screen shows, within the rectangle, any way.
 *
 * The picture in the rectangle goes @p dx pixels to the right and @p dy
 * pixels down; negative ones are to the left and up, and the two together
 * are a move aslant. What leaves the rectangle is lost, what is outside it
 * stays as it is, and the strips the picture has left are filled with
 * @p color. A move as large as the rectangle, or larger, only fills it. The
 * rectangle is cut by the screen first.
 *
 * The picture is read from the screen, so the screen has to give its pixels
 * back, dgx_screen_is_readable(). A virtual screen of whole bytes a pixel, 8
 * bits and more but for 12, is moved in its memory a row at a time, and so is
 * a DSI display in its frame buffer, by its bus. Any other screen that is
 * read is moved pixel by pixel, each read and written by the screen.
 *
 * A display that keeps its pixels in the panel behind a bus is not read. A
 * panel that scrolls itself moves the picture then, and no pixel of it is
 * sent: an ILI9341 does, along the long side of its panel, which is y of a
 * screen upright and x of one that lies. It is a move the panel can make
 * when it goes along that side only and the rectangle takes the screen from
 * edge to edge across it; and the panel has one such area at a time, so a
 * rectangle that has been moved has to be filled, by a move as large as it,
 * before another is moved. Everything drawn after is still drawn where it is
 * seen. Any other move on such a display draws nothing and returns
 * ESP_ERR_NOT_SUPPORTED, but for a move that only fills. A virtual screen
 * always works then: draw the text on a virtual screen, scroll that, and send
 * it to the display, in any orientation.
 *
 * On such a panel the strip that leaves is filled with @p color before the
 * move, so that it comes in at the other edge as the color already. That
 * fill is the cost of the move: about 100 us a column of 240 rows on an
 * ILI9341 at 40 MHz, against about 150 us for the commands of the panel. A
 * caller that draws the strip that comes in anyway uses
 * dgx_scroll_no_fill(), and the move is the commands alone.
 *
 * @param scr   The screen.
 * @param x     The x coordinate of the top-left corner of the rectangle.
 * @param y     The y coordinate of the top-left corner of the rectangle.
 * @param w     The width of the rectangle.
 * @param h     The height of the rectangle.
 * @param dx    How far the picture goes to the right; negative: to the left.
 * @param dy    How far the picture goes down; negative: up.
 * @param color The color of what the picture has left.
 * @return ESP_OK when the picture is moved, or when there was nothing to move;
 *         ESP_ERR_NOT_SUPPORTED when the picture had to be moved and the
 *         screen can neither be read nor move it itself. Nothing is drawn
 *         then.
 */
esp_err_t dgx_scroll(dgx_screen_t *scr, int x, int y, int w, int h, int dx, int dy, uint32_t color);

/**
 * @brief dgx_scroll() without the fill: the strips the picture has left are not touched, the caller draws them.
 *
 * The same moves, the same screens and the same return values as
 * dgx_scroll(). On a screen that is read the strips keep the old picture. On
 * a panel that scrolls itself the strip that comes in holds what has just
 * left at the other edge, for the memory of the panel wraps round; the move
 * costs the commands of the panel alone. A move as large as the rectangle
 * draws nothing; on such a panel it still lets the area go, and the
 * rectangle shows the rows of the panel in their own order until it is drawn
 * over.
 *
 * @param scr   The screen.
 * @param x     The x coordinate of the top-left corner of the rectangle.
 * @param y     The y coordinate of the top-left corner of the rectangle.
 * @param w     The width of the rectangle.
 * @param h     The height of the rectangle.
 * @param dx    How far the picture goes to the right; negative: to the left.
 * @param dy    How far the picture goes down; negative: up.
 * @return As dgx_scroll().
 */
esp_err_t dgx_scroll_no_fill(dgx_screen_t *scr, int x, int y, int w, int h, int dx, int dy);

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
 * An even width has no middle pixel: the spare one goes to the right of the
 * way from (x1, y1) to (x2, y2), so the same line drawn the other way lies a
 * pixel aside. Draw the lines of one figure in one direction. An odd width
 * gives the same pixels either way.
 *
 * @param scr The screen to draw on.
 * @param x1,y1 Line start.
 * @param x2,y2 Line end.
 * @param width Line thickness in pixels (>= 1).
 * @param color Line color.
 */
void dgx_draw_line_thick(dgx_screen_t *scr, int x1, int y1, int x2, int y2, int width, uint32_t color);

/**
 * @brief Added to the width of a pen, it asks for a soft edge: `DGX_PEN_SOFT | 2`.
 *
 * It is for whatever draws a curve by a pen of a width: dgx_draw_bezier3(),
 * dgx_draw_bezier4(), dgx_bezier3_begin(), dgx_bezier4_begin() and what is
 * built on them, an arc of dgx_draw_arc(), a handwritten text, its writing
 * and its morphs.
 *
 * A soft pen leaves ink by the share of a pixel it covers. The color of the
 * curve is then an amount of ink, that of a pixel the pen covers whole, and a
 * pixel on the edge of the line gets a share of it, half a pixel either way
 * from where a hard edge would be. Where lines meet or cross, a pixel keeps
 * the larger amount: nothing gets darker there, and a line drawn twice is
 * the line drawn once. A soft pen with color 0 takes the ink off every pixel
 * it touches at all, so a line written by it is taken off whole by the same
 * pen going the same way; a hard pen with color 0 takes off the pixels of a
 * hard line and leaves the soft edge of a soft one. Drawn in any number of
 * steps, a curve is byte for byte the curve drawn at once.
 *
 * A soft line also runs where the curve does: the ends of its straight pieces
 * are not brought to whole pixels, only the ends of the curve are whole. A
 * pen of an even width runs half a pixel to the right and down of them,
 * between pixels. Where the line turns little at a joint it has a corner and
 * not a round joint; the difference is under a sixteenth of the ink of a
 * pixel.
 *
 * All this takes a pixel that is an amount of ink: a virtual screen of 8
 * bits a pixel, dgx_vscreen_init(). What an amount looks like on a display is
 * the business of the table such a screen is sent through,
 * dgx_vscreen8_to_screen16(), which has to run from the color of the paper
 * at 0 to the color of the ink. A pixel of colors has nothing to take the
 * larger of, and a display behind a bus cannot be read at all: on every
 * other screen a pen asked to be soft draws as a hard one of the same width.
 */
#define DGX_PEN_SOFT 0x40000000

/**
 * @brief Draws a quadratic Bezier curve as a thick line with round ends.
 *
 * The curve is split into straight pieces drawn one after another. A piece
 * has a flat start and a round end, which closes the joint with the next one.
 *
 * The pen is given by @p width and @p color. Its edge is hard: a pixel is
 * the line or is not. With DGX_PEN_SOFT added to @p width the edge is soft,
 * see there.
 *
 * @param scr    The screen to draw on.
 * @param points Start point, control point, end point.
 * @param pieces How many straight pieces to split the curve into, equal in
 *               its parameter. 0: as many as it takes for the polyline to stay
 *               within half a pixel of the curve, found from the points with
 *               a margin. A curve of a handwritten font comes with its own
 *               number, see dgx_hw_font.h.
 * @param width  Line thickness in pixels (>= 1), with DGX_PEN_SOFT for a soft
 *               edge.
 * @param color  Line color; the amount of ink for a soft pen.
 */
void dgx_draw_bezier3(dgx_screen_t *scr, const dgx_point_2d_t points[3], int pieces, int width, uint32_t color);

/**
 * @brief Draws a cubic Bezier curve as a thick line with round ends.
 *
 * @param scr    The screen to draw on.
 * @param points Start point, two control points, end point.
 * @param pieces How many straight pieces to split the curve into, see
 *               dgx_draw_bezier3(); 0 estimates it from the points.
 * @param width  Line thickness in pixels (>= 1), with DGX_PEN_SOFT for a soft
 *               edge.
 * @param color  Line color; the amount of ink for a soft pen.
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
    float          piece_length;   /* of the piece the pen is on */
    float          piece_drawn;    /* how far along it the pen is */
    bool           in_piece;       /* the piece the pen is on is known */
    bool           started;
    bool           soft;           /* the edge of the line is soft: DGX_PEN_SOFT on a screen of a byte a pixel */
    uint8_t        direct;         /* bytes a pixel of a virtual screen whose memory the pen writes itself; 0: it asks the screen */
    union {
        struct { /* a line with a hard edge: its points are whole pixels */
            int x, y;           /* the start of the piece the pen is on */
            int bx, by;         /* its end */
            int ex, ey;         /* where the axis runs from the points, in half pixels */
            int rx, ry, rw, rh; /* equal rows not sent yet, rh == 0: none */
        };
        struct { /* a line with a soft edge: its points are not brought to whole pixels */
            float fx, fy;          /* the start of the piece the pen is on */
            float fbx, fby;        /* its end */
            float ux, uy;          /* the way the piece goes, a vector of length 1 */
            float nx, ny, next[2]; /* the end of the piece after it and the way that one goes, */
            float next_length;     /* its length; 0: it is not known, or there is none */
            float back, fore;      /* how much longer the piece is made at its start and at its end; fore < 0: the end is round */
            float little;          /* the least cosine of a turn that needs no round joint */
        };
    };
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


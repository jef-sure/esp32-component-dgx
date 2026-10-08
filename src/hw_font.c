#include <math.h>

#include "dgx_hw_font.h"

float dgx_hw_element_effort(const dgx_hw_element_t *element, const dgx_hw_pace_t *pace)
{
    float effort = element->length * 0.01f + pace->piece * element->pieces;
    return effort > pace->least ? effort : pace->least;
}

void dgx_hw_line_begin(dgx_hw_line_t *line, dgx_font_t *font)
{
    line->font = font;
    line->pen = 0;
    line->up = 0;
    line->has_up = false;
    line->first = true;
}

const glyph_t *dgx_hw_line_place(dgx_hw_line_t *line, uint32_t code_point, int *shift)
{
    /* a font of another kind has nothing to place */
    if (!line->font || line->font->f_type != DGX_FONT_HW || !line->font->hw) return 0;
    const dgx_hw_font_t *hw = line->font->hw;
    int16_t              advance;
    const glyph_t       *g = dgx_font_find_glyph(code_point, line->font, &advance);
    bool                 first = line->first;
    line->first = false;
    if (!g) {
        /* a space has its own width, a symbol which is not in the font takes the whole active area */
        line->pen += code_point == ' ' ? hw->space_width : hw->symbol_size_x;
        line->has_up = false;
        return 0;
    }
    const dgx_hw_symbol_t *s = g->hw;
    /* the left edge of the drawing; the first symbol of a line is written whole from its start */
    int32_t left = first ? line->pen : line->pen + g->xOffset;
    if (s->up_left != DGX_HW_NONE) {
        /* two tall symbols are also kept apart by their parts above the lowercase band */
        int32_t up_left = left + s->up_left - s->left;
        if (line->has_up && up_left < line->up + s->space_before) left += line->up + s->space_before - up_left;
        int32_t up = left + s->up_right - s->left + 1 + s->space_after;
        if (!line->has_up) line->up = 0;
        if (up > line->up) line->up = up;
        line->has_up = true;
    }
    *shift = left - s->left - hw->symbol_offset_x;
    line->pen = left - g->xOffset + g->xAdvance;
    return g;
}

bool dgx_hw_text_box(dgx_font_t *font, const char *text, int *left, int *top, int *right, int *bottom)
{
    dgx_hw_line_t line;
    size_t        idx = 0;
    int           line_y = 0, l = 0, t = 0, r = 0, b = 0;
    bool          any = false;
    if (!font || font->f_type != DGX_FONT_HW || !font->hw) return false;
    dgx_hw_line_begin(&line, font);
    while (text[idx]) {
        uint32_t cp = decodeUTF8next(text, &idx);
        int      shift;
        if (cp == '\n') {
            dgx_hw_line_begin(&line, font);
            line_y += font->hw->symbol_size_y;
            continue;
        }
        const glyph_t *g = dgx_hw_line_place(&line, cp, &shift);
        if (!g) continue;
        int gl = shift + font->hw->symbol_offset_x + g->hw->left, gt = line_y + g->hw->top;
        int gr = shift + font->hw->symbol_offset_x + g->hw->right, gb = line_y + g->hw->bottom;
        if (!any || gl < l) l = gl;
        if (!any || gt < t) t = gt;
        if (!any || gr > r) r = gr;
        if (!any || gb > b) b = gb;
        any = true;
    }
    if (!any) return false;
    *left = l, *top = t, *right = r, *bottom = b;
    return true;
}

dgx_hw_pace_t dgx_hw_pace(dgx_font_t *font, float k)
{
    dgx_hw_pace_t  pace = {.piece = k, .least = 0};
    int16_t        advance;
    if (!font || font->f_type != DGX_FONT_HW || !font->hw) return pace;
    const glyph_t *hyphen = dgx_font_find_glyph('-', font, &advance);
    if (hyphen) {
        for (int i = 0; i < hyphen->hw->main_segments + hyphen->hw->post_segments; ++i) {
            int at = i < hyphen->hw->main_segments ? hyphen->hw->begin_connection : hyphen->hw->begin_connection + hyphen->hw->end_connection;
            pace.least += dgx_hw_element_effort(&hyphen->elements[at + i], &pace);
        }
    }
    if (!(pace.least > 0)) pace.least = font->hw->x_height * 2 / 3.0f;
    return pace;
}

float dgx_hw_stroke_effort(const dgx_hw_stroke_t *stroke, const dgx_hw_pace_t *pace)
{
    float effort = stroke->length + pace->piece * stroke->pieces;
    return effort > pace->least ? effort : pace->least;
}

/* what a symbol is being written of */
enum { DGX_HW_WRITE_JOINT, DGX_HW_WRITE_BEGIN, DGX_HW_WRITE_MAIN, DGX_HW_WRITE_END };

void dgx_hw_writer_begin(dgx_hw_writer_t *w, dgx_font_t *font, const char *text, bool joined)
{
    w->font = font;
    w->text = text;
    w->idx = 0;
    w->number = 0;
    w->joined = joined;
    dgx_hw_line_begin(&w->line, font);
    w->line_y = 0;
    w->g = 0;
    w->prev = 0;
    w->has_joint = false;
    w->run = false;
    w->run_idx = w->run_end = 0;
    w->posting = false;
    w->pen_set = false;
}

/* the points of an element in the text */
static void dgx_hw_in_text(const dgx_hw_writer_t *w, const dgx_hw_element_t *e, int shift, float *x, float *y)
{
    const dgx_hw_font_t *hw = w->font->hw;
    for (int i = 0; i < e->type; ++i) {
        x[i] = e->points[i].x + shift;
        y[i] = e->points[i].y - hw->symbol_offset_y - hw->base_line + w->line_y;
    }
}

/* any element as a cubic curve drawing the same */
static void dgx_hw_cubic(int type, float *x, float *y)
{
    if (type == DGX_HW_CURVE3P) {
        x[3] = x[2], y[3] = y[2];
        x[2] = x[3] + (x[1] - x[3]) * 2 / 3, y[2] = y[3] + (y[1] - y[3]) * 2 / 3;
        x[1] = x[0] + (x[1] - x[0]) * 2 / 3, y[1] = y[0] + (y[1] - y[0]) * 2 / 3;
    } else if (type == DGX_HW_LINE) {
        x[3] = x[1], y[3] = y[1];
        x[1] = x[0] + (x[3] - x[0]) / 3, y[1] = y[0] + (y[3] - y[0]) / 3;
        x[2] = x[0] + (x[3] - x[0]) * 2 / 3, y[2] = y[0] + (y[3] - y[0]) * 2 / 3;
    } else if (type == DGX_HW_DOT) {
        x[1] = x[2] = x[3] = x[0], y[1] = y[2] = y[3] = y[0];
    }
}

/* where the pen has come to; whether it had to be lifted to begin this stroke */
static bool dgx_hw_stroke_done(dgx_hw_writer_t *w, dgx_hw_stroke_t *s)
{
    s->lift = w->pen_set && (s->x[0] != w->pen_x || s->y[0] != w->pen_y);
    w->pen_set = true;
    w->pen_x = s->x[s->type - 1];
    w->pen_y = s->y[s->type - 1];
    return true;
}

static bool dgx_hw_stroke_of(dgx_hw_writer_t *w, const dgx_hw_element_t *e, int shift, uint16_t symbol, dgx_hw_stroke_t *s)
{
    dgx_hw_in_text(w, e, shift, s->x, s->y);
    s->symbol = symbol;
    s->type = e->type;
    s->pieces = e->pieces;
    s->length = e->length * 0.01f;
    return dgx_hw_stroke_done(w, s);
}

/*
 * One curve joining two symbols. a is the last element of the end connection
 * of the first one, b the first element of the begin connection of the second
 * one, both as cubic curves in the text. The curve they would be joined with
 * ideally has 5 points: two of a, two of b and the middle of the two between.
 * The curve of 4 points closest to that one is taken: its ends are a[0] and
 * b[3], its second point lies on the line a[0] - a[1] and the third one on
 * b[3] - b[2], so the pen leaves the first symbol and comes to the second one
 * as their connections are drawn; how far they are on those lines is found by
 * least squares in 7 points.
 */
static void dgx_hw_joint(const float *ax, const float *ay, const float *bx, const float *by, dgx_hw_stroke_t *s)
{
    float qx[5] = {ax[0], ax[1], (ax[2] + bx[1]) / 2, bx[2], bx[3]};
    float qy[5] = {ay[0], ay[1], (ay[2] + by[1]) / 2, by[2], by[3]};
    float ux = ax[1] - ax[0], uy = ay[1] - ay[0];
    float vx = bx[2] - bx[3], vy = by[2] - by[3];
    /* curve(t) = a[0] * (B0 + B1) + b[3] * (B2 + B3) + ka * u * B1 + kb * v * B2 */
    float suu = 0, suv = 0, svv = 0, sur = 0, svr = 0;
    for (int i = 1; i < 8; ++i) {
        float t = i / 8.0f, m = 1 - t;
        float b1 = 3 * m * m * t, b2 = 3 * m * t * t;
        float k[5] = {m * m * m * m, 4 * m * m * m * t, 6 * m * m * t * t, 4 * m * t * t * t, t * t * t * t};
        float rx = -(m * m * m + b1) * ax[0] - (b2 + t * t * t) * bx[3];
        float ry = -(m * m * m + b1) * ay[0] - (b2 + t * t * t) * by[3];
        for (int n = 0; n < 5; ++n) rx += k[n] * qx[n], ry += k[n] * qy[n];
        suu += b1 * b1 * (ux * ux + uy * uy);
        svv += b2 * b2 * (vx * vx + vy * vy);
        suv += b1 * b2 * (ux * vx + uy * vy);
        sur += b1 * (ux * rx + uy * ry);
        svr += b2 * (vx * rx + vy * ry);
    }
    /* the pen never goes backwards: the points are kept not closer than a quarter of the way to a[1] and b[2] */
    const float least = 0.25f;
    float       ka = least, kb = least;
    float       det = suu * svv - suv * suv;
    if (det > 0.000001f) {
        ka = (sur * svv - svr * suv) / det;
        kb = (suu * svr - suv * sur) / det;
    }
    if (ka < least || kb < least) {
        /* one of them is on its limit, the other one is found alone */
        float ka_alone = suu > 0 ? (sur - least * suv) / suu : least;
        float kb_alone = svv > 0 ? (svr - least * suv) / svv : least;
        if (ka < least && kb < least) {
            ka = kb = least;
        } else if (ka < least) {
            ka = least;
            kb = kb_alone > least ? kb_alone : least;
        } else {
            kb = least;
            ka = ka_alone > least ? ka_alone : least;
        }
    }
    s->type = DGX_HW_CURVE;
    s->x[0] = ax[0], s->y[0] = ay[0];
    s->x[1] = ax[0] + ka * ux, s->y[1] = ay[0] + ka * uy;
    s->x[2] = bx[3] + kb * vx, s->y[2] = by[3] + kb * vy;
    s->x[3] = bx[3], s->y[3] = by[3];
    /*
     * The font has no pieces and length for it. As many pieces as keep the
     * polyline within half a cell: a chord over 1/n of the parameter is not
     * further from the curve than a eighth of its second derivative, the
     * largest at one of the ends, over n squared. The length is theirs.
     */
    float k1x = 3 * (s->x[0] - 2 * s->x[1] + s->x[2]), k1y = 3 * (s->y[0] - 2 * s->y[1] + s->y[2]);
    float k0x = s->x[3] - 3 * s->x[2] + 3 * s->x[1] - s->x[0], k0y = s->y[3] - 3 * s->y[2] + 3 * s->y[1] - s->y[0];
    float at_start = 2 * hypotf(k1x, k1y), at_end = 2 * hypotf(3 * k0x + k1x, 3 * k0y + k1y);
    int   pieces = (int)ceilf(sqrtf((at_start > at_end ? at_start : at_end) / 4));
    if (pieces < 1) pieces = 1;
    if (pieces > 255) pieces = 255;
    s->pieces = (uint8_t)pieces;
    s->length = 0;
    float px = s->x[0], py = s->y[0];
    for (int i = 1; i <= pieces; ++i) {
        float t = (float)i / pieces, m = 1 - t;
        float x = m * m * m * s->x[0] + 3 * m * m * t * s->x[1] + 3 * m * t * t * s->x[2] + t * t * t * s->x[3];
        float y = m * m * m * s->y[0] + 3 * m * m * t * s->y[1] + 3 * m * t * t * s->y[2] + t * t * t * s->y[3];
        s->length += hypotf(x - px, y - py);
        px = x, py = y;
    }
}

/* the postponed strokes of the run are written from its first symbol on */
static void dgx_hw_post(dgx_hw_writer_t *w)
{
    w->posting = true;
    w->post_idx = w->run_idx;
    w->post_number = w->run_number;
    w->post_line = w->run_line;
    w->post_g = 0;
}

bool dgx_hw_writer_next(dgx_hw_writer_t *w, dgx_hw_stroke_t *stroke)
{
    /* a font of another kind has no strokes */
    if (!w->font || w->font->f_type != DGX_FONT_HW || !w->font->hw) return false;
    for (;;) {
        if (w->posting) {
            if (w->post_g && w->post_k < w->post_g->hw->post_segments) {
                const dgx_hw_symbol_t *s = w->post_g->hw;
                int                    at = s->begin_connection + s->main_segments + s->end_connection + w->post_k++;
                return dgx_hw_stroke_of(w, &w->post_g->elements[at], w->post_shift, w->post_number - 1, stroke);
            }
            if (w->post_idx >= w->run_end) {
                w->posting = false;
                w->run = false;
                continue;
            }
            /* the symbols of the run are placed once more, as they were */
            w->post_g = dgx_hw_line_place(&w->post_line, decodeUTF8next(w->text, &w->post_idx), &w->post_shift);
            ++w->post_number;
            w->post_k = 0;
            continue;
        }
        if (w->g) {
            const dgx_hw_symbol_t  *s = w->g->hw;
            const dgx_hw_element_t *e = w->g->elements;
            if (w->phase == DGX_HW_WRITE_JOINT) {
                /* the pen comes from the symbol before by one curve, then the rest of the begin connection */
                float bx[4], by[4];
                dgx_hw_in_text(w, &e[0], w->shift, bx, by);
                dgx_hw_cubic(e[0].type, bx, by);
                dgx_hw_joint(w->jx, w->jy, bx, by, stroke);
                stroke->symbol = w->g_number;
                w->phase = DGX_HW_WRITE_BEGIN;
                w->k = 1;
                return dgx_hw_stroke_done(w, stroke);
            }
            if (w->phase == DGX_HW_WRITE_BEGIN) {
                if (w->k < s->begin_connection) return dgx_hw_stroke_of(w, &e[w->k++], w->shift, w->g_number, stroke);
                w->phase = DGX_HW_WRITE_MAIN;
                w->k = 0;
            }
            if (w->phase == DGX_HW_WRITE_MAIN) {
                if (w->k < s->main_segments) {
                    return dgx_hw_stroke_of(w, &e[s->begin_connection + w->k++], w->shift, w->g_number, stroke);
                }
                w->phase = DGX_HW_WRITE_END;
                w->k = 0;
            }
            /* the end connection but its last element, which goes into the joint with the next symbol */
            w->has_joint = false;
            if (w->joined_next) {
                const dgx_hw_element_t *end = &e[s->begin_connection + s->main_segments];
                if (w->k + 1 < s->end_connection) return dgx_hw_stroke_of(w, &end[w->k++], w->shift, w->g_number, stroke);
                const dgx_hw_element_t *last = &end[s->end_connection - 1];
                dgx_hw_in_text(w, last, w->shift, w->jx, w->jy);
                dgx_hw_cubic(last->type, w->jx, w->jy);
                w->has_joint = true;
            }
            w->prev = w->g;
            w->g = 0;
        }
        /* the next symbol */
        if (!w->text[w->idx]) {
            if (!w->run) return false;
            dgx_hw_post(w);
            continue;
        }
        size_t         next = w->idx;
        uint32_t       cp = decodeUTF8next(w->text, &next);
        int16_t        advance;
        const glyph_t *g = cp == '\n' ? 0 : dgx_font_find_glyph(cp, w->font, &advance);
        /* two symbols are joined only when the first has an end connection and the second a begin connection */
        bool joined_prev = w->joined && g && w->prev && w->prev->hw->end_connection && g->hw->begin_connection;
        if (!joined_prev && w->run) {
            /* the pen is taken off the paper: first the postponed strokes of what is written */
            dgx_hw_post(w);
            continue;
        }
        if (cp == '\n') {
            w->idx = next;
            ++w->number;
            dgx_hw_line_begin(&w->line, w->font);
            w->line_y += w->font->hw->symbol_size_y;
            w->prev = 0;
            continue;
        }
        if (g && !w->run) {
            w->run = true;
            w->run_idx = w->idx;
            w->run_number = w->number;
            w->run_line = w->line;
        }
        w->idx = next;
        w->g_number = w->number++;
        g = dgx_hw_line_place(&w->line, cp, &w->shift);
        if (!g) {
            w->prev = 0;
            continue;
        }
        w->run_end = w->idx;
        w->joined_next = false;
        if (w->joined && g->hw->end_connection && w->text[w->idx]) {
            size_t         after = w->idx;
            uint32_t       cp_next = decodeUTF8next(w->text, &after);
            const glyph_t *g_next = cp_next == '\n' ? 0 : dgx_font_find_glyph(cp_next, w->font, &advance);
            w->joined_next = g_next && g_next->hw->begin_connection;
        }
        /*
         * A connection which joins nothing is not written. The only exception is a
         * letter after a letter of joined writing which has no end connection: the
         * pen cannot come out of that one, so this one is begun with its whole
         * begin connection.
         */
        bool dead_end = w->joined && w->prev && w->prev->hw->begin_connection && !w->prev->hw->end_connection;
        w->phase = joined_prev ? DGX_HW_WRITE_JOINT : dead_end ? DGX_HW_WRITE_BEGIN : DGX_HW_WRITE_MAIN;
        w->k = 0;
        w->g = g;
    }
}

float dgx_hw_text_effort(dgx_font_t *font, const char *text, bool joined, const dgx_hw_pace_t *pace)
{
    static const dgx_hw_pace_t even = {0, 0};
    dgx_hw_writer_t            w;
    dgx_hw_stroke_t            s;
    float                      effort = 0;
    if (!pace) pace = &even;
    dgx_hw_writer_begin(&w, font, text, joined);
    while (dgx_hw_writer_next(&w, &s)) effort += (s.lift ? pace->least : 0) + dgx_hw_stroke_effort(&s, pace);
    return effort;
}

void dgx_hw_writing_begin(dgx_hw_writing_t *w, dgx_screen_t *scr, int x, int y, dgx_font_t *font, const char *text, float scale,
                          int width, uint32_t color, bool joined, const dgx_hw_pace_t *pace)
{
    dgx_hw_writer_begin(&w->writer, font, text, joined);
    w->scr = scr;
    w->x = x, w->y = y;
    w->scale = scale > 0 ? scale : 0;
    w->width = width;
    w->color = color;
    w->pace = pace ? *pace : (dgx_hw_pace_t){0, 0};
    w->in_stroke = false;
    w->passed = 0;
    w->finished = false;
}

/* the next stroke of the text as a curve on the screen */
static bool dgx_hw_writing_stroke(dgx_hw_writing_t *w)
{
    dgx_hw_stroke_t s;
    dgx_point_2d_t  p[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    if (!dgx_hw_writer_next(&w->writer, &s)) return false;
    for (int i = 0; i < s.type; ++i) {
        p[i].x = dgx_hw_pixel(w->x + s.x[i] * w->scale);
        p[i].y = dgx_hw_pixel(w->y + s.y[i] * w->scale);
    }
    /* any size at all may be asked for: the number is kept within what a curve takes */
    float needed = ceilf(s.pieces * sqrtf(w->scale));
    int   pieces = !(needed >= 1) ? 1 : needed > INT16_MAX ? INT16_MAX : (int)needed;
    if (s.type == DGX_HW_CURVE) {
        dgx_bezier4_begin(&w->curve, w->scr, p, pieces, w->width, w->color);
    } else {
        /* a dot is all its points in one: a disc of the pen; a line is a curve of one piece */
        if (s.type == DGX_HW_DOT) p[1] = p[2] = p[0];
        if (s.type == DGX_HW_LINE) p[2] = p[1];
        dgx_bezier3_begin(&w->curve, w->scr, p, pieces, w->width, w->color);
    }
    w->start = w->passed + (s.lift ? w->pace.least : 0);
    w->effort = dgx_hw_stroke_effort(&s, &w->pace);
    return true;
}

bool dgx_hw_writing_draw_to(dgx_hw_writing_t *w, float effort)
{
    if (w->finished) return true;
    dgx_screen_progress_up(w->scr);
    for (;;) {
        if (!w->in_stroke) {
            if (!dgx_hw_writing_stroke(w)) {
                w->finished = true;
                break;
            }
            w->in_stroke = true;
        }
        if (effort >= w->start + w->effort) {
            dgx_bezier_draw_to(&w->curve, 1);
            w->passed = w->start + w->effort;
            w->in_stroke = false;
            continue;
        }
        if (effort > w->start) dgx_bezier_draw_to(&w->curve, (effort - w->start) / w->effort);
        break;
    }
    dgx_screen_progress_down(w->scr);
    return w->finished;
}

void dgx_hw_draw_text(dgx_screen_t *scr, int x, int y, dgx_font_t *font, const char *text, float scale, int width, uint32_t color,
                      bool joined)
{
    dgx_hw_writing_t w;
    dgx_hw_writing_begin(&w, scr, x, y, font, text, scale, width, color, joined, 0);
    dgx_hw_writing_draw_to(&w, INFINITY);
}

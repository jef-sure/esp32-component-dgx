#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_hw_font.h"

/* the strokes of a text as cubic curves, with the symbols they are of */
typedef struct {
    int             number;
    dgx_hw_curve_t *curves;
    uint16_t       *symbols;
    size_t          length; /* code points in the text */
} dgx_hw_way_t;

static void dgx_hw_way_free(dgx_hw_way_t *way)
{
    free(way->curves);
    free(way->symbols);
    way->curves = 0;
    way->symbols = 0;
}

/* room is how many curves to leave place for */
static bool dgx_hw_way_of(dgx_hw_way_t *way, dgx_font_t *font, const char *text, bool joined, int room)
{
    dgx_hw_writer_t w;
    dgx_hw_stroke_t s;
    way->number = 0;
    dgx_hw_writer_begin(&w, font, text, joined);
    while (dgx_hw_writer_next(&w, &s)) ++way->number;
    way->length = 0;
    for (size_t idx = 0; text[idx]; ++way->length) decodeUTF8next(text, &idx);
    /* symbols are numbered in 16 bits */
    if (way->length > UINT16_MAX) return false;
    if (room < way->number) room = way->number;
    way->curves = malloc(sizeof(dgx_hw_curve_t) * (room ? room : 1));
    way->symbols = malloc(sizeof(uint16_t) * (room ? room : 1));
    if (!way->curves || !way->symbols) {
        dgx_hw_way_free(way);
        return false;
    }
    dgx_hw_writer_begin(&w, font, text, joined);
    for (int i = 0; i < way->number && dgx_hw_writer_next(&w, &s); ++i) {
        dgx_hw_curve_t *c = &way->curves[i];
        memcpy(c->x, s.x, sizeof(float) * s.type);
        memcpy(c->y, s.y, sizeof(float) * s.type);
        /* any stroke as a cubic curve drawing the same */
        if (s.type == DGX_HW_CURVE3P) {
            c->x[3] = c->x[2], c->y[3] = c->y[2];
            c->x[2] = c->x[3] + (c->x[1] - c->x[3]) * 2 / 3, c->y[2] = c->y[3] + (c->y[1] - c->y[3]) * 2 / 3;
            c->x[1] = c->x[0] + (c->x[1] - c->x[0]) * 2 / 3, c->y[1] = c->y[0] + (c->y[1] - c->y[0]) * 2 / 3;
        } else if (s.type == DGX_HW_LINE) {
            c->x[3] = c->x[1], c->y[3] = c->y[1];
            c->x[1] = c->x[0] + (c->x[3] - c->x[0]) / 3, c->y[1] = c->y[0] + (c->y[3] - c->y[0]) / 3;
            c->x[2] = c->x[0] + (c->x[3] - c->x[0]) * 2 / 3, c->y[2] = c->y[0] + (c->y[3] - c->y[0]) * 2 / 3;
        } else if (s.type == DGX_HW_DOT) {
            c->x[1] = c->x[2] = c->x[3] = c->x[0], c->y[1] = c->y[2] = c->y[3] = c->y[0];
        }
        way->symbols[i] = s.symbol;
    }
    return true;
}

static void dgx_hw_curve_point(dgx_hw_curve_t *c, float x, float y)
{
    for (int i = 0; i < 4; ++i) c->x[i] = x, c->y[i] = y;
}

/* about the length of a curve: between its chord and the line through its points */
static float dgx_hw_curve_length(const dgx_hw_curve_t *c)
{
    float through = 0;
    for (int i = 1; i < 4; ++i) through += hypotf(c->x[i] - c->x[i - 1], c->y[i] - c->y[i - 1]);
    return (through + hypotf(c->x[3] - c->x[0], c->y[3] - c->y[0])) / 2;
}

/* the longest curves are cut in two, each half drawing its part of the same curve, till there are number of them */
static void dgx_hw_way_cut(dgx_hw_way_t *way, int number)
{
    while (way->number > 0 && way->number < number) {
        int   longest = 0;
        float length = -1;
        for (int i = 0; i < way->number; ++i) {
            float l = dgx_hw_curve_length(&way->curves[i]);
            if (l > length) length = l, longest = i;
        }
        dgx_hw_curve_t *c = &way->curves[longest];
        memmove(c + 1, c, sizeof(dgx_hw_curve_t) * (way->number - longest));
        dgx_hw_curve_t *d = c + 1;
        for (int k = 0; k < 2; ++k) {
            float *p = k ? c->y : c->x, *q = k ? d->y : d->x;
            float  a = (p[0] + p[1]) / 2, b = (p[1] + p[2]) / 2, e = (p[2] + p[3]) / 2;
            float  ab = (a + b) / 2, be = (b + e) / 2, m = (ab + be) / 2;
            p[1] = a, p[2] = ab, p[3] = m;
            q[0] = m, q[1] = be, q[2] = e;
        }
        ++way->number;
    }
}

static dgx_hw_morph_t *dgx_hw_morph_new(int number)
{
    dgx_hw_morph_t *m = calloc(1, sizeof(dgx_hw_morph_t));
    if (!m) return 0;
    m->number = (uint16_t)number;
    m->from = malloc(sizeof(dgx_hw_curve_t) * number);
    m->to = malloc(sizeof(dgx_hw_curve_t) * number);
    m->nothing = calloc(number, 1);
    if (!m->from || !m->to || !m->nothing) dgx_hw_morph_destroy(&m);
    return m;
}

/* whether a morph changes anything is seen from its curves */
static void dgx_hw_morph_done(dgx_hw_morph_t *m)
{
    m->changes = memcmp(m->from, m->to, sizeof(dgx_hw_curve_t) * m->number) != 0;
    for (int i = 0; i < m->number; ++i) m->changes = m->changes || m->nothing[i];
}

void dgx_hw_morph_destroy(dgx_hw_morph_t **morph)
{
    if (!morph || !*morph) return;
    free((*morph)->from);
    free((*morph)->to);
    free((*morph)->nothing);
    free(*morph);
    *morph = 0;
}

void dgx_hw_morph_shift(dgx_hw_morph_t *m, float from_x, float from_y, float to_x, float to_y)
{
    if (!m) return;
    for (int i = 0; i < m->number; ++i) {
        for (int k = 0; k < 4; ++k) {
            m->from[i].x[k] += from_x, m->from[i].y[k] += from_y;
            m->to[i].x[k] += to_x, m->to[i].y[k] += to_y;
        }
    }
    dgx_hw_morph_done(m);
}

dgx_hw_morph_t *dgx_hw_morph_create(dgx_font_t *font, const char *from, const char *to, bool joined)
{
    dgx_hw_way_t    a = {0}, b = {0}, count;
    dgx_hw_morph_t *m = 0;
    /* either way may have to grow to the number of the other one */
    if (!dgx_hw_way_of(&count, font, to, joined, 0)) return 0;
    dgx_hw_way_free(&count);
    if (!dgx_hw_way_of(&a, font, from, joined, count.number) || !dgx_hw_way_of(&b, font, to, joined, a.number)) goto done;
    int number = a.number > b.number ? a.number : b.number;
    if (!number || number > UINT16_MAX || !(m = dgx_hw_morph_new(number))) goto done;
    dgx_hw_way_cut(&a, number);
    dgx_hw_way_cut(&b, number);
    for (int i = 0; i < number; ++i) {
        /* a text of nothing: the other one grows out of the starts of its strokes */
        if (a.number) m->from[i] = a.curves[i];
        else dgx_hw_curve_point(&m->from[i], b.curves[i].x[0], b.curves[i].y[0]), m->nothing[i] = DGX_HW_MORPH_FROM_NOTHING;
        if (b.number) m->to[i] = b.curves[i];
        else dgx_hw_curve_point(&m->to[i], a.curves[i].x[0], a.curves[i].y[0]), m->nothing[i] = DGX_HW_MORPH_TO_NOTHING;
    }
    dgx_hw_morph_done(m);
done:
    dgx_hw_way_free(&a);
    dgx_hw_way_free(&b);
    return m;
}

void dgx_hw_morph_draw(const dgx_hw_morph_t *m, float t, dgx_screen_t *scr, int x, int y, float scale, int width, uint32_t color)
{
    dgx_hw_morph_draw_xy(m, t, scr, x, y, scale, scale, width, color);
}

void dgx_hw_morph_draw_xy(const dgx_hw_morph_t *m, float t, dgx_screen_t *scr, int x, int y, float scale_x, float scale_y, int width,
                          uint32_t color)
{
    if (!m) return;
    if (!(t > 0)) t = 0; /* and what is not a number */
    if (t > 1) t = 1;
    /* as a text is written: a size that is not above nothing is nothing */
    if (!(scale_x > 0)) scale_x = 0;
    if (!(scale_y > 0)) scale_y = 0;
    dgx_screen_progress_up(scr);
    for (int i = 0; i < m->number; ++i) {
        const dgx_hw_curve_t *a = &m->from[i], *b = &m->to[i];
        dgx_point_2d_t        p[4];
        /* what is not there yet, or not there any more, is not a dot */
        if ((t <= 0 && (m->nothing[i] & DGX_HW_MORPH_FROM_NOTHING)) || (t >= 1 && (m->nothing[i] & DGX_HW_MORPH_TO_NOTHING))) continue;
        for (int k = 0; k < 4; ++k) {
            p[k].x = dgx_hw_pixel(x + (a->x[k] + (b->x[k] - a->x[k]) * t) * scale_x);
            p[k].y = dgx_hw_pixel(y + (a->y[k] + (b->y[k] - a->y[k]) * t) * scale_y);
        }
        dgx_draw_bezier4(scr, p, 0, width, color);
    }
    dgx_screen_progress_down(scr);
}

bool dgx_hw_morph_box(const dgx_hw_morph_t *m, int *left, int *top, int *right, int *bottom)
{
    if (!m || !m->number) return false;
    float l = m->from[0].x[0], t = m->from[0].y[0], r = l, b = t;
    for (int i = 0; i < m->number; ++i) {
        for (int k = 0; k < 8; ++k) {
            const dgx_hw_curve_t *c = k < 4 ? &m->from[i] : &m->to[i];
            if (c->x[k & 3] < l) l = c->x[k & 3];
            if (c->x[k & 3] > r) r = c->x[k & 3];
            if (c->y[k & 3] < t) t = c->y[k & 3];
            if (c->y[k & 3] > b) b = c->y[k & 3];
        }
    }
    *left = (int)floorf(l), *top = (int)floorf(t), *right = (int)ceilf(r), *bottom = (int)ceilf(b);
    return true;
}

/* how many curves of a way are of a symbol, and the first of them */
static int dgx_hw_way_symbol(const dgx_hw_way_t *way, size_t symbol, int from, int *found)
{
    int number = 0;
    for (int i = from; i < way->number; ++i) {
        if (way->symbols[i] != symbol) continue;
        found[number++] = i;
    }
    return number;
}

/*
 * Where the pen of a text is after a symbol: the end of the last stroke of
 * that symbol or, when it has none, of the symbols before it; the start of the
 * text when there are none either.
 */
static bool dgx_hw_way_pen(const dgx_hw_way_t *way, size_t symbol, float *x, float *y)
{
    int last = -1;
    for (int i = 0; i < way->number; ++i) {
        if (way->symbols[i] <= symbol) last = i;
    }
    if (!way->number) return false;
    if (last < 0) *x = way->curves[0].x[0], *y = way->curves[0].y[0];
    else *x = way->curves[last].x[3], *y = way->curves[last].y[3];
    return true;
}

/* the way the four points of one curve go to become another; the other one may be taken from its end */
static float dgx_hw_curve_way(const dgx_hw_curve_t *a, const dgx_hw_curve_t *b, bool turned)
{
    float way = 0;
    for (int k = 0; k < 4; ++k) {
        int at = turned ? 3 - k : k;
        way += hypotf(b->x[at] - a->x[k], b->y[at] - a->y[k]);
    }
    return way;
}

/* a symbol of more strokes than this is paired in the order of writing: 2^n numbers are kept to choose the pairs */
#define DGX_HW_MORPH_PAIRED_STROKES 10

/*
 * Which stroke of one symbol goes into which stroke of the other. Symbols are
 * drawn as they are written, and the order of writing is not the order in
 * which their parts answer one another: the oval of "9" is written first and
 * its tail last, in "7" the top comes first. Paired by that order, a curve
 * flies across the whole symbol. So the pairs are those with the shortest way
 * of all the points together, and a curve may be taken from its other end,
 * which draws the same.
 *
 * way[i * n + j] is the way of stroke i of the first symbol into stroke j of
 * the second one; a symbol of fewer strokes is filled up with strokes that
 * are not there. to[i] becomes the pair of i. All the ways of choosing are
 * gone through by the sets of strokes taken so far: best[set] is the
 * shortest way of pairing the first strokes with that set.
 */
static bool dgx_hw_morph_pairs(int n, const float *way, uint8_t *to)
{
    size_t   sets = (size_t)1 << n;
    float   *best = malloc(sizeof(float) * sets);
    uint8_t *last = malloc(sets);
    if (!best || !last) {
        free(best);
        free(last);
        return false;
    }
    for (size_t set = 1; set < sets; ++set) best[set] = INFINITY;
    best[0] = 0;
    for (size_t set = 0; set + 1 < sets; ++set) {
        int i = __builtin_popcount((unsigned)set);
        for (int j = 0; j < n; ++j) {
            size_t with = set | ((size_t)1 << j);
            if (with == set) continue;
            float w = best[set] + way[i * n + j];
            if (w < best[with]) best[with] = w, last[with] = (uint8_t)j;
        }
    }
    for (size_t set = sets - 1; set; set &= ~((size_t)1 << last[set])) to[__builtin_popcount((unsigned)set) - 1] = last[set];
    free(best);
    free(last);
    return true;
}

dgx_hw_morph_text_t *dgx_hw_morph_text_create(dgx_font_t *font, const char *from, const char *to, bool joined)
{
    dgx_hw_way_t         a = {0}, b = {0};
    dgx_hw_morph_text_t *text = calloc(1, sizeof(dgx_hw_morph_text_t));
    int                 *of_a = 0, *of_b = 0;
    bool                 ok = false;
    if (!text || !dgx_hw_way_of(&a, font, from, joined, 0) || !dgx_hw_way_of(&b, font, to, joined, 0)) goto done;
    text->length = a.length > b.length ? a.length : b.length;
    text->letters = calloc(text->length ? text->length : 1, sizeof(dgx_hw_morph_t *));
    of_a = malloc(sizeof(int) * (a.number ? a.number : 1));
    of_b = malloc(sizeof(int) * (b.number ? b.number : 1));
    if (!text->letters || !of_a || !of_b) goto done;
    for (size_t i = 0; i < text->length; ++i) {
        int na = dgx_hw_way_symbol(&a, i, 0, of_a), nb = dgx_hw_way_symbol(&b, i, 0, of_b);
        int number = na > nb ? na : nb;
        if (!number) continue;
        dgx_hw_morph_t *m = text->letters[i] = dgx_hw_morph_new(number);
        if (!m) goto done;
        float ax = 0, ay = 0, bx = 0, by = 0;
        bool  pen_a = dgx_hw_way_pen(&a, i, &ax, &ay), pen_b = dgx_hw_way_pen(&b, i, &bx, &by);
        /*
         * The strokes of both symbols, each filled up to the same number: a stroke
         * without a pair grows from where the pen of the other text is, or from its
         * own start, and one that has nothing to become goes there.
         */
        for (int k = 0; k < number; ++k) {
            if (k < na) m->from[k] = a.curves[of_a[k]];
            else dgx_hw_curve_point(&m->from[k], ax, ay);
            if (k < nb) m->to[k] = b.curves[of_b[k]];
            else dgx_hw_curve_point(&m->to[k], bx, by);
        }
        uint8_t pair[DGX_HW_MORPH_PAIRED_STROKES];
        uint8_t turned[DGX_HW_MORPH_PAIRED_STROKES * DGX_HW_MORPH_PAIRED_STROKES];
        float   way[DGX_HW_MORPH_PAIRED_STROKES * DGX_HW_MORPH_PAIRED_STROKES];
        bool    paired = false;
        if (number >= 1 && number <= DGX_HW_MORPH_PAIRED_STROKES) {
            for (int k = 0; k < number; ++k) {
                for (int j = 0; j < number; ++j) {
                    float straight = dgx_hw_curve_way(&m->from[k], &m->to[j], false);
                    float back = dgx_hw_curve_way(&m->from[k], &m->to[j], true);
                    /* a stroke that is not there has no way of its own, only that of its pair into the point */
                    if (k >= na && !pen_a) straight = back = 0;
                    if (j >= nb && !pen_b) straight = back = 0;
                    turned[k * number + j] = back < straight;
                    way[k * number + j] = back < straight ? back : straight;
                }
            }
            /* a stroke alone has its pair, and may still be taken from its other end */
            pair[0] = 0;
            if (number > 1 && !dgx_hw_morph_pairs(number, way, pair)) goto done;
            paired = true;
        }
        if (paired) {
            dgx_hw_curve_t *ordered = malloc(sizeof(dgx_hw_curve_t) * number);
            if (!ordered) goto done;
            for (int k = 0; k < number; ++k) {
                const dgx_hw_curve_t *c = &m->to[pair[k]];
                for (int p = 0; p < 4; ++p) {
                    int at = turned[k * number + pair[k]] ? 3 - p : p;
                    ordered[k].x[p] = c->x[at], ordered[k].y[p] = c->y[at];
                }
                if (pair[k] >= nb) m->nothing[k] |= DGX_HW_MORPH_TO_NOTHING;
            }
            memcpy(m->to, ordered, sizeof(dgx_hw_curve_t) * number);
            free(ordered);
        } else {
            for (int k = nb; k < number; ++k) m->nothing[k] |= DGX_HW_MORPH_TO_NOTHING;
        }
        for (int k = na; k < number; ++k) m->nothing[k] |= DGX_HW_MORPH_FROM_NOTHING;
        /* with no pen to grow from or go to, a stroke does it at its own start */
        for (int k = 0; k < number; ++k) {
            if ((m->nothing[k] & DGX_HW_MORPH_FROM_NOTHING) && !pen_a) dgx_hw_curve_point(&m->from[k], m->to[k].x[0], m->to[k].y[0]);
            if ((m->nothing[k] & DGX_HW_MORPH_TO_NOTHING) && !pen_b) dgx_hw_curve_point(&m->to[k], m->from[k].x[0], m->from[k].y[0]);
        }
        dgx_hw_morph_done(m);
        if (m->changes) text->changed = i + 1;
    }
    ok = true;
done:
    free(of_a);
    free(of_b);
    dgx_hw_way_free(&a);
    dgx_hw_way_free(&b);
    if (!ok) dgx_hw_morph_text_destroy(&text);
    return text;
}

void dgx_hw_morph_text_destroy(dgx_hw_morph_text_t **text)
{
    if (!text || !*text) return;
    if ((*text)->letters) {
        for (size_t i = 0; i < (*text)->length; ++i) dgx_hw_morph_destroy(&(*text)->letters[i]);
    }
    free((*text)->letters);
    free(*text);
    *text = 0;
}

void dgx_hw_morph_text_shift(dgx_hw_morph_text_t *text, float from_x, float from_y, float to_x, float to_y)
{
    if (!text) return;
    text->changed = 0;
    for (size_t i = 0; i < text->length; ++i) {
        dgx_hw_morph_shift(text->letters[i], from_x, from_y, to_x, to_y);
        if (text->letters[i] && text->letters[i]->changes) text->changed = i + 1;
    }
}

int64_t dgx_hw_morph_text_duration_us(const dgx_hw_morph_text_t *text, int64_t duration_us, int64_t stagger_us)
{
    if (!text || !text->changed) return 0;
    return (int64_t)(text->changed - 1) * stagger_us + duration_us;
}

/*
 * text.c - sprite lookups and RGB565 alpha blending.
 *
 * No allocation, no font engine: labels come from the generated tables and
 * everything else falls back to the ASCII strip.
 */
#include "text.h"
#include "text_data.h"

#include <stddef.h>
#include <stdint.h>

/* The generator only emits 12 and 16 px tables; clamp anything else. */
static int norm_px(int px)
{
    return px >= 14 ? 16 : 12;
}

/*
 * Decode one UTF-8 codepoint.  Returns -1 for malformed input; *pp always
 * advances by at least one byte so callers cannot loop forever.
 */
static int utf8_next(const unsigned char **pp)
{
    const unsigned char *s = *pp;
    unsigned int c = s[0];
    unsigned int cp;
    int n;
    int i;

    if (c < 0x80) {
        *pp = s + 1;
        return (int)c;
    }
    if ((c & 0xE0) == 0xC0) {
        n = 1;
        cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        cp = c & 0x07u;
    } else {
        *pp = s + 1;
        return -1;
    }
    for (i = 1; i <= n; i++) {
        unsigned int cc = s[i];
        if ((cc & 0xC0u) != 0x80u) {
            *pp = s + 1;
            return -1;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    *pp = s + n + 1;
    return (int)cp;
}

static const TextSpriteData *ascii_glyph(int codepoint, int px)
{
    const TextSpriteData *g = text_data_ascii(codepoint, px);

    if (g == NULL)
        g = text_data_ascii('?', px);
    return g;
}

/* Divide by 255, rounded, for values < 65536. */
static unsigned int div255(unsigned int v)
{
    v += 128u;
    return (v + (v >> 8)) >> 8;
}

static void blend_span(uint16_t *dst, const uint8_t *mask, int n, uint16_t color)
{
    unsigned int sr = (color >> 11) & 0x1Fu;
    unsigned int sg = (color >> 5) & 0x3Fu;
    unsigned int sb = color & 0x1Fu;
    int i;

    for (i = 0; i < n; i++) {
        unsigned int a = mask[i];

        if (a == 0)
            continue;
        if (a == 255) {
            dst[i] = color;
            continue;
        }
        {
            unsigned int ia = 255u - a;
            unsigned int d = dst[i];
            unsigned int r = div255(sr * a + ((d >> 11) & 0x1Fu) * ia);
            unsigned int g = div255(sg * a + ((d >> 5) & 0x3Fu) * ia);
            unsigned int b = div255(sb * a + (d & 0x1Fu) * ia);

            dst[i] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

/* Blend a w*h mask with its top-left at (x, y), clipped to clip_w x clip_h. */
static void put_mask(uint16_t *buf, int pitch, int clip_w, int clip_h,
                     const uint8_t *mask, int w, int h, int x, int y,
                     uint16_t color)
{
    int sx0 = 0;
    int sx1 = w;
    int sy;

    if (buf == NULL || mask == NULL || w <= 0 || h <= 0 || pitch <= 0)
        return;
    if (clip_w <= 0 || clip_h <= 0)
        return;

    if (x < 0)
        sx0 = -x;
    if (x + sx1 > clip_w)
        sx1 = clip_w - x;
    if (sx0 >= sx1)
        return;

    for (sy = 0; sy < h; sy++) {
        int dy = y + sy;
        if (dy < 0)
            continue;
        if (dy >= clip_h)
            break;
        blend_span(buf + (size_t)dy * (size_t)pitch + x + sx0,
                   mask + (size_t)sy * (size_t)w + sx0, sx1 - sx0, color);
    }
}

int text_init(void)
{
    return 0;
}

const TextSprite *text_lookup(const char *utf8, int px)
{
    if (utf8 == NULL)
        return NULL;
    return text_data_sprite(utf8, norm_px(px));
}

int text_width(const char *utf8, int px)
{
    const unsigned char *p;
    const TextSpriteData *sprite;
    int width = 0;

    if (utf8 == NULL)
        return 0;
    px = norm_px(px);
    sprite = text_data_find(utf8, px);
    if (sprite != NULL)
        return sprite->advance;

    p = (const unsigned char *)utf8;
    while (*p != 0) {
        const TextSpriteData *g = ascii_glyph(utf8_next(&p), px);
        if (g != NULL)
            width += g->advance;
    }
    return width;
}

int text_draw(uint16_t *buf, int pitch, int clip_w, int clip_h, const char *utf8,
              int x, int y, int px, uint16_t color)
{
    const TextSpriteData *sprite;
    const unsigned char *p;
    int pen = 0;

    if (utf8 == NULL)
        return 0;
    px = norm_px(px);

    sprite = text_data_find(utf8, px);
    if (sprite != NULL) {
        put_mask(buf, pitch, clip_w, clip_h, sprite->mask, sprite->w,
                 sprite->h, x + sprite->bx, y + sprite->by, color);
        return sprite->advance;
    }

    p = (const unsigned char *)utf8;
    while (*p != 0) {
        const TextSpriteData *g = ascii_glyph(utf8_next(&p), px);
        if (g == NULL)
            continue;
        put_mask(buf, pitch, clip_w, clip_h, g->mask, g->w, g->h,
                 x + pen + g->bx, y + g->by, color);
        pen += g->advance;
    }
    return pen;
}

int text_draw_center(uint16_t *buf, int pitch, int clip_w, int clip_h,
                     const char *utf8, int x, int y, int w, int px,
                     uint16_t color)
{
    int width = text_width(utf8, px);

    return text_draw(buf, pitch, clip_w, clip_h, utf8, x + (w - width) / 2, y,
                     px, color);
}

int text_draw_middle(uint16_t *buf, int pitch, int clip_w, int clip_h,
                     const char *utf8, int x, int y, int w, int h, int px,
                     uint16_t color)
{
    int top = y + (h - text_line_height(px)) / 2;

    return text_draw_center(buf, pitch, clip_w, clip_h, utf8, x, top, w, px,
                            color);
}

int text_line_height(int px)
{
    return text_data_line_height(norm_px(px));
}

/*
 * gfx.h - tiny RGB565 software renderer (no dependencies).
 *
 * Every function takes a destination buffer, its pitch in pixels, a clip
 * rectangle in destination coordinates, and clips to it.
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

typedef struct {
    uint16_t *buf;
    int pitch; /* pixels per row */
    int cx, cy, cw, ch;
} Gfx;

void gfx_init(Gfx *g, uint16_t *buf, int pitch, int clip_w, int clip_h);
void gfx_clip(Gfx *g, int x, int y, int w, int h);
void gfx_reset_clip(Gfx *g, int clip_w, int clip_h);

static inline uint16_t gfx_rgb565(int r, int g, int b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void gfx_clear(Gfx *g, uint16_t color);
void gfx_fill(Gfx *g, int x, int y, int w, int h, uint16_t color);
void gfx_hline(Gfx *g, int x, int y, int w, uint16_t color);
void gfx_vline(Gfx *g, int x, int y, int h, uint16_t color);
void gfx_line(Gfx *g, int x0, int y0, int x1, int y1, uint16_t color);
void gfx_frame(Gfx *g, int x, int y, int w, int h, uint16_t color); /* 1 px */
void gfx_fill_circle(Gfx *g, int cx, int cy, int r, uint16_t color);
void gfx_circle(Gfx *g, int cx, int cy, int r, uint16_t color);
void gfx_fill_round(Gfx *g, int x, int y, int w, int h, int r, uint16_t color);

/*
 * Alpha blend of a rounded rectangle.  Used for press highlights and soft
 * shadows; per-row spans keep it cheap enough for the 26 fps draw path.
 */
void gfx_fill_round_alpha(Gfx *g, int x, int y, int w, int h, int r,
                          uint16_t color, int alpha);

/*
 * Alpha blend of a solid colour over the destination.
 * alpha 0..255, applied per pixel (used for the status strip / toast).
 */
void gfx_fill_alpha(Gfx *g, int x, int y, int w, int h, uint16_t color, int alpha);

/*
 * Blit an 8 bit alpha mask, tinted with `color`.  Used for text glyphs and
 * the pre-rendered icons.
 */
void gfx_blit_mask(Gfx *g, const uint8_t *mask, int mw, int mh, int mx, int my,
                   int x, int y, uint16_t color, int alpha);

/* Copy an RGB565 image; no scaling. */
void gfx_blit565(Gfx *g, const uint16_t *src, int spitch, int sw, int sh, int x,
                 int y);

/* Nearest-neighbour scale, used for photo thumbnails. */
void gfx_blit565_scaled(Gfx *g, const uint16_t *src, int spitch, int sw, int sh,
                        int x, int y, int dw, int dh);

/* Aspect-fit / aspect-fill helpers return the destination rect. */
typedef struct {
    int x, y, w, h;
} Rect;

Rect gfx_fit(int sw, int sh, int dw, int dh);
Rect gfx_cover(int sw, int sh, int dw, int dh);

#endif /* GFX_H */

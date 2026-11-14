#include "gfx.h"

#include <string.h>

static inline uint16_t mix565(uint16_t dst, uint16_t src, int alpha) {
    /* alpha 0..255; split into 5/6/5 and blend independently. */
    if (alpha <= 0) {
        return dst;
    }
    if (alpha >= 255) {
        return src;
    }
    {
        int dr = (dst >> 11) & 0x1F;
        int dg = (dst >> 5) & 0x3F;
        int db = dst & 0x1F;
        int sr = (src >> 11) & 0x1F;
        int sg = (src >> 5) & 0x3F;
        int sb = src & 0x1F;
        int ia = 255 - alpha;
        int r = (dr * ia + sr * alpha + 127) / 255;
        int g = (dg * ia + sg * alpha + 127) / 255;
        int b = (db * ia + sb * alpha + 127) / 255;
        return (uint16_t)((r << 11) | (g << 5) | b);
    }
}

void gfx_init(Gfx *g, uint16_t *buf, int pitch, int clip_w, int clip_h) {
    g->buf = buf;
    g->pitch = pitch;
    g->cx = 0;
    g->cy = 0;
    g->cw = clip_w;
    g->ch = clip_h;
}

void gfx_clip(Gfx *g, int x, int y, int w, int h) {
    int x0 = g->cx > x ? g->cx : x;
    int y0 = g->cy > y ? g->cy : y;
    int x1 = (g->cx + g->cw) < (x + w) ? (g->cx + g->cw) : (x + w);
    int y1 = (g->cy + g->ch) < (y + h) ? (g->cy + g->ch) : (y + h);
    g->cx = x0;
    g->cy = y0;
    g->cw = x1 > x0 ? x1 - x0 : 0;
    g->ch = y1 > y0 ? y1 - y0 : 0;
}

void gfx_reset_clip(Gfx *g, int clip_w, int clip_h) {
    g->cx = 0;
    g->cy = 0;
    g->cw = clip_w;
    g->ch = clip_h;
}

void gfx_clear(Gfx *g, uint16_t color) {
    for (int y = 0; y < g->ch; ++y) {
        uint16_t *row = g->buf + (size_t)(g->cy + y) * g->pitch + g->cx;
        for (int x = 0; x < g->cw; ++x) {
            row[x] = color;
        }
    }
}

void gfx_fill(Gfx *g, int x, int y, int w, int h, uint16_t color) {
    int x0 = x > g->cx ? x : g->cx;
    int y0 = y > g->cy ? y : g->cy;
    int x1 = (x + w) < (g->cx + g->cw) ? (x + w) : (g->cx + g->cw);
    int y1 = (y + h) < (g->cy + g->ch) ? (y + h) : (g->cy + g->ch);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    for (int row = y0; row < y1; ++row) {
        uint16_t *p = g->buf + (size_t)row * g->pitch + x0;
        for (int col = x0; col < x1; ++col) {
            *p++ = color;
        }
    }
}

void gfx_hline(Gfx *g, int x, int y, int w, uint16_t color) {
    gfx_fill(g, x, y, w, 1, color);
}

void gfx_vline(Gfx *g, int x, int y, int h, uint16_t color) {
    gfx_fill(g, x, y, 1, h, color);
}

void gfx_line(Gfx *g, int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (x0 >= g->cx && x0 < g->cx + g->cw && y0 >= g->cy &&
            y0 < g->cy + g->ch) {
            g->buf[(size_t)y0 * g->pitch + x0] = color;
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        {
            int e2 = err * 2;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

void gfx_frame(Gfx *g, int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    gfx_hline(g, x, y, w, color);
    gfx_hline(g, x, y + h - 1, w, color);
    gfx_vline(g, x, y, h, color);
    gfx_vline(g, x + w - 1, y, h, color);
}

void gfx_fill_circle(Gfx *g, int cx, int cy, int r, uint16_t color) {
    int x = r;
    int y = 0;
    int err = 1 - r;
    if (r <= 0) {
        gfx_fill(g, cx, cy, 1, 1, color);
        return;
    }
    while (x >= y) {
        gfx_hline(g, cx - x, cy + y, 2 * x + 1, color);
        gfx_hline(g, cx - x, cy - y, 2 * x + 1, color);
        gfx_hline(g, cx - y, cy + x, 2 * y + 1, color);
        gfx_hline(g, cx - y, cy - x, 2 * y + 1, color);
        ++y;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            --x;
            err += 2 * (y - x) + 1;
        }
    }
}

void gfx_circle(Gfx *g, int cx, int cy, int r, uint16_t color) {
    int x = r;
    int y = 0;
    int err = 1 - r;
    if (r <= 0) {
        gfx_fill(g, cx, cy, 1, 1, color);
        return;
    }
    while (x >= y) {
        const int pts[8][2] = {
            {cx + x, cy + y}, {cx + y, cy + x}, {cx - y, cy + x},
            {cx - x, cy + y}, {cx - x, cy - y}, {cx - y, cy - x},
            {cx + y, cy - x}, {cx + x, cy - y}};
        for (int i = 0; i < 8; ++i) {
            int px = pts[i][0];
            int py = pts[i][1];
            if (px >= g->cx && px < g->cx + g->cw && py >= g->cy &&
                py < g->cy + g->ch) {
                g->buf[(size_t)py * g->pitch + px] = color;
            }
        }
        ++y;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            --x;
            err += 2 * (y - x) + 1;
        }
    }
}

void gfx_fill_round(Gfx *g, int x, int y, int w, int h, int r, uint16_t color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    if (r * 2 > w) {
        r = w / 2;
    }
    if (r * 2 > h) {
        r = h / 2;
    }
    if (r <= 0) {
        gfx_fill(g, x, y, w, h, color);
        return;
    }
    /* Body plus the four corner caps. */
    gfx_fill(g, x, y + r, w, h - 2 * r, color);
    gfx_fill(g, x + r, y, w - 2 * r, r, color);
    gfx_fill(g, x + r, y + h - r, w - 2 * r, r, color);
    gfx_fill_circle(g, x + r, y + r, r - 1, color);
    gfx_fill_circle(g, x + w - r - 1, y + r, r - 1, color);
    gfx_fill_circle(g, x + r, y + h - r - 1, r - 1, color);
    gfx_fill_circle(g, x + w - r - 1, y + h - r - 1, r - 1, color);
}

void gfx_fill_alpha(Gfx *g, int x, int y, int w, int h, uint16_t color,
                    int alpha) {
    int x0 = x > g->cx ? x : g->cx;
    int y0 = y > g->cy ? y : g->cy;
    int x1 = (x + w) < (g->cx + g->cw) ? (x + w) : (g->cx + g->cw);
    int y1 = (y + h) < (g->cy + g->ch) ? (y + h) : (g->cy + g->ch);
    if (alpha <= 0 || x1 <= x0 || y1 <= y0) {
        return;
    }
    if (alpha >= 255) {
        gfx_fill(g, x0, y0, x1 - x0, y1 - y0, color);
        return;
    }
    for (int row = y0; row < y1; ++row) {
        uint16_t *p = g->buf + (size_t)row * g->pitch + x0;
        for (int col = x0; col < x1; ++col) {
            *p = mix565(*p, color, alpha);
            ++p;
        }
    }
}

void gfx_blit_mask(Gfx *g, const uint8_t *mask, int mw, int mh, int mx,
                   int my, int x, int y, uint16_t color, int alpha) {
    int sy0 = 0;
    int sx0 = 0;
    int sx1 = mw;
    int sy1 = mh;

    if (x < g->cx) {
        sx0 = g->cx - x;
    }
    if (y < g->cy) {
        sy0 = g->cy - y;
    }
    if (x + sx1 > g->cx + g->cw) {
        sx1 = g->cx + g->cw - x;
    }
    if (y + sy1 > g->cy + g->ch) {
        sy1 = g->cy + g->ch - y;
    }
    if (sx0 < 0) {
        sx0 = 0;
    }
    if (sy0 < 0) {
        sy0 = 0;
    }
    if (sx0 >= sx1 || sy0 >= sy1) {
        return;
    }

    for (int row = sy0; row < sy1; ++row) {
        const uint8_t *src = mask + (size_t)(my + row) * mw + (mx + sx0);
        uint16_t *dst = g->buf + (size_t)(y + row) * g->pitch + (x + sx0);
        for (int col = sx0; col < sx1; ++col) {
            int a = *src++;
            if (a) {
                if (alpha < 255) {
                    a = a * alpha / 255;
                }
                *dst = mix565(*dst, color, a);
            }
            ++dst;
        }
    }
}

void gfx_blit565(Gfx *g, const uint16_t *src, int spitch, int sw, int sh, int x,
                 int y) {
    int sx0 = 0;
    int sy0 = 0;
    int sx1 = sw;
    int sy1 = sh;

    if (x < g->cx) {
        sx0 = g->cx - x;
    }
    if (y < g->cy) {
        sy0 = g->cy - y;
    }
    if (x + sx1 > g->cx + g->cw) {
        sx1 = g->cx + g->cw - x;
    }
    if (y + sy1 > g->cy + g->ch) {
        sy1 = g->cy + g->ch - y;
    }
    if (sx0 >= sx1 || sy0 >= sy1) {
        return;
    }
    for (int row = sy0; row < sy1; ++row) {
        memcpy(g->buf + (size_t)(y + row) * g->pitch + (x + sx0),
               src + (size_t)(sy0 + (row - sy0)) * spitch + sx0,
               (size_t)(sx1 - sx0) * 2u);
    }
}

void gfx_blit565_scaled(Gfx *g, const uint16_t *src, int spitch, int sw, int sh,
                        int x, int y, int dw, int dh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) {
        return;
    }
    for (int row = 0; row < dh; ++row) {
        int dy = y + row;
        int sy = row * sh / dh;
        const uint16_t *src_row;
        uint16_t *dst_row;
        if (dy < g->cy || dy >= g->cy + g->ch) {
            continue;
        }
        src_row = src + (size_t)sy * spitch;
        dst_row = g->buf + (size_t)dy * g->pitch;
        for (int col = 0; col < dw; ++col) {
            int dx = x + col;
            int sx;
            if (dx < g->cx || dx >= g->cx + g->cw) {
                continue;
            }
            sx = col * sw / dw;
            dst_row[dx] = src_row[sx];
        }
    }
}

Rect gfx_fit(int sw, int sh, int dw, int dh) {
    Rect r = {0, 0, dw, dh};
    if (sw <= 0 || sh <= 0) {
        return r;
    }
    {
        int w = dw;
        int h = sh * dw / sw;
        if (h > dh) {
            h = dh;
            w = sw * dh / sh;
        }
        r.w = w;
        r.h = h;
        r.x = (dw - w) / 2;
        r.y = (dh - h) / 2;
    }
    return r;
}

Rect gfx_cover(int sw, int sh, int dw, int dh) {
    Rect r = {0, 0, dw, dh};
    if (sw <= 0 || sh <= 0) {
        return r;
    }
    if ((long)sw * dh > (long)dw * sh) {
        r.w = sw * dh / sh;
        r.h = dh;
        r.x = (dw - r.w) / 2;
    } else {
        r.w = dw;
        r.h = sh * dw / sw;
        r.y = (dh - r.h) / 2;
    }
    return r;
}

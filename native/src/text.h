/*
 * text.h - pre-rendered CJK/ASCII text.
 *
 * There is no font engine at runtime.  tools/gen_font.py runs on the build
 * machine with Pillow + DroidSansFallback.ttf and emits src/text_data.c:
 *
 *   - one alpha-bitmap sprite per string listed in tools/strings.txt, at the
 *     sizes 12 and 16 px;
 *   - an ASCII strip (0x20..0x7E) at 12 and 16 px for dynamic values such as
 *     file names, timers, percentages and free space.
 *
 * text.c only does lookups and blends.  Anything not in the sprite table is
 * rendered character by character from the ASCII strip; unknown CJK falls
 * back to a '?' box.
 */
#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>

typedef struct {
    int w, h;      /* bounding box */
    int advance;   /* pen advance */
    int bx, by;    /* bearing: offset from pen origin to bitmap top-left */
    const uint8_t *mask;
} TextSprite;

/* 0 on success.  Safe to call more than once. */
int text_init(void);

/* Returns NULL when the string has no pre-rendered sprite at `px`. */
const TextSprite *text_lookup(const char *utf8, int px);

/* Width in pixels of `utf8` at `px` (uses sprites, then ASCII metrics). */
int text_width(const char *utf8, int px);

/*
 * Draw `utf8` with its pen origin at (x, y) where y is the baseline-free top
 * of the line box.  `px` is the size (12 or 16).  Returns the advance.
 */
int text_draw(uint16_t *buf, int pitch, int clip_w, int clip_h, const char *utf8,
              int x, int y, int px, uint16_t color);

/* Draw with the sprite horizontally centred in [x, x+w). */
int text_draw_center(uint16_t *buf, int pitch, int clip_w, int clip_h,
                     const char *utf8, int x, int y, int w, int px,
                     uint16_t color);

/* Vertical centring helper. */
int text_draw_middle(uint16_t *buf, int pitch, int clip_w, int clip_h,
                     const char *utf8, int x, int y, int w, int h, int px,
                     uint16_t color);

/* Height of the line box for `px`. */
int text_line_height(int px);

#endif /* TEXT_H */

/*
 * text_test.c - host-side exercise of the pre-rendered text renderer.
 *
 * Fills a 320x240 RGB565 buffer, draws CJK labels, ASCII fallbacks and
 * deliberately clipped strings, then writes /tmp/opencode/text_test.ppm.
 *
 * Build/run: tests/run_text_test.sh (or the gcc line in that script).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "text_data.h"

#define W 320
#define H 240
#define PPM_PATH "/tmp/opencode/text_test.ppm"

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static void fill_pattern(uint16_t *fb, int pitch, int w, int h)
{
    int x;
    int y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned r = (unsigned)(x * 24 / (w > 1 ? w - 1 : 1));
            unsigned g = (unsigned)(y * 20 / (h > 1 ? h - 1 : 1));
            unsigned b = 24u + (unsigned)(((x ^ y) & 8) ? 12 : 0);
            uint16_t v = rgb565(r, g, b);

            if ((x % 16) == 0 || (y % 16) == 0)
                v = rgb565(70, 70, 70);
            fb[(size_t)y * (size_t)pitch + (size_t)x] = v;
        }
    }
}

static int write_ppm(const char *path, const uint16_t *fb, int pitch, int w, int h)
{
    static unsigned char rgb[W * H * 3];
    FILE *f;
    int x;
    int y;

    if (w * h > W * H)
        return -1;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint16_t v = fb[(size_t)y * (size_t)pitch + (size_t)x];
            unsigned r = (v >> 11) & 0x1Fu;
            unsigned g = (v >> 5) & 0x3Fu;
            unsigned b = v & 0x1Fu;
            unsigned char *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3u;

            p[0] = (unsigned char)((r << 3) | (r >> 2));
            p[1] = (unsigned char)((g << 2) | (g >> 4));
            p[2] = (unsigned char)((b << 3) | (b >> 2));
        }
    }
    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * (size_t)h * 3u, f);
    fclose(f);
    return 0;
}

static int ascii_w(int cp, int px)
{
    const TextSpriteData *g = text_data_ascii(cp, px);

    return g != NULL ? g->advance : -1;
}

/* Draw a label sprite followed by an ASCII value, the way the UI will. */
static void draw_label_value(uint16_t *fb, const char *label, const char *value,
                             int x, int y, int px, uint16_t label_color,
                             uint16_t value_color)
{
    int w = text_draw(fb, W, W, H, label, x, y, px, label_color);

    text_draw(fb, W, W, H, value, x + w, y, px, value_color);
}

static void test_metrics(void)
{
    const TextSprite *s12;
    const TextSprite *s16;
    const unsigned char unknown[] = {0xE4, 0xB8, 0xAD, 0};      /* 中, not a label */
    const unsigned char emoji[] = {0xF0, 0x9F, 0x98, 0x80, 0}; /* U+1F600 */
    const unsigned char bad[] = {0xE4, 0xB8, 0};

    CHECK(text_init() == 0);
    CHECK(text_line_height(12) == 17);
    CHECK(text_line_height(16) == 22);
    CHECK(text_line_height(13) == 17);
    CHECK(text_line_height(99) == 22);

    s12 = text_lookup("拍照", 12);
    s16 = text_lookup("拍照", 16);
    CHECK(s12 != NULL);
    CHECK(s16 != NULL);
    if (s12 != NULL) {
        CHECK(s12->w == 24);
        CHECK(s12->h > 0);
        CHECK(s12->advance == 24);
        CHECK(s12->mask != NULL);
    }
    if (s16 != NULL) {
        CHECK(s16->w == 32);
        CHECK(s16->advance == 32);
    }
    CHECK(text_lookup("不存在的标签", 12) == NULL);
    CHECK(text_lookup(NULL, 12) == NULL);

    CHECK(text_width("拍照", 12) == 24);
    CHECK(text_width("拍照", 16) == 32);
    CHECK(text_width("", 12) == 0);
    CHECK(text_width(NULL, 12) == 0);

    /* ASCII fallback: sum of the individual glyph advances. */
    CHECK(text_width("AB12", 12) ==
          ascii_w('A', 12) + ascii_w('B', 12) + ascii_w('1', 12) +
              ascii_w('2', 12));
    CHECK(text_width("A B", 16) == ascii_w('A', 16) + ascii_w(' ', 16) +
                                      ascii_w('B', 16));

    /* Unknown CJK and 4-byte sequences use the '?' advance. */
    CHECK(text_width((const char *)unknown, 12) == ascii_w('?', 12));
    CHECK(text_width((const char *)emoji, 16) == ascii_w('?', 16));
    CHECK(text_width((const char *)bad, 12) >= ascii_w('?', 12));
    CHECK(ascii_w(' ', 12) > 0);
    CHECK(ascii_w(0x1F, 12) == -1);
}

static void test_draw_and_clip(void)
{
    enum { PITCH = 53, CW = 47, CH = 33 };
    uint16_t *small = (uint16_t *)malloc((size_t)PITCH * CH * sizeof(uint16_t));
    const char *long_ascii = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    CHECK(small != NULL);
    if (small == NULL)
        return;
    memset(small, 0, (size_t)PITCH * CH * sizeof(uint16_t));

    /* Every edge, negative origins, huge offsets: ASan catches any overrun. */
    text_draw(small, PITCH, CW, CH, "拍照", -20, -20, 16, 0xFFFF);
    text_draw(small, PITCH, CW, CH, "录像中", CW - 5, 0, 16, 0xFFFF);
    text_draw(small, PITCH, CW, CH, long_ascii, -300, 12, 12, 0xF800);
    text_draw(small, PITCH, CW, CH, long_ascii, 5, 12, 12, 0x07E0);
    text_draw(small, PITCH, CW, CH, long_ascii, 0, CH - 3, 16, 0x001F);
    text_draw(small, PITCH, CW, CH, "", 10, 10, 12, 0xFFFF);
    text_draw(small, PITCH, CW, CH, NULL, 10, 10, 12, 0xFFFF);
    text_draw(NULL, PITCH, CW, CH, "拍照", 0, 0, 12, 0xFFFF);
    text_draw(small, PITCH, 0, 0, "拍照", 0, 0, 12, 0xFFFF);
    text_draw_center(small, PITCH, CW, CH, "已保存", -30, 2, 20, 16, 0xFFFF);
    text_draw_middle(small, PITCH, CW, CH, "无照片", 0, 0, CW, CH, 12, 0xFFFF);

    CHECK(text_draw(small, PITCH, CW, CH, "拍照", 0, 0, 12, 0xFFFF) == 24);
    CHECK(text_draw(small, PITCH, CW, CH, long_ascii, 0, 0, 12, 0xFFFF) ==
          text_width(long_ascii, 12));

    free(small);
}

static void draw_screen(uint16_t *fb)
{
    int w;

    fill_pattern(fb, W, W, H);
    text_draw_center(fb, W, W, H, "设置", 0, 2, W, 16, rgb565(245, 245, 245));

    draw_label_value(fb, "拍照", "", 8, 30, 16, rgb565(255, 193, 7),
                     rgb565(245, 245, 245));
    draw_label_value(fb, "视频规格", " 1080P30", 8, 52, 12,
                     rgb565(255, 193, 7), rgb565(245, 245, 245));
    draw_label_value(fb, "剩余空间", " 12.3 GB", 8, 70, 12, rgb565(160, 160, 160),
                     rgb565(245, 245, 245));
    draw_label_value(fb, "已保存", " IMG_0001.JPG", 8, 88, 12,
                     rgb565(120, 220, 140), rgb565(160, 160, 160));
    draw_label_value(fb, "录像中", " 00:42", 8, 106, 16, rgb565(229, 57, 53),
                     rgb565(245, 245, 245));
    draw_label_value(fb, "剩余", " 87%", 220, 106, 12, rgb565(160, 160, 160),
                     rgb565(255, 193, 7));

    /* mixed CJK/Latin sprites: baseline of both fonts must line up */
    text_draw(fb, W, W, H, "ov5647 未知", 200, 52, 12, rgb565(160, 160, 160));
    text_draw(fb, W, W, H, "长按 3 秒退出", 190, 70, 12, rgb565(160, 160, 160));
    text_draw(fb, W, W, H, "视频分辨率 / 帧率", 140, 88, 12,
              rgb565(160, 160, 160));

    /* boxed centre/middle helpers, the way buttons will use them */
    w = text_width("删除？", 16);
    CHECK(w > 0);
    text_draw_middle(fb, W, W, H, "删除？", 10, 128, 100, 24, 16,
                     rgb565(245, 245, 245));
    text_draw_middle(fb, W, W, H, "取消", 10, 154, 100, 20, 12,
                     rgb565(160, 160, 160));
    text_draw_center(fb, W, W, H, "确定", 210, 128, 100, 16, rgb565(255, 193, 7));
    text_draw_middle(fb, W, W, H, "无照片", 210, 154, 100, 20, 12,
                     rgb565(110, 110, 110));

    /* ASCII strip, all printable glyphs at both sizes */
    {
        char line[96];
        int cp;

        for (cp = 0x20; cp <= 0x7E; cp++)
            line[cp - 0x20] = (char)cp;
        line[95] = 0;
        text_draw(fb, W, W, H, line, 4, 172, 12, rgb565(200, 200, 200));
        text_draw(fb, W, W, H, line, 4, 190, 16, rgb565(200, 200, 200));
    }
    text_draw(fb, W, W, H, "5MP 30fps 99% ./_-", 4, 214, 12,
              rgb565(255, 193, 7));

    /* deliberately clipped: left of 0, right of the panel, below the panel */
    text_draw(fb, W, W, H, "录像中 12:34", -18, 226, 16, rgb565(229, 57, 53));
    text_draw(fb, W, W, H, "0123456789ABCDEF", 280, 228, 12,
              rgb565(160, 160, 160));
    text_draw(fb, W, W, H, "拍照", 300, -8, 16, rgb565(245, 245, 245));
}

int main(void)
{
    static uint16_t fb[W * H];
    size_t i;
    int drawn = 0;

    test_metrics();

    memset(fb, 0, sizeof(fb));
    CHECK(text_draw(fb, W, W, H, "拍照", 40, 100, 16, 0xFFFF) == 32);
    for (i = 0; i < W * H; i++) {
        if (fb[i] != 0)
            drawn++;
    }
    CHECK(drawn > 100);

    test_draw_and_clip();
    draw_screen(fb);
    CHECK(write_ppm(PPM_PATH, fb, W, W, H) == 0);

    printf("text_test: sprites=%s, line heights 12=%d 16=%d\n",
           text_lookup("拍照", 12) != NULL ? "ok" : "missing",
           text_line_height(12), text_line_height(16));
    printf("text_test: widths 拍照=%d/12 %d/16, ASCII AB12=%d/12\n",
           text_width("拍照", 12), text_width("拍照", 16),
           text_width("AB12", 12));
    printf("text_test: wrote %s\n", PPM_PATH);
    if (failures != 0) {
        fprintf(stderr, "text_test: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("text_test: all checks passed\n");
    return 0;
}

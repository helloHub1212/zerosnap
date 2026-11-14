#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fb.h"
#include "../src/gfx.h"

static void write_ppm(const Fb *fb, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("fopen");
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", fb->w, fb->h);
    for (int y = 0; y < fb->h; ++y) {
        const uint16_t *row = fb->back + (size_t)y * fb->w;
        for (int x = 0; x < fb->w; ++x) {
            uint16_t p = row[x];
            unsigned char rgb[3];
            rgb[0] = (unsigned char)(((p >> 11) & 0x1F) * 255 / 31);
            rgb[1] = (unsigned char)(((p >> 5) & 0x3F) * 255 / 63);
            rgb[2] = (unsigned char)((p & 0x1F) * 255 / 31);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("wrote %s\n", path);
}

int main(int argc, char **argv) {
    Fb fb;
    Gfx g;
    const char *out = argc > 1 ? argv[1] : "/tmp/opencode/gfx_test.ppm";
    const char *dev = getenv("CAMERA_FB");
    int failures = 0;

    if (fb_open(&fb, dev ? dev : "/tmp/opencode/fb.img") != 0) {
        perror("fb_open");
        return 1;
    }

    gfx_init(&g, fb.back, fb.w, fb.w, fb.h);

    /* Background + right-hand control column. */
    gfx_clear(&g, gfx_rgb565(0, 0, 0));
    gfx_fill(&g, 256, 0, 64, 240, gfx_rgb565(12, 12, 12));
    gfx_vline(&g, 255, 0, 240, gfx_rgb565(40, 40, 40));

    /* Preview stand-in: grey gradient with a colour bar. */
    for (int y = 0; y < 240; ++y) {
        int v = 40 + y * 120 / 240;
        gfx_hline(&g, 0, y, 256, gfx_rgb565(v, v, v));
    }
    for (int i = 0; i < 6; ++i) {
        gfx_fill(&g, 8 + i * 40, 8, 36, 24,
                 gfx_rgb565(i & 1 ? 220 : 30, i & 2 ? 200 : 30,
                            i & 4 ? 190 : 30));
    }

    /* Rounded chips, circles and rings. */
    gfx_fill_round(&g, 8, 200, 72, 28, 8, gfx_rgb565(30, 90, 160));
    gfx_fill_round(&g, 88, 200, 72, 28, 8, gfx_rgb565(160, 60, 30));
    gfx_fill_circle(&g, 224, 214, 16, gfx_rgb565(220, 30, 40));
    gfx_circle(&g, 224, 214, 20, gfx_rgb565(255, 255, 255));
    gfx_circle(&g, 224, 214, 21, gfx_rgb565(255, 255, 255));

    /* Icons column: gear-ish, gallery thumb, shutter. */
    for (int i = 0; i < 8; ++i) {
        double a = i * 3.14159265 / 4.0;
        gfx_line(&g, 288 + (int)(cos(a) * 9), 40 + (int)(sin(a) * 9),
                 288 + (int)(cos(a) * 15), 40 + (int)(sin(a) * 15),
                 gfx_rgb565(235, 235, 235));
    }
    gfx_circle(&g, 288, 40, 7, gfx_rgb565(235, 235, 235));

    gfx_frame(&g, 274, 74, 28, 22, gfx_rgb565(230, 230, 230));
    gfx_fill(&g, 280, 90, 8, 4, gfx_rgb565(230, 230, 230));
    gfx_fill_circle(&g, 282, 80, 2, gfx_rgb565(230, 230, 230));

    gfx_circle(&g, 288, 190, 22, gfx_rgb565(240, 240, 240));
    gfx_fill_circle(&g, 288, 190, 17, gfx_rgb565(200, 40, 40));

    /* Alpha overlays near the top. */
    gfx_fill_alpha(&g, 0, 0, 256, 20, gfx_rgb565(0, 0, 0), 130);
    gfx_fill_alpha(&g, 0, 220, 256, 20, gfx_rgb565(0, 0, 0), 130);

    /* Scaled blit of a synthetic thumbnail. */
    {
        static uint16_t thumb[48 * 36];
        for (int y = 0; y < 36; ++y) {
            for (int x = 0; x < 48; ++x) {
                thumb[y * 48 + x] =
                    gfx_rgb565(x * 5, y * 7, (x ^ y) * 4 & 0xFF);
            }
        }
        gfx_blit565_scaled(&g, thumb, 48, 48, 36, 20, 126, 60, 45);
        /* Verify the scaled output is not blank. */
        {
            uint16_t px = fb.back[(size_t)130 * fb.w + 30];
            if (px == 0) {
                printf("FAIL: scaled blit wrote black\n");
                failures++;
            }
        }
    }

    /* Clipping: a big rect must not escape its clip window. */
    gfx_clip(&g, 100, 100, 20, 10);
    gfx_fill(&g, 0, 0, 320, 240, gfx_rgb565(255, 0, 255));
    gfx_reset_clip(&g, fb.w, fb.h);
    {
        uint16_t inside = fb.back[(size_t)102 * fb.w + 102];
        uint16_t outside = fb.back[(size_t)98 * fb.w + 98];
        if (inside == outside) {
            printf("FAIL: clip did not take effect\n");
            failures++;
        }
    }

    /* fit / cover maths. */
    {
        Rect f = gfx_fit(640, 480, 320, 180);
        Rect c = gfx_cover(640, 480, 320, 180);
        if (f.h != 180 || f.w != 240) {
            printf("FAIL: fit 640x480 -> 320x180 gave %dx%d\n", f.w, f.h);
            failures++;
        }
        if (c.w != 320 || c.h != 240) {
            printf("FAIL: cover 640x480 -> 320x180 gave %dx%d\n", c.w, c.h);
            failures++;
        }
    }

    fb_present(&fb);
    write_ppm(&fb, out);

    fb_close(&fb);
    if (failures) {
        printf("gfx_test: %d FAILURES\n", failures);
        return 1;
    }
    printf("gfx_test: PASS\n");
    return 0;
}

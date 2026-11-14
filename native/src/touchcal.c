#include "touchcal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TOUCHCAL_FALLBACK "/tmp/zerosnap-touch.conf"

static int map_axis(int raw, int rmin, int rmax, int screen) {
    long long num, den, value;
    if (screen < 1) {
        screen = 1;
    }
    if (rmax <= rmin) {
        if (raw < 0) return 0;
        if (raw > screen - 1) return screen - 1;
        return raw;
    }
    num = ((long long)raw - (long long)rmin) * (long long)(screen - 1);
    den = (long long)rmax - (long long)rmin;
    value = num / den;
    if (value < 0) value = 0;
    if (value > screen - 1) value = screen - 1;
    return (int)value;
}

/* Mirrors input.c's map_point() so the solver and the reader agree. */
static void map_point(int raw_x, int raw_y, int x_min, int x_max, int y_min,
                      int y_max, int screen_w, int screen_h, int swap_xy,
                      int invert_x, int invert_y, int *out_x, int *out_y) {
    int rx = raw_x;
    int ry = raw_y;
    int rmin_x = x_min;
    int rmax_x = x_max;
    int rmin_y = y_min;
    int rmax_y = y_max;
    int mx, my;

    if (swap_xy) {
        int t;
        t = rx; rx = ry; ry = t;
        t = rmin_x; rmin_x = rmin_y; rmin_y = t;
        t = rmax_x; rmax_x = rmax_y; rmax_y = t;
    }
    mx = map_axis(rx, rmin_x, rmax_x, screen_w);
    my = map_axis(ry, rmin_y, rmax_y, screen_h);
    if (invert_x) mx = screen_w - 1 - mx;
    if (invert_y) my = screen_h - 1 - my;
    *out_x = mx;
    *out_y = my;
}

int touchcal_solve(int rx1, int ry1, int rx2, int ry2, int sx1, int sy1,
                   int sx2, int sy2, int x_min, int x_max, int y_min,
                   int y_max, int *swap_xy, int *invert_x, int *invert_y) {
    int best = -1;
    long best_score = 0;

    /* The screen size is implied by the calibration points: the caller asks
     * for the corners, so use the largest coordinate it gave us. */
    int screen_w = (sx1 > sx2 ? sx1 : sx2) + 1;
    int screen_h = (sy1 > sy2 ? sy1 : sy2) + 1;

    if (screen_w < 32 || screen_h < 32) {
        return 0;
    }

    for (int swap = 0; swap < 2; ++swap) {
        for (int ix = 0; ix < 2; ++ix) {
            for (int iy = 0; iy < 2; ++iy) {
                int mx1, my1, mx2, my2;
                long dx1, dy1, dx2, dy2, score;
                int idx = (swap << 2) | (ix << 1) | iy;

                map_point(rx1, ry1, x_min, x_max, y_min, y_max, screen_w,
                          screen_h, swap, ix, iy, &mx1, &my1);
                map_point(rx2, ry2, x_min, x_max, y_min, y_max, screen_w,
                          screen_h, swap, ix, iy, &mx2, &my2);

                dx1 = mx1 - sx1;
                dy1 = my1 - sy1;
                dx2 = mx2 - sx2;
                dy2 = my2 - sy2;
                score = dx1 * dx1 + dy1 * dy1 + dx2 * dx2 + dy2 * dy2;
                if (best < 0 || score < best_score) {
                    best = idx;
                    best_score = score;
                }
            }
        }
    }

    if (best < 0) {
        return 0;
    }
    *swap_xy = (best >> 2) & 1;
    *invert_x = (best >> 1) & 1;
    *invert_y = best & 1;
    return 1;
}

static int parse_file(const char *path, int *swap_xy, int *invert_x,
                      int *invert_y) {
    FILE *file = fopen(path, "r");
    char line[128];
    int have = 0;

    if (file == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        int value;
        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        if (sscanf(line, " swap = %d", &value) == 1 ||
            sscanf(line, "swap=%d", &value) == 1 ||
            sscanf(line, "swap_xy=%d", &value) == 1) {
            *swap_xy = value ? 1 : 0;
            have = 1;
        } else if (sscanf(line, " invert_x = %d", &value) == 1 ||
                   sscanf(line, "invert_x=%d", &value) == 1) {
            *invert_x = value ? 1 : 0;
            have = 1;
        } else if (sscanf(line, " invert_y = %d", &value) == 1 ||
                   sscanf(line, "invert_y=%d", &value) == 1) {
            *invert_y = value ? 1 : 0;
            have = 1;
        }
    }
    fclose(file);
    return have;
}

int touchcal_load(int *swap_xy, int *invert_x, int *invert_y) {
    int s = 0, ix = 0, iy = 0;
    if (!parse_file(TOUCHCAL_PATH, &s, &ix, &iy) &&
        !parse_file(TOUCHCAL_FALLBACK, &s, &ix, &iy)) {
        return 0;
    }
    if (swap_xy) *swap_xy = s;
    if (invert_x) *invert_x = ix;
    if (invert_y) *invert_y = iy;
    return 1;
}

int touchcal_save(int swap_xy, int invert_x, int invert_y) {
    const char *paths[2] = {TOUCHCAL_PATH, TOUCHCAL_FALLBACK};

    for (int i = 0; i < 2; ++i) {
        FILE *file = fopen(paths[i], "w");
        if (file == NULL) {
            continue;
        }
        fprintf(file,
                "# 触摸校准（由相机界面写入，可手动编辑）\n"
                "# swap/invert 三个 0/1 决定触摸轴与屏幕轴的对应关系\n"
                "swap=%d\n"
                "invert_x=%d\n"
                "invert_y=%d\n",
                swap_xy ? 1 : 0, invert_x ? 1 : 0, invert_y ? 1 : 0);
        if (fclose(file) == 0) {
            return 0;
        }
    }
    return -1;
}

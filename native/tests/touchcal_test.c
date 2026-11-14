/*
 * touchcal_test.c - verify that the two-point calibration recovers the real
 * axis transform for all eight possibilities.
 */
#include <stdio.h>

#include "touchcal.h"

#define W 320
#define H 240

static int failures;

static int axis_round(int screen, int lo, int hi, int screen_max) {
    /* screen coordinate -> raw device units */
    return lo + (screen * (hi - lo) + (screen_max - 1) / 2) / (screen_max - 1);
}

/*
 * Produce the raw values the panel would report for a touch at (sx, sy) given
 * the transform the reader applies.  This is the inverse of map_point().
 */
static void raw_for_screen(int sx, int sy, int swap, int ix, int iy, int x_min,
                           int x_max, int y_min, int y_max, int *out_rx,
                           int *out_ry) {
    int mx = ix ? (W - 1 - sx) : sx;
    int my = iy ? (H - 1 - sy) : sy;
    int rx = axis_round(mx, x_min, x_max, W);
    int ry = axis_round(my, y_min, y_max, H);

    if (swap) {
        int t = rx;
        rx = ry;
        ry = t;
    }
    *out_rx = rx;
    *out_ry = ry;
}

static void check(int swap, int ix, int iy, int x_min, int x_max, int y_min,
                  int y_max) {
    int rx1, ry1, rx2, ry2;
    int got_swap = 9, got_ix = 9, got_iy = 9;

    raw_for_screen(40, 40, swap, ix, iy, x_min, x_max, y_min, y_max, &rx1, &ry1);
    raw_for_screen(280, 200, swap, ix, iy, x_min, x_max, y_min, y_max, &rx2, &ry2);

    if (!touchcal_solve(rx1, ry1, rx2, ry2, 40, 40, 280, 200, x_min, x_max,
                        y_min, y_max, &got_swap, &got_ix, &got_iy)) {
        printf("FAIL solve rejected swap=%d ix=%d iy=%d\n", swap, ix, iy);
        failures++;
        return;
    }
    if (got_swap != swap || got_ix != ix || got_iy != iy) {
        printf("FAIL swap=%d ix=%d iy=%d -> got %d/%d/%d "
               "(raw %d,%d and %d,%d)\n",
               swap, ix, iy, got_swap, got_ix, got_iy, rx1, ry1, rx2, ry2);
        failures++;
        return;
    }
    printf("PASS swap=%d ix=%d iy=%d\n", swap, ix, iy);
}

int main(void) {
    /* Common ADS7846 ranges. */
    check(0, 0, 0, 0, 4095, 0, 4095);
    check(0, 0, 1, 0, 4095, 0, 4095);
    check(0, 1, 0, 0, 4095, 0, 4095);
    check(0, 1, 1, 0, 4095, 0, 4095);
    check(1, 0, 0, 0, 4095, 0, 4095);
    check(1, 0, 1, 0, 4095, 0, 4095);
    check(1, 1, 0, 0, 4095, 0, 4095);
    check(1, 1, 1, 0, 4095, 0, 4095);

    /* A different, asymmetric range, as some panels report. */
    check(1, 0, 1, 200, 3900, 150, 3800);
    check(0, 1, 1, 120, 4000, 300, 3700);

    if (failures) {
        printf("touchcal_test: %d FAILURES\n", failures);
        return 1;
    }
    printf("touchcal_test: all transforms recovered\n");
    return 0;
}

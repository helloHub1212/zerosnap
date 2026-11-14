/*
 * touchcal.h - touchscreen orientation calibration.
 *
 * The display is rotated 270 degrees by the fbtft overlay, but the ADS7846
 * controller reports the panel's native axes.  Without a transform every tap
 * lands 90 degrees away from where the user pressed, which looks exactly like
 * "the buttons do not respond".
 *
 * Two taps are enough to pick one of the eight axis transforms.  The result is
 * stored on the FAT partition so it can also be edited from Windows.
 */
#ifndef TOUCHCAL_H
#define TOUCHCAL_H

/* Returns 1 and fills the transform when a config file exists. */
int touchcal_load(int *swap_xy, int *invert_x, int *invert_y);

/* Persist the transform.  Returns 0 on success. */
int touchcal_save(int swap_xy, int invert_x, int invert_y);

/*
 * Pick the transform that maps the two raw points onto the two screen points.
 * Returns 1 on success.  Falls back to identity when the ranges are unusable.
 */
int touchcal_solve(int raw_x1, int raw_y1, int raw_x2, int raw_y2, int scr_x1,
                   int scr_y1, int scr_x2, int scr_y2, int x_min, int x_max,
                   int y_min, int y_max, int *swap_xy, int *invert_x,
                   int *invert_y);

/* Where the config lives (first writable location wins on save). */
#define TOUCHCAL_PATH "/boot/firmware/zerosnap-touch.conf"

#endif /* TOUCHCAL_H */

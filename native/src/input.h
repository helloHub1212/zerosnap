/*
 * input.h - touchscreen input through evdev, no X11, no SDL.
 *
 * Supports both multitouch protocol B (ABS_MT_*) and single touch
 * (ABS_X/ABS_Y + BTN_TOUCH).  Coordinates are already mapped to screen
 * pixels, honouring the device's ABS ranges.
 */
#ifndef INPUT_H
#define INPUT_H

/*
 * Defined by <linux/input.h>.  input.h only needs a pointer to it, so keep
 * kernel headers out of the UI translation units.
 */
struct input_event;

enum {
    IN_NONE = 0,
    IN_DOWN = 1,
    IN_MOVE = 2,
    IN_UP = 3
};

typedef struct {
    int action;  /* IN_* */
    int x;       /* screen pixels, after the transform */
    int y;
    int raw_x;   /* device units before scaling/transform (for calibration) */
    int raw_y;
    int slot;    /* multitouch slot, 0 for single touch */
} InputEvent;

/*
 * Open a device.  dev may be NULL to auto-detect.  Auto-detection scans
 * /dev/input/event* and prefers, in order:
 *   1. names containing "ADS7846", "XPT2046", "touch", "TSC2007", "ft5x06",
 *      "goodix" or "GT911";
 *   2. any device reporting ABS_X, ABS_Y and BTN_TOUCH.
 * Devices whose name contains "vc4", "hdmi", "cec", "accel" or "xinput"
 * (the accelerometer, GPIO keys, HDMI CEC, ...) are rejected.
 * Returns 0 on success.
 */
int input_open(const char *dev);

void input_close(void);

/* File descriptor suitable for poll(); -1 when closed. */
int input_fd(void);

/*
 * Drain up to max events without blocking.  Returns the number written to
 * out, or -1 on a read error (the caller may then reopen).
 */
int input_read(InputEvent *out, int max);

/*
 * Screen size used to map ABS ranges.  Must be called after input_open() and
 * re-called whenever the framebuffer size is known.
 */
void input_set_screen(int w, int h);

/* Optional: force the ABS ranges when a device reports bogus values. */
void input_set_ranges(int x_min, int x_max, int y_min, int y_max);

/* Report the ranges in use; calibration needs them. */
void input_get_ranges(int *x_min, int *x_max, int *y_min, int *y_max);

/*
 * Runtime axis transform.  The Pi panel is mounted rotated by 270 degrees,
 * so the raw touch axes do not necessarily match the framebuffer axes; this
 * is a device-tree/board property, hence a runtime knob rather than an #if.
 *
 *   swap_xy  - exchange the raw X and Y axes (and their ABS ranges) before
 *              scaling to screen pixels
 *   invert_x - mirror the scaled X coordinate (sx = screen_w - 1 - sx)
 *   invert_y - mirror the scaled Y coordinate (sy = screen_h - 1 - sy)
 *
 * The transform is applied after ABS range scaling.  Default: all zero
 * (no transform).  Safe to call at any time.
 */
void input_set_transform(int swap_xy, int invert_x, int invert_y);

/* exposed for tests */
/*
 * Parse one raw evdev event and append any completed InputEvents to out.
 * MOVE events are coalesced until the following SYN_REPORT, so a whole
 * event burst yields at most one MOVE per active slot.  Returns the number
 * of events written (0 while more events are still pending in the burst).
 * Never fails.
 */
int input_feed(const struct input_event *ev, InputEvent *out, int max);

/* Reset the parser state (and pending raw queue) between test cases. */
void input_feed_reset(void);

/*
 * Tell the parser whether the device advertises BTN_TOUCH.  When it does
 * not (pressure-only resistive panels), ABS_PRESSURE 0/ > 0 drives the
 * press/release state.  Defaults to 1.
 */
void input_feed_set_btn_touch(int present);

#endif /* INPUT_H */

/*
 * input.c - evdev touchscreen input for the 320x240 Pi panel.
 *
 * Handles multitouch protocol B (ABS_MT_*) and single touch
 * (ABS_X/ABS_Y + BTN_TOUCH, optionally pressure-only).  Everything is
 * non-blocking and allocation free; coordinates are scaled to the screen
 * and optionally transformed (the panel is mounted rotated 270 degrees).
 */
#include "input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DEFAULT_SCREEN_W 320
#define DEFAULT_SCREEN_H 240
#define MT_MAX_SLOTS 10
#define EV_READ_BATCH 64

#define BITS_PER_LONG ((int)(8 * sizeof(unsigned long)))
#define NLONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)

/* ------------------------------------------------------------------ */
/* Device configuration (survives a parser reset).                     */

static int g_fd = -1;

static int g_screen_w = DEFAULT_SCREEN_W;
static int g_screen_h = DEFAULT_SCREEN_H;

/* A degenerate range (max <= min) means the device already reports pixels. */
static int g_x_min = 0, g_x_max = 0;
static int g_y_min = 0, g_y_max = 0;

static int g_swap_xy = 0;
static int g_invert_x = 0;
static int g_invert_y = 0;

static int g_btn_touch_present = 1;
static int g_have_mt = 0;

/* ------------------------------------------------------------------ */
/* Parser state.                                                       */

typedef struct {
    int active;       /* tracking id >= 0 */
    int down_pending; /* emit IN_DOWN at the next SYN */
    int moved;        /* position changed while down */
    int up_pending;   /* emit IN_UP at the next SYN */
    int have_pos;
    int raw_x, raw_y;
} SlotState;

typedef struct {
    SlotState slot[MT_MAX_SLOTS];
    int cur_slot;
    int use_mt;

    /* single touch / pressure state */
    int st_active;
    int st_down_pending;
    int st_moved;
    int st_up_pending;
    int st_have_pos;
    int st_raw_x, st_raw_y;
    int st_pressure;
} FeedState;

static FeedState g_fs;

/*
 * Raw events read ahead in input_read().  Kept across calls so a small
 * "max" never loses events in the middle of a SYN burst.
 */
static struct input_event g_evbuf[EV_READ_BATCH];
static struct input_event g_queue[EV_READ_BATCH * 2];
static int g_qhead, g_qtail;

/*
 * input_read() parses a whole SYN burst into g_stage, then copies as much as
 * fits into the caller's array.  Leftovers wait in g_pending for the next
 * call, so no events are lost or delayed by a small "max".
 */
#define MAX_SYN_BURST (2 * MT_MAX_SLOTS + 2)
static InputEvent g_stage[MAX_SYN_BURST];
static InputEvent g_pending[MAX_SYN_BURST];
static int g_pending_n, g_pending_pos;

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */

static int bit_isset(const unsigned long *bits, int bit) {
    return (int)((bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL);
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void lower_copy(char *dst, size_t cap, const char *src) {
    size_t i;

    for (i = 0; i + 1 < cap && src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    dst[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Coordinate mapping.                                                 */

static int map_axis(int raw, int rmin, int rmax, int screen) {
    long long num, den, v;

    if (screen < 1) screen = 1;

    /* Degenerate range: the device already reports screen pixels. */
    if (rmax <= rmin) return clamp_int(raw, 0, screen - 1);

    num = ((long long)raw - (long long)rmin) * (long long)(screen - 1);
    den = (long long)rmax - (long long)rmin;
    v = num / den;
    if (v < 0) v = 0;
    if (v > screen - 1) v = screen - 1;
    return (int)v;
}

static void map_point(int raw_x, int raw_y, int *sx, int *sy) {
    int rx = raw_x, ry = raw_y;
    int xmin = g_x_min, xmax = g_x_max;
    int ymin = g_y_min, ymax = g_y_max;
    int mx, my;

    if (g_swap_xy) {
        int t;

        t = rx; rx = ry; ry = t;
        t = xmin; xmin = ymin; ymin = t;
        t = xmax; xmax = ymax; ymax = t;
    }

    mx = map_axis(rx, xmin, xmax, g_screen_w);
    my = map_axis(ry, ymin, ymax, g_screen_h);

    if (g_invert_x) mx = (g_screen_w > 0 ? g_screen_w - 1 : 0) - mx;
    if (g_invert_y) my = (g_screen_h > 0 ? g_screen_h - 1 : 0) - my;

    *sx = mx;
    *sy = my;
}

/* ------------------------------------------------------------------ */
/* Event emission (called at SYN_REPORT).                              */

static void emit_event(InputEvent *out, int max, int *n, int action,
                       int x, int y, int slot, int raw_x, int raw_y) {
    if (*n >= max) return;
    out[*n].action = action;
    out[*n].x = x;
    out[*n].y = y;
    out[*n].raw_x = raw_x;
    out[*n].raw_y = raw_y;
    out[*n].slot = slot;
    (*n)++;
}

static void flush_mt(InputEvent *out, int max, int *n) {
    int i;

    for (i = 0; i < MT_MAX_SLOTS; i++) {
        SlotState *s = &g_fs.slot[i];
        int sx = 0, sy = 0;

        if (!s->down_pending && !s->up_pending && !(s->moved && s->active))
            continue;

        if (s->have_pos)
            map_point(s->raw_x, s->raw_y, &sx, &sy);
        else
            map_point(0, 0, &sx, &sy);

        if (s->down_pending) {
            emit_event(out, max, n, IN_DOWN, sx, sy, i, s->raw_x, s->raw_y);
            s->down_pending = 0;
            s->moved = 0;
        }
        if (s->up_pending) {
            emit_event(out, max, n, IN_UP, sx, sy, i, s->raw_x, s->raw_y);
            s->up_pending = 0;
            s->moved = 0;
        } else if (s->moved && s->active) {
            emit_event(out, max, n, IN_MOVE, sx, sy, i, s->raw_x, s->raw_y);
            s->moved = 0;
        }
    }
}

static void flush_single(InputEvent *out, int max, int *n) {
    int sx = 0, sy = 0;

    if (!g_fs.st_down_pending && !g_fs.st_up_pending &&
        !(g_fs.st_moved && g_fs.st_active))
        return;

    if (g_fs.st_have_pos)
        map_point(g_fs.st_raw_x, g_fs.st_raw_y, &sx, &sy);
    else
        map_point(0, 0, &sx, &sy);

    if (g_fs.st_down_pending) {
        emit_event(out, max, n, IN_DOWN, sx, sy, 0, g_fs.st_raw_x, g_fs.st_raw_y);
        g_fs.st_down_pending = 0;
        g_fs.st_moved = 0;
    }
    if (g_fs.st_up_pending) {
        emit_event(out, max, n, IN_UP, sx, sy, 0, g_fs.st_raw_x, g_fs.st_raw_y);
        g_fs.st_up_pending = 0;
        g_fs.st_moved = 0;
    } else if (g_fs.st_moved && g_fs.st_active) {
        emit_event(out, max, n, IN_MOVE, sx, sy, 0, g_fs.st_raw_x, g_fs.st_raw_y);
        g_fs.st_moved = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Parser.                                                             */

int input_feed(const struct input_event *ev, InputEvent *out, int max) {
    int n = 0;

    if (ev == NULL) return 0;
    if (max < 0 || out == NULL) max = 0;

    switch (ev->type) {
    case EV_ABS:
        switch (ev->code) {
        case ABS_MT_SLOT: {
            int slot = ev->value;

            g_fs.use_mt = 1;
            g_fs.cur_slot = clamp_int(slot, 0, MT_MAX_SLOTS - 1);
            break;
        }
        case ABS_MT_TRACKING_ID: {
            SlotState *s;

            g_fs.use_mt = 1;
            s = &g_fs.slot[g_fs.cur_slot];
            if (ev->value >= 0) {
                if (!s->active) {
                    s->active = 1;
                    s->down_pending = 1;
                    s->up_pending = 0;
                    s->moved = 0;
                }
            } else {
                if (s->active) {
                    s->active = 0;
                    s->up_pending = 1;
                    s->down_pending = 0;
                    s->moved = 0;
                }
            }
            break;
        }
        case ABS_MT_POSITION_X:
        case ABS_MT_POSITION_Y: {
            SlotState *s;

            g_fs.use_mt = 1;
            s = &g_fs.slot[g_fs.cur_slot];
            if (ev->code == ABS_MT_POSITION_X)
                s->raw_x = ev->value;
            else
                s->raw_y = ev->value;
            s->have_pos = 1;
            if (s->active && !s->down_pending) s->moved = 1;
            break;
        }
        case ABS_X:
        case ABS_Y: {
            if (ev->code == ABS_X)
                g_fs.st_raw_x = ev->value;
            else
                g_fs.st_raw_y = ev->value;
            g_fs.st_have_pos = 1;
            if (g_fs.st_active && !g_fs.st_down_pending) g_fs.st_moved = 1;
            break;
        }
        case ABS_PRESSURE: {
            g_fs.st_pressure = ev->value;
            if (!g_btn_touch_present) {
                if (ev->value > 0) {
                    if (!g_fs.st_active) {
                        g_fs.st_active = 1;
                        g_fs.st_down_pending = 1;
                        g_fs.st_moved = 0;
                    }
                } else {
                    if (g_fs.st_active) {
                        g_fs.st_active = 0;
                        g_fs.st_up_pending = 1;
                        g_fs.st_moved = 0;
                    }
                }
            }
            break;
        }
        default:
            break;
        }
        break;

    case EV_KEY:
        if (ev->code == BTN_TOUCH) {
            if (ev->value) {
                if (!g_fs.st_active) {
                    g_fs.st_active = 1;
                    g_fs.st_down_pending = 1;
                    g_fs.st_moved = 0;
                }
            } else {
                if (g_fs.st_active) {
                    g_fs.st_active = 0;
                    g_fs.st_up_pending = 1;
                    g_fs.st_moved = 0;
                }
            }
        }
        break;

    case EV_SYN:
        if (ev->code == SYN_REPORT) {
            if (g_fs.use_mt)
                flush_mt(out, max, &n);
            else
                flush_single(out, max, &n);
        }
        break;

    default:
        break;
    }

    return n;
}

void input_feed_reset(void) {
    memset(&g_fs, 0, sizeof g_fs);
    g_fs.use_mt = g_have_mt;
    g_qhead = 0;
    g_qtail = 0;
    g_pending_n = 0;
    g_pending_pos = 0;
}

void input_feed_set_btn_touch(int present) {
    g_btn_touch_present = present ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Device discovery and configuration.                                 */

typedef struct {
    char name[256];
    int has_abs_x, has_abs_y;
    int has_btn_touch;
    int has_mt_x, has_mt_y;
} DevCaps;

static int query_caps(int fd, DevCaps *c) {
    unsigned long absbits[NLONGS(ABS_CNT)];
    unsigned long keybits[NLONGS(KEY_CNT)];

    memset(c, 0, sizeof *c);
    if (ioctl(fd, EVIOCGNAME(sizeof c->name - 1), c->name) < 0)
        c->name[0] = '\0';
    c->name[sizeof c->name - 1] = '\0';

    memset(absbits, 0, sizeof absbits);
    memset(keybits, 0, sizeof keybits);
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) < 0)
        return -1;
    (void)ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);

    c->has_abs_x = bit_isset(absbits, ABS_X);
    c->has_abs_y = bit_isset(absbits, ABS_Y);
    c->has_btn_touch = bit_isset(keybits, BTN_TOUCH);
    c->has_mt_x = bit_isset(absbits, ABS_MT_POSITION_X);
    c->has_mt_y = bit_isset(absbits, ABS_MT_POSITION_Y);
    return 0;
}

static int name_rejected(const char *low) {
    static const char *const bad[] = {
        "vc4", "hdmi", "cec", "accel", "xinput"
    };
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (strstr(low, bad[i]) != NULL) return 1;
    }
    return 0;
}

static int name_is_touch(const char *low) {
    static const char *const good[] = {
        "ads7846", "xpt2046", "touch", "tsc2007",
        "ft5x06", "goodix", "gt911"
    };
    size_t i;

    for (i = 0; i < sizeof good / sizeof good[0]; i++) {
        if (strstr(low, good[i]) != NULL) return 1;
    }
    return 0;
}

static int open_dev(const char *path) {
    int fd, flags;

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    flags = fcntl(fd, F_GETFD);
    if (flags >= 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    return fd;
}

/*
 * Scan /dev/input/event*.  Priority 1 is a touch-like name; priority 2 is
 * an ABS_X + ABS_Y + BTN_TOUCH device.  Rejected names (accelerometer, HDMI
 * CEC, ...) never qualify.  On success path holds the chosen device.
 */
static int find_touch(char *path, size_t pathsz) {
    DIR *dir;
    struct dirent *de;
    char named[300] = "";
    char fallback[300] = "";

    dir = opendir("/dev/input");
    if (dir == NULL) return -1;

    while ((de = readdir(dir)) != NULL) {
        const char *p;
        char devpath[300];
        char low[256];
        DevCaps c;
        int fd;

        if (strncmp(de->d_name, "event", 5) != 0) continue;
        p = de->d_name + 5;
        if (*p < '0' || *p > '9') continue;
        while (*p >= '0' && *p <= '9') p++;
        if (*p != '\0') continue;

        snprintf(devpath, sizeof devpath, "/dev/input/%s", de->d_name);
        fd = open_dev(devpath);
        if (fd < 0) continue;

        if (query_caps(fd, &c) == 0) {
            lower_copy(low, sizeof low, c.name);
            if (!name_rejected(low)) {
                int has_abs = c.has_abs_x || c.has_mt_x;

                if (has_abs && name_is_touch(low)) {
                    if (named[0] == '\0')
                        snprintf(named, sizeof named, "%s", devpath);
                } else if (c.has_abs_x && c.has_abs_y && c.has_btn_touch) {
                    if (fallback[0] == '\0')
                        snprintf(fallback, sizeof fallback, "%s", devpath);
                }
            }
        }
        close(fd);
    }
    closedir(dir);

    if (named[0] != '\0') {
        snprintf(path, pathsz, "%s", named);
        return 0;
    }
    if (fallback[0] != '\0') {
        snprintf(path, pathsz, "%s", fallback);
        return 0;
    }
    errno = ENODEV;
    return -1;
}

static int abs_range(int fd, int code, int *min, int *max) {
    struct input_absinfo ai;

    memset(&ai, 0, sizeof ai);
    if (ioctl(fd, EVIOCGABS(code), &ai) < 0) return -1;
    *min = ai.minimum;
    *max = ai.maximum;
    return 0;
}

static void configure_device(int fd) {
    DevCaps c;
    int mn, mx;

    g_x_min = g_x_max = 0;
    g_y_min = g_y_max = 0;
    g_have_mt = 0;
    g_btn_touch_present = 1;

    if (query_caps(fd, &c) < 0) return;

    g_have_mt = c.has_mt_x || c.has_mt_y;
    g_btn_touch_present = c.has_btn_touch;

    if (c.has_abs_x && abs_range(fd, ABS_X, &mn, &mx) == 0 && mx > mn) {
        g_x_min = mn;
        g_x_max = mx;
    } else if (c.has_mt_x &&
               abs_range(fd, ABS_MT_POSITION_X, &mn, &mx) == 0 && mx > mn) {
        /* Some MT panels only expose the MT axes. */
        g_x_min = mn;
        g_x_max = mx;
    }

    if (c.has_abs_y && abs_range(fd, ABS_Y, &mn, &mx) == 0 && mx > mn) {
        g_y_min = mn;
        g_y_max = mx;
    } else if (c.has_mt_y &&
               abs_range(fd, ABS_MT_POSITION_Y, &mn, &mx) == 0 && mx > mn) {
        g_y_min = mn;
        g_y_max = mx;
    }

    /*
     * A few controllers report 0..0 for ABS_X/ABS_Y but a sane range on the
     * MT axes; prefer the usable range when that happens.
     */
    if (g_x_max <= g_x_min && c.has_mt_x &&
        abs_range(fd, ABS_MT_POSITION_X, &mn, &mx) == 0 && mx > mn) {
        g_x_min = mn;
        g_x_max = mx;
    }
    if (g_y_max <= g_y_min && c.has_mt_y &&
        abs_range(fd, ABS_MT_POSITION_Y, &mn, &mx) == 0 && mx > mn) {
        g_y_min = mn;
        g_y_max = mx;
    }
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */

int input_open(const char *dev) {
    char path[300];
    int fd;

    if (dev != NULL) {
        if (strlen(dev) >= sizeof path) {
            errno = ENAMETOOLONG;
            return -1;
        }
        snprintf(path, sizeof path, "%s", dev);
    } else {
        if (find_touch(path, sizeof path) < 0) return -1;
    }

    fd = open_dev(path);
    if (fd < 0) return -1;

    if (g_fd >= 0) input_close();
    g_fd = fd;
    g_qhead = g_qtail = 0;

    configure_device(fd);
    input_feed_reset();
    return 0;
}

void input_close(void) {
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    g_qhead = g_qtail = 0;
    g_have_mt = 0;
    g_btn_touch_present = 1;
    g_x_min = g_x_max = 0;
    g_y_min = g_y_max = 0;
    input_feed_reset();
}

int input_fd(void) {
    return g_fd;
}

int input_read(InputEvent *out, int max) {
    int n = 0;

    if (out == NULL || max <= 0) return 0;
    if (g_fd < 0) {
        errno = EBADF;
        return -1;
    }

    /* Hand back events saved from a previous, too-small call. */
    while (g_pending_pos < g_pending_n && n < max)
        out[n++] = g_pending[g_pending_pos++];
    if (g_pending_pos >= g_pending_n)
        g_pending_n = g_pending_pos = 0;
    if (n >= max) return n;

    for (;;) {
        if (g_qhead >= g_qtail) {
            ssize_t got;
            int cnt;

            got = read(g_fd, g_evbuf, sizeof g_evbuf);
            if (got < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                return -1;
            }
            if (got == 0) break;

            cnt = (int)(got / (ssize_t)sizeof(struct input_event));
            if (cnt <= 0) break;
            memcpy(g_queue, g_evbuf,
                   (size_t)cnt * sizeof(struct input_event));
            g_qhead = 0;
            g_qtail = cnt;
        }

        while (g_qhead < g_qtail && n < max) {
            struct input_event *e = &g_queue[g_qhead];
            int w;

            if (e->type == EV_SYN && e->code == SYN_REPORT) {
                int copy;

                w = input_feed(e, g_stage, MAX_SYN_BURST);
                g_qhead++;

                copy = w;
                if (copy > max - n) copy = max - n;
                if (copy > 0) {
                    memcpy(out + n, g_stage,
                           (size_t)copy * sizeof(InputEvent));
                    n += copy;
                }
                if (copy < w) {
                    memcpy(g_pending, g_stage + copy,
                           (size_t)(w - copy) * sizeof(InputEvent));
                    g_pending_n = w - copy;
                    g_pending_pos = 0;
                    return n;
                }
            } else {
                w = input_feed(e, out + n, max - n);
                g_qhead++;
                n += w;
            }
        }

        if (n >= max) break;
        if (g_qhead < g_qtail) break; /* out of output room */
        /* queue empty: loop and read again until EAGAIN */
    }

    return n;
}

void input_set_screen(int w, int h) {
    g_screen_w = w > 0 ? w : 1;
    g_screen_h = h > 0 ? h : 1;
}

void input_set_ranges(int x_min, int x_max, int y_min, int y_max) {
    if (x_max <= x_min) {
        g_x_min = 0;
        g_x_max = 0;
    } else {
        g_x_min = x_min;
        g_x_max = x_max;
    }

    if (y_max <= y_min) {
        g_y_min = 0;
        g_y_max = 0;
    } else {
        g_y_min = y_min;
        g_y_max = y_max;
    }
}

void input_get_ranges(int *x_min, int *x_max, int *y_min, int *y_max) {
    if (x_min) *x_min = g_x_min;
    if (x_max) *x_max = g_x_max;
    if (y_min) *y_min = g_y_min;
    if (y_max) *y_max = g_y_max;
}

void input_set_transform(int swap_xy, int invert_x, int invert_y) {
    g_swap_xy = swap_xy ? 1 : 0;
    g_invert_x = invert_x ? 1 : 0;
    g_invert_y = invert_y ? 1 : 0;
}

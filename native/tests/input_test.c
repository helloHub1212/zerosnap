/*
 * input_test.c - host-side tests for the evdev touch parser.
 *
 * Feeds synthetic struct input_event streams through input_feed() and checks
 * the emitted InputEvents.  Build and run with tests/run_input_test.sh.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/input.h>

#include "../src/input.h"

static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("    fail: %s (line %d)\n", #cond, __LINE__);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* Reset parser + fixed 320x240 screen, full 12-bit resistive range. */
static void setup(int btn_touch, int swap_xy, int invert_x, int invert_y) {
    input_set_screen(320, 240);
    input_set_ranges(0, 4095, 0, 4095);
    input_set_transform(swap_xy, invert_x, invert_y);
    input_feed_set_btn_touch(btn_touch);
    input_feed_reset();
}

static int feed(int type, int code, int value, InputEvent *out, int max) {
    struct input_event ev;

    memset(&ev, 0, sizeof ev);
    ev.type = (__u16)type;
    ev.code = (__u16)code;
    ev.value = value;
    return input_feed(&ev, out, max);
}

static int syn(InputEvent *out, int max) {
    return feed(EV_SYN, SYN_REPORT, 0, out, max);
}

/* ------------------------------------------------------------------ */

static void test_mt_basic(void) {
    InputEvent out[64];
    int n;

    setup(1, 0, 0, 0);

    CHECK(feed(EV_ABS, ABS_MT_SLOT, 0, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_TRACKING_ID, 1, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_X, 100, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_Y, 200, out, 64) == 0);

    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_DOWN);
        CHECK(out[0].x == 7);
        CHECK(out[0].y == 11);
        CHECK(out[0].slot == 0);
    }

    /* Three raw moves in one burst must coalesce to a single IN_MOVE. */
    CHECK(feed(EV_ABS, ABS_MT_POSITION_X, 500, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_Y, 600, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_X, 1000, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_Y, 1200, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_X, 2000, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_Y, 2000, out, 64) == 0);

    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_MOVE);
        CHECK(out[0].x == 155);
        CHECK(out[0].y == 116);
        CHECK(out[0].slot == 0);
    }

    CHECK(feed(EV_ABS, ABS_MT_TRACKING_ID, -1, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_UP);
        CHECK(out[0].x == 155);
        CHECK(out[0].y == 116);
        CHECK(out[0].slot == 0);
    }
}

static void test_mt_two_slots(void) {
    InputEvent out[64];
    int n;

    setup(1, 0, 0, 0);

    /* Slot 0 down. */
    CHECK(feed(EV_ABS, ABS_MT_SLOT, 0, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_TRACKING_ID, 10, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_X, 100, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_Y, 200, out, 64) == 0);

    /* Slot 1 down in the same frame. */
    CHECK(feed(EV_ABS, ABS_MT_SLOT, 1, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_TRACKING_ID, 11, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_X, 300, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_Y, 400, out, 64) == 0);

    n = syn(out, 64);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(out[0].action == IN_DOWN && out[0].slot == 0);
        CHECK(out[0].x == 7 && out[0].y == 11);
        CHECK(out[1].action == IN_DOWN && out[1].slot == 1);
        CHECK(out[1].x == 23 && out[1].y == 23);
    }

    /* Both slots move in one frame; emitted in slot order. */
    CHECK(feed(EV_ABS, ABS_MT_SLOT, 0, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_X, 500, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_Y, 600, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_SLOT, 1, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_X, 1000, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_Y, 1200, out, 64) == 0);

    n = syn(out, 64);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(out[0].action == IN_MOVE && out[0].slot == 0);
        CHECK(out[0].x == 38 && out[0].y == 35);
        CHECK(out[1].action == IN_MOVE && out[1].slot == 1);
        CHECK(out[1].x == 77 && out[1].y == 70);
    }

    /* Only slot 0 lifts. */
    CHECK(feed(EV_ABS, ABS_MT_SLOT, 0, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_TRACKING_ID, -1, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_UP && out[0].slot == 0);
        CHECK(out[0].x == 38 && out[0].y == 35);
    }

    /* Slot 1 keeps working. */
    CHECK(feed(EV_ABS, ABS_MT_SLOT, 1, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_X, 1500, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_MT_POSITION_Y, 1600, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_MOVE && out[0].slot == 1);
        CHECK(out[0].x == 116 && out[0].y == 93);
    }

    CHECK(feed(EV_ABS, ABS_MT_TRACKING_ID, -1, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_UP && out[0].slot == 1);
        CHECK(out[0].x == 116 && out[0].y == 93);
    }
}

static void test_single_touch(void) {
    InputEvent out[64];
    int n;

    setup(1, 0, 0, 0);

    /* Top-left corner. */
    CHECK(feed(EV_ABS, ABS_X, 0, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_Y, 0, out, 64) == 0);
    CHECK(feed(EV_KEY, BTN_TOUCH, 1, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_DOWN);
        CHECK(out[0].x == 0 && out[0].y == 0);
        CHECK(out[0].slot == 0);
    }

    /* Bottom-right corner. */
    CHECK(feed(EV_ABS, ABS_X, 4095, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_Y, 4095, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_MOVE);
        CHECK(out[0].x == 319 && out[0].y == 239);
    }

    CHECK(feed(EV_KEY, BTN_TOUCH, 0, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_UP);
        CHECK(out[0].x == 319 && out[0].y == 239);
    }

    /* Top-right corner. */
    CHECK(feed(EV_ABS, ABS_X, 4095, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_Y, 0, out, 64) == 0);
    CHECK(feed(EV_KEY, BTN_TOUCH, 1, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_DOWN);
        CHECK(out[0].x == 319 && out[0].y == 0);
    }
    CHECK(feed(EV_KEY, BTN_TOUCH, 0, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) CHECK(out[0].action == IN_UP);

    /* Bottom-left corner. */
    CHECK(feed(EV_ABS, ABS_X, 0, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_Y, 4095, out, 64) == 0);
    CHECK(feed(EV_KEY, BTN_TOUCH, 1, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) CHECK(out[0].x == 0 && out[0].y == 239);
    CHECK(feed(EV_KEY, BTN_TOUCH, 0, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) CHECK(out[0].action == IN_UP);
}

static void test_pressure_only(void) {
    InputEvent out[64];
    int n;

    /* No BTN_TOUCH: pressure drives press/release. */
    setup(0, 0, 0, 0);

    CHECK(feed(EV_ABS, ABS_X, 100, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_Y, 200, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_PRESSURE, 80, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_DOWN);
        CHECK(out[0].x == 7 && out[0].y == 11);
    }

    /* Pressure alone with no movement produces nothing. */
    CHECK(feed(EV_ABS, ABS_PRESSURE, 50, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 0);

    /* Moving while pressed produces one move. */
    CHECK(feed(EV_ABS, ABS_X, 2000, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_Y, 2000, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_MOVE);
        CHECK(out[0].x == 155 && out[0].y == 116);
    }

    /* Pressure 0 means lift. */
    CHECK(feed(EV_ABS, ABS_PRESSURE, 0, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_UP);
        CHECK(out[0].x == 155 && out[0].y == 116);
    }

    /* Repeated zero pressure is silent. */
    CHECK(feed(EV_ABS, ABS_PRESSURE, 0, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 0);
}

static void test_degenerate_range(void) {
    InputEvent out[64];
    int n;

    /* 0..0 means the controller already reports screen pixels. */
    setup(1, 0, 0, 0);
    input_set_ranges(0, 0, 0, 0);

    CHECK(feed(EV_ABS, ABS_X, 100, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_Y, 50, out, 64) == 0);
    CHECK(feed(EV_KEY, BTN_TOUCH, 1, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_DOWN);
        CHECK(out[0].x == 100 && out[0].y == 50);
    }

    /* Out-of-range values are clamped, not scaled. */
    CHECK(feed(EV_ABS, ABS_X, 500, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_Y, 300, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_MOVE);
        CHECK(out[0].x == 319 && out[0].y == 239);
    }

    CHECK(feed(EV_KEY, BTN_TOUCH, 0, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) CHECK(out[0].action == IN_UP);
}

static void test_transform(void) {
    InputEvent out[64];
    int n;

    setup(1, 1, 1, 1); /* swap_xy, invert_x, invert_y */

    /* Raw (0,0) becomes screen bottom-right after swap + both inversions. */
    CHECK(feed(EV_ABS, ABS_X, 0, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_Y, 0, out, 64) == 0);
    CHECK(feed(EV_KEY, BTN_TOUCH, 1, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_DOWN);
        CHECK(out[0].x == 319 && out[0].y == 239);
    }

    /* Raw (4095,4095) becomes screen top-left. */
    CHECK(feed(EV_ABS, ABS_X, 4095, out, 64) == 0);
    CHECK(feed(EV_ABS, ABS_Y, 4095, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_MOVE);
        CHECK(out[0].x == 0 && out[0].y == 0);
    }

    CHECK(feed(EV_KEY, BTN_TOUCH, 0, out, 64) == 0);
    n = syn(out, 64);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_UP);
        CHECK(out[0].x == 0 && out[0].y == 0);
    }
}

/* ------------------------------------------------------------------ */

/*
 * input_read() integration: a FIFO is a readable non-blocking fd, which is
 * all input_read() needs.  This exercises the real read path, EAGAIN
 * handling, SYN batching and the small-"max" staging buffer.  Raw ranges
 * are 0..0 here, so coordinates pass through unchanged.
 */
static void put_ev(struct input_event *ev, int type, int code, int value) {
    memset(ev, 0, sizeof *ev);
    ev->type = (__u16)type;
    ev->code = (__u16)code;
    ev->value = value;
}

static int write_all(int fd, const struct input_event *ev, int count) {
    const char *p = (const char *)ev;
    size_t left = (size_t)count * sizeof *ev;

    while (left > 0) {
        ssize_t w = write(fd, p, left);

        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    return 0;
}

static void test_input_read_fifo(void) {
    const char *tmp = getenv("TMPDIR");
    char path[256];
    struct input_event ev[16];
    InputEvent out[16];
    int wfd, n, i;

    if (tmp == NULL || tmp[0] == '\0') tmp = "/tmp";
    snprintf(path, sizeof path, "%s/camera-input-%ld.fifo",
             tmp, (long)getpid());
    unlink(path);
    CHECK(mkfifo(path, 0600) == 0);

    CHECK(input_open(path) == 0);
    if (input_fd() < 0) {
        unlink(path);
        return;
    }

    input_set_screen(320, 240);
    input_set_ranges(0, 0, 0, 0); /* already-scaled coordinates */
    input_set_transform(0, 0, 0);
    input_feed_reset();

    wfd = open(path, O_WRONLY | O_NONBLOCK);
    CHECK(wfd >= 0);
    if (wfd < 0) {
        input_close();
        unlink(path);
        return;
    }

    /* Nothing pending: EAGAIN must look like zero events. */
    CHECK(input_read(out, 16) == 0);

    /* Two slots go down in one SYN burst. */
    i = 0;
    put_ev(&ev[i++], EV_ABS, ABS_MT_SLOT, 0);
    put_ev(&ev[i++], EV_ABS, ABS_MT_TRACKING_ID, 1);
    put_ev(&ev[i++], EV_ABS, ABS_MT_POSITION_X, 10);
    put_ev(&ev[i++], EV_ABS, ABS_MT_POSITION_Y, 20);
    put_ev(&ev[i++], EV_ABS, ABS_MT_SLOT, 1);
    put_ev(&ev[i++], EV_ABS, ABS_MT_TRACKING_ID, 2);
    put_ev(&ev[i++], EV_ABS, ABS_MT_POSITION_X, 30);
    put_ev(&ev[i++], EV_ABS, ABS_MT_POSITION_Y, 40);
    put_ev(&ev[i++], EV_SYN, SYN_REPORT, 0);
    CHECK(write_all(wfd, ev, i) == 0);

    /* max=1 must return the first event and stash the second. */
    n = input_read(out, 1);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_DOWN);
        CHECK(out[0].slot == 0);
        CHECK(out[0].x == 10 && out[0].y == 20);
    }
    n = input_read(out, 16);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].action == IN_DOWN);
        CHECK(out[0].slot == 1);
        CHECK(out[0].x == 30 && out[0].y == 40);
    }

    /* Both slots move in one frame. */
    i = 0;
    put_ev(&ev[i++], EV_ABS, ABS_MT_SLOT, 0);
    put_ev(&ev[i++], EV_ABS, ABS_MT_POSITION_X, 50);
    put_ev(&ev[i++], EV_ABS, ABS_MT_POSITION_Y, 60);
    put_ev(&ev[i++], EV_ABS, ABS_MT_SLOT, 1);
    put_ev(&ev[i++], EV_ABS, ABS_MT_POSITION_X, 70);
    put_ev(&ev[i++], EV_ABS, ABS_MT_POSITION_Y, 80);
    put_ev(&ev[i++], EV_SYN, SYN_REPORT, 0);
    CHECK(write_all(wfd, ev, i) == 0);

    n = input_read(out, 16);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(out[0].action == IN_MOVE && out[0].slot == 0);
        CHECK(out[0].x == 50 && out[0].y == 60);
        CHECK(out[1].action == IN_MOVE && out[1].slot == 1);
        CHECK(out[1].x == 70 && out[1].y == 80);
    }

    /* Both slots lift. */
    i = 0;
    put_ev(&ev[i++], EV_ABS, ABS_MT_SLOT, 0);
    put_ev(&ev[i++], EV_ABS, ABS_MT_TRACKING_ID, -1);
    put_ev(&ev[i++], EV_ABS, ABS_MT_SLOT, 1);
    put_ev(&ev[i++], EV_ABS, ABS_MT_TRACKING_ID, -1);
    put_ev(&ev[i++], EV_SYN, SYN_REPORT, 0);
    CHECK(write_all(wfd, ev, i) == 0);

    n = input_read(out, 16);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(out[0].action == IN_UP && out[0].slot == 0);
        CHECK(out[1].action == IN_UP && out[1].slot == 1);
    }

    CHECK(input_read(out, 16) == 0);
    CHECK(input_read(NULL, 16) == 0);
    CHECK(input_read(out, 0) == 0);

    close(wfd);
    input_close();
    CHECK(input_fd() == -1);
    CHECK(unlink(path) == 0);
}

/* ------------------------------------------------------------------ */

typedef void (*TestFn)(void);

static int run_case(const char *name, TestFn fn) {
    int before = g_failures;

    fn();
    if (g_failures == before) {
        printf("PASS %s\n", name);
        return 0;
    }
    printf("FAIL %s\n", name);
    return 1;
}

int main(void) {
    int failed = 0;

    failed += run_case("mt_basic_coalesced", test_mt_basic);
    failed += run_case("mt_two_slots_interleaved", test_mt_two_slots);
    failed += run_case("single_touch_corners", test_single_touch);
    failed += run_case("pressure_only", test_pressure_only);
    failed += run_case("degenerate_range_passthrough", test_degenerate_range);
    failed += run_case("transform_swap_invert", test_transform);
    failed += run_case("input_read_fifo", test_input_read_fifo);

    if (failed != 0) {
        printf("%d test case(s) FAILED, %d assertion(s) failed\n",
               failed, g_failures);
        return 1;
    }
    printf("all 7 test cases passed\n");
    return 0;
}

/*
 * ui.c - the on-screen interface.
 *
 * Layout: a 256 px preview column on the left and a 64 px control rail on the
 * right.  Everything is drawn with gfx.c primitives; the only font is the
 * build-time atlas.  The visual language is deliberately quiet: near-black
 * surfaces, one warm accent, and a single red shutter.
 */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "media.h"
#include "text.h"
#include "touchcal.h"

/* ------------------------------------------------------------------ layout */

#define SCREEN_W 320
#define SCREEN_H 240
#define STATUS_H 34
#define SIDE_W 64
#define SIDE_X (SCREEN_W - SIDE_W)

typedef struct {
    int x, y, w, h;
} R;

static const R GEAR_RECT = {266, 12, 44, 44};
static const R GALLERY_RECT = {266, 70, 44, 44};
static const R SHUTTER_RECT = {258, 138, 60, 60};
static const R CHIP_RECT = {264, 206, 48, 28};
static const R BACK_RECT = {8, 8, 32, 32};

static const R PAGE_PREV = {92, 208, 40, 26};
static const R PAGE_NEXT = {188, 208, 40, 26};
static const R PHOTO_PREV = {92, 202, 44, 30};
static const R PHOTO_NEXT = {184, 202, 44, 30};

#define GRID_TOP 52
#define GRID_CELL_W 96
#define GRID_CELL_H 72
#define GRID_GUTTER 6
#define GRID_COLS 3
#define GRID_ROWS 2
#define GRID_PER_PAGE (GRID_COLS * GRID_ROWS)

#define SET_X 8
#define SET_Y 48
#define SET_W 304
#define SET_ROW_H 38

#define THUMB_W 36
#define THUMB_H 27

#define APP_VERSION "v1.0"

/* ----------------------------------------------------------------- colours */

static uint16_t C_BLACK(void) { return gfx_rgb565(0, 0, 0); }
static uint16_t C_WHITE(void) { return gfx_rgb565(255, 255, 255); }
static uint16_t C_BG(void) { return gfx_rgb565(9, 10, 13); }
static uint16_t C_RAIL(void) { return gfx_rgb565(12, 13, 17); }
static uint16_t C_SURFACE(void) { return gfx_rgb565(20, 22, 27); }
static uint16_t C_SURFACE_HI(void) { return gfx_rgb565(31, 34, 41); }
static uint16_t C_LINE(void) { return gfx_rgb565(41, 44, 52); }
static uint16_t C_LINE_SOFT(void) { return gfx_rgb565(27, 30, 36); }
static uint16_t C_TEXT(void) { return gfx_rgb565(242, 244, 247); }
static uint16_t C_TEXT2(void) { return gfx_rgb565(158, 164, 174); }
static uint16_t C_TEXT3(void) { return gfx_rgb565(96, 102, 112); }
static uint16_t C_ACCENT(void) { return gfx_rgb565(255, 191, 71); }
static uint16_t C_SHUTTER(void) { return gfx_rgb565(255, 77, 94); }
static uint16_t C_SHUTTER_DOWN(void) { return gfx_rgb565(212, 52, 70); }
static uint16_t C_GREEN(void) { return gfx_rgb565(74, 222, 128); }
static uint16_t C_BLUE(void) { return gfx_rgb565(96, 165, 250); }
static uint16_t C_PURPLE(void) { return gfx_rgb565(167, 139, 250); }
static uint16_t C_GRAY(void) { return gfx_rgb565(148, 163, 184); }

/* -------------------------------------------------------------------- state */

enum {
    HIT_NONE = 0,
    HIT_GEAR,
    HIT_GALLERY,
    HIT_SHUTTER,
    HIT_CHIP,
    HIT_BACK,
    HIT_CELL,
    HIT_PAGE_PREV,
    HIT_PAGE_NEXT,
    HIT_PHOTO_PREV,
    HIT_PHOTO_NEXT,
    HIT_TOUCH_ROW
};

enum {
    TOAST_INFO = 0,
    TOAST_OK,
    TOAST_ERR
};

struct Ui {
    Fb *fb;
    Gfx g;

    UiView view;
    CamShm st;
    int has_state;
    const uint8_t *frame;
    int frame_valid;
    int link_ready;

    uint32_t pending_cmd[8];
    uint32_t pending_arg[8];
    int pending_count;

    MediaList media;
    int media_loaded;
    int gallery_page;
    int gallery_sel;

    /* The gallery shows six previews per page.  Loading them from the FAT
     * partition every frame is far too slow, so each cell is decoded into a
     * 96x72 RGB565 tile once and reused. */
    uint16_t cell[GRID_PER_PAGE][GRID_CELL_W * GRID_CELL_H];
    int cell_valid[GRID_PER_PAGE];

    /* The rail shows the newest photo. */
    uint16_t thumb[THUMB_W * THUMB_H];
    int thumb_valid;
    double thumb_try_at;

    int press_active;
    int press_x, press_y;
    int press_moved;
    int press_id;
    int press_round;
    R press_rect;

    double toast_until;
    char toast[96];
    int toast_kind;

    uint32_t last_photo_count;
    int first_draw;

    int shutter_pending;
    double shutter_at;

    int calib;
    double calib_started;
    int calib_step;
    int calib_raw_x[2];
    int calib_raw_y[2];
};

static const char *media_dir(void) {
    const char *dir = getenv("CAMERA_MEDIA_DIR");
    return (dir && *dir) ? dir : "/boot/firmware/Camera";
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void enqueue(Ui *ui, uint32_t cmd, uint32_t arg) {
    if (ui->pending_count >= (int)(sizeof(ui->pending_cmd) / sizeof(ui->pending_cmd[0]))) {
        return;
    }
    ui->pending_cmd[ui->pending_count] = cmd;
    ui->pending_arg[ui->pending_count] = arg;
    ++ui->pending_count;
}

static void toast(Ui *ui, const char *text, int kind) {
    snprintf(ui->toast, sizeof(ui->toast), "%s", text);
    ui->toast_until = now_ms() + (kind == TOAST_ERR ? 3200.0 : 2000.0);
    ui->toast_kind = kind;
}

/* ------------------------------------------------------------- text helpers */

static int text_clip(Gfx *g, const char *s, int x, int y, int px, uint16_t color,
                     int cx, int cy, int cw, int ch) {
    uint16_t *base;
    if (cw <= 0 || ch <= 0 || x >= cx + cw || y >= cy + ch) {
        return 0;
    }
    base = g->buf + (size_t)cy * g->pitch + cx;
    return text_draw(base, g->pitch, cw, ch, s, x - cx, y - cy, px, color);
}

static int text_clip_center(Gfx *g, const char *s, int cx, int cy, int cw,
                            int ch, int px, uint16_t color, int vcenter) {
    int tw = text_width(s, px);
    int th = text_line_height(px);
    int x = cx + (cw - tw) / 2;
    int y = vcenter ? cy + (ch - th) / 2 : cy;
    return text_clip(g, s, x, y, px, color, cx, cy, cw, ch);
}

/*
 * Right-aligned "N 张".  The atlas stores whole labels, so a dynamic number
 * cannot be concatenated with the CJK unit; draw the ASCII count and the
 * pre-rendered unit as two sprites.
 */
static void text_count_unit(Gfx *g, unsigned count, int right_x, int y,
                            int px, uint16_t color, int cx, int cy, int cw,
                            int ch) {
    char buf[16];
    int uw = text_width("张", px);
    int nw;

    snprintf(buf, sizeof(buf), "%u", count);
    nw = text_width(buf, px);
    text_clip(g, buf, right_x - uw - nw - 2, y, px, color, cx, cy, cw, ch);
    text_clip(g, "张", right_x - uw, y, px, color, cx, cy, cw, ch);
}

/* ------------------------------------------------------------------ shapes */

/* A one-pixel hairline border around a rounded fill. */
static void panel(Gfx *g, int x, int y, int w, int h, int r, uint16_t fill) {
    if (w < 2 || h < 2) {
        return;
    }
    gfx_fill_round(g, x, y, w, h, r, C_LINE_SOFT());
    gfx_fill_round(g, x + 1, y + 1, w - 2, h - 2, r > 0 ? r - 1 : 0, fill);
}

/* Translucent pill used over a photo. */
static void glass_panel(Gfx *g, int x, int y, int w, int h, int r) {
    if (w < 2 || h < 2) {
        return;
    }
    gfx_fill_round_alpha(g, x, y, w, h, r, C_TEXT(), 26);
    gfx_fill_round_alpha(g, x + 1, y + 1, w - 2, h - 2, r - 1, C_BLACK(),
                         150);
}

static void draw_scrim(Gfx *g, int x, int y, int w, int h, int a0, int a1,
                       int bands) {
    if (h <= 0 || bands <= 0) {
        return;
    }
    for (int i = 0; i < bands; ++i) {
        int y0 = y + i * h / bands;
        int y1 = y + (i + 1) * h / bands;
        int a = bands == 1 ? a0 : a0 + (a1 - a0) * i / (bands - 1);
        if (a > 0 && y1 > y0) {
            gfx_fill_alpha(g, x, y0, w, y1 - y0, C_BLACK(), a);
        }
    }
}

/*
 * Paint the outside of a rounded rectangle with the colour behind it.  Used
 * after blitting a rectangular thumbnail so its corners match the card.
 */
static void cut_corners(Gfx *g, int x, int y, int w, int h, int r,
                        uint16_t color) {
    if (r <= 0) {
        return;
    }
    for (int dy = 0; dy < r; ++dy) {
        for (int dx = 0; dx < r; ++dx) {
            int ax = r - 1 - dx;
            int ay = r - 1 - dy;
            if (ax * ax + ay * ay <= r * r) {
                continue;
            }
            gfx_fill(g, x + dx, y + dy, 1, 1, color);
            gfx_fill(g, x + w - 1 - dx, y + dy, 1, 1, color);
            gfx_fill(g, x + dx, y + h - 1 - dy, 1, 1, color);
            gfx_fill(g, x + w - 1 - dx, y + h - 1 - dy, 1, 1, color);
        }
    }
}

/* -------------------------------------------------------------------- icons */

static void icon_back(Gfx *g, int cx, int cy, int s, uint16_t color) {
    gfx_line(g, cx + s / 2, cy - s / 2, cx - s / 2, cy, color);
    gfx_line(g, cx - s / 2, cy, cx + s / 2, cy + s / 2, color);
    gfx_line(g, cx + s / 2, cy - s / 2 + 1, cx - s / 2 + 1, cy + 1, color);
    gfx_line(g, cx - s / 2 + 1, cy + 1, cx + s / 2, cy + s / 2 + 1, color);
}

static void icon_chevron(Gfx *g, int cx, int cy, int s, int right,
                         uint16_t color) {
    int dir = right ? 1 : -1;
    gfx_line(g, cx - dir * s / 2, cy - s / 2, cx + dir * s / 2, cy, color);
    gfx_line(g, cx + dir * s / 2, cy, cx - dir * s / 2, cy + s / 2, color);
}

/* Three sliders: the modern replacement for a gear at this size. */
static void icon_sliders(Gfx *g, int cx, int cy, int size, uint16_t color) {
    int x0 = cx - size / 2;
    int rows[3] = {cy - 6, cy, cy + 6};
    int knob[3] = {x0 + size - 5, x0 + size / 3, x0 + size / 2 + 2};

    for (int i = 0; i < 3; ++i) {
        gfx_hline(g, x0, rows[i], size, color);
        gfx_fill_circle(g, knob[i], rows[i], 2, color);
    }
}

static void icon_gallery(Gfx *g, int x, int y, int w, int h, uint16_t color) {
    gfx_frame(g, x, y, w, h, color);
    gfx_fill_circle(g, x + w - 6, y + 5, 2, color);
    gfx_line(g, x + 3, y + h - 4, x + w / 2 - 1, y + h / 2, color);
    gfx_line(g, x + w / 2 - 1, y + h / 2, x + w - 3, y + h - 4, color);
}

/* Larger line-art camera for the empty gallery and the boot screen. */
static void icon_camera_outline(Gfx *g, int cx, int cy, int s, uint16_t color) {
    int w = s;
    int h = s * 3 / 4;

    gfx_frame(g, cx - w / 2, cy - h / 2 + 2, w, h, color);
    gfx_fill(g, cx - s / 8, cy - h / 2 - 1, s / 4, 3, color);
    gfx_circle(g, cx, cy + 1, s / 4, color);
    gfx_fill(g, cx + w / 2 - 5, cy - h / 2 + 5, 2, 2, color);
}

static void icon_camera_small(Gfx *g, int cx, int cy, uint16_t color) {
    gfx_frame(g, cx - 7, cy - 4, 14, 10, color);
    gfx_fill(g, cx - 3, cy - 6, 6, 2, color);
    gfx_circle(g, cx, cy + 1, 3, color);
    gfx_fill(g, cx + 5, cy - 2, 1, 1, color);
}

static void icon_sd(Gfx *g, int cx, int cy, uint16_t color) {
    gfx_frame(g, cx - 6, cy - 7, 12, 14, color);
    gfx_fill(g, cx - 6, cy - 7, 4, 3, color);
    for (int i = 0; i < 3; ++i) {
        gfx_vline(g, cx - 3 + i * 3, cy - 6, 3, color);
    }
    gfx_hline(g, cx - 4, cy + 5, 8, color);
}

static void icon_battery(Gfx *g, int cx, int cy, int level, uint16_t color) {
    int fill = level * 8 / 100;

    gfx_frame(g, cx - 8, cy - 4, 14, 8, color);
    gfx_fill(g, cx + 6, cy - 2, 2, 4, color);
    if (fill > 0) {
        gfx_fill(g, cx - 6, cy - 2, fill, 4, color);
    }
}

static void icon_crosshair(Gfx *g, int cx, int cy, uint16_t color) {
    gfx_circle(g, cx, cy, 6, color);
    gfx_hline(g, cx - 9, cy, 3, color);
    gfx_hline(g, cx + 7, cy, 3, color);
    gfx_vline(g, cx, cy - 9, 3, color);
    gfx_vline(g, cx, cy + 7, 3, color);
    gfx_fill(g, cx, cy, 1, 1, color);
}

static void icon_info(Gfx *g, int cx, int cy, uint16_t color) {
    gfx_circle(g, cx, cy, 7, color);
    gfx_fill(g, cx, cy - 4, 2, 2, color);
    gfx_fill(g, cx, cy - 1, 2, 5, color);
}

static void icon_check(Gfx *g, int cx, int cy, int s, uint16_t color) {
    gfx_line(g, cx - s, cy, cx - s / 3, cy + s * 2 / 3, color);
    gfx_line(g, cx - s / 3, cy + s * 2 / 3, cx + s, cy - s * 2 / 3, color);
    gfx_line(g, cx - s, cy + 1, cx - s / 3, cy + s * 2 / 3 + 1, color);
    gfx_line(g, cx - s / 3, cy + s * 2 / 3 + 1, cx + s, cy - s * 2 / 3 + 1,
             color);
}

/* ------------------------------------------------------------------ status */

static void draw_status_strip(Ui *ui, const CamShm *st) {
    Gfx *g = &ui->g;
    char buf[48];
    int th = text_line_height(12);

    draw_scrim(g, 0, 0, SIDE_X, STATUS_H, 150, 0, 7);
    draw_scrim(g, 0, SCREEN_H - 30, SIDE_X, 30, 0, 130, 6);

    gfx_fill_circle(g, 12, 17, 3, C_ACCENT());
    text_clip(g, "拍照", 21, 16 - th / 2, 12, C_TEXT(), 0, 0, SIDE_X,
              STATUS_H);

    if (st->free_mb > 0) {
        snprintf(buf, sizeof(buf), "%u.%uG", (unsigned)(st->free_mb / 1024u),
                 (unsigned)((st->free_mb % 1024u) * 10u / 1024u));
        text_clip(g, buf, SIDE_X - 10 - text_width(buf, 12), 16 - th / 2, 12,
                  C_TEXT2(), 0, 0, SIDE_X, STATUS_H);
    }

    text_clip(g, "2592x1944", 10, SCREEN_H - 21, 12, C_TEXT3(), 0,
              SCREEN_H - 30, SIDE_X, 30);

    if (st->photo_count > 0) {
        text_count_unit(g, (unsigned)st->photo_count, SIDE_X - 10,
                        SCREEN_H - 21, 12, C_TEXT3(), 0, SCREEN_H - 30, SIDE_X,
                        30);
    }

    if (st->flags & CAM_FLAG_ERROR) {
        gfx_fill_alpha(g, 0, STATUS_H, SIDE_X, 18, C_SHUTTER(), 190);
        text_clip_center(g, "操作失败", 0, STATUS_H, SIDE_X, 18, 12, C_WHITE(),
                         1);
    }
}

/* --------------------------------------------------------------- viewfinder */

static void draw_latest_thumb(Ui *ui, int x, int y) {
    Gfx *g = &ui->g;

    panel(g, x - 1, y - 1, THUMB_W + 2, THUMB_H + 2, 6, C_SURFACE());
    gfx_clip(g, x, y, THUMB_W, THUMB_H);
    gfx_blit565(g, ui->thumb, THUMB_W, THUMB_W, THUMB_H, x, y);
    gfx_reset_clip(g, SCREEN_W, SCREEN_H);
    cut_corners(g, x, y, THUMB_W, THUMB_H, 5, C_LINE_SOFT());
}

static void draw_shutter(Ui *ui, int busy, int pressed) {
    Gfx *g = &ui->g;
    int cx = SHUTTER_RECT.x + SHUTTER_RECT.w / 2;
    int cy = SHUTTER_RECT.y + SHUTTER_RECT.h / 2;

    gfx_circle(g, cx, cy, 29, C_SURFACE_HI());

    if (busy) {
        int head = (int)(now_ms() / 90.0) % 8;
        gfx_circle(g, cx, cy, 25, C_LINE());
        gfx_circle(g, cx, cy, 23, C_LINE());
        for (int i = 0; i < 8; ++i) {
            static const int dx[8] = {0, 10, 14, 10, 0, -10, -14, -10};
            static const int dy[8] = {-14, -10, 0, 10, 14, 10, 0, -10};
            gfx_fill_circle(g, cx + dx[i], cy + dy[i], i == head ? 3 : 2,
                            i == head ? C_ACCENT() : C_TEXT3());
        }
    } else {
        gfx_fill_circle(g, cx, cy, 27, C_TEXT());
        gfx_fill_circle(g, cx, cy, 23, C_RAIL());
        gfx_fill_circle(g, cx, cy, pressed ? 18 : 20,
                        pressed ? C_SHUTTER_DOWN() : C_SHUTTER());
        gfx_circle(g, cx, cy, pressed ? 17 : 19, C_SHUTTER_DOWN());
    }
}

static void draw_rail(Ui *ui, const CamShm *st) {
    Gfx *g = &ui->g;
    int busy = (st->flags & CAM_FLAG_BUSY) ? 1 : 0;
    int pressed = ui->press_active && ui->press_id == HIT_SHUTTER;

    gfx_fill(g, SIDE_X, 0, SIDE_W, SCREEN_H, C_RAIL());
    gfx_vline(g, SIDE_X, 0, SCREEN_H, C_LINE_SOFT());

    icon_sliders(g, 288, 34, 18, C_TEXT());

    if (ui->thumb_valid) {
        draw_latest_thumb(ui, 270, 79);
    } else {
        icon_gallery(g, 276, 82, 24, 20, C_TEXT());
    }

    draw_shutter(ui, busy, pressed);

    panel(g, CHIP_RECT.x, CHIP_RECT.y, CHIP_RECT.w, CHIP_RECT.h, 9,
          C_SURFACE());
    text_clip_center(g, "5MP", CHIP_RECT.x, CHIP_RECT.y, CHIP_RECT.w,
                     CHIP_RECT.h, 12, C_ACCENT(), 1);
}

static void draw_boot_message(Ui *ui, const char *message) {
    Gfx *g = &ui->g;
    int dot = (int)(now_ms() / 380.0) % 4;

    icon_camera_outline(g, SIDE_X / 2, 88, 46, C_TEXT3());
    text_clip_center(g, message, 0, 116, SIDE_X, 22, 16, C_TEXT(), 1);
    for (int i = 0; i < 3; ++i) {
        gfx_fill_circle(g, SIDE_X / 2 - 7 + i * 7, 146, 2,
                        i == dot ? C_ACCENT() : C_TEXT3());
    }
}

static void draw_viewfinder(Ui *ui, const CamShm *st) {
    Gfx *g = &ui->g;

    if (ui->frame_valid && ui->frame) {
        gfx_clip(g, 0, 0, SIDE_X, SCREEN_H);
        gfx_blit565(g, (const uint16_t *)ui->frame, SCREEN_W, SCREEN_W,
                    SCREEN_H, 0, 0);
        gfx_reset_clip(g, SCREEN_W, SCREEN_H);
    } else {
        const char *message;
        gfx_fill(g, 0, 0, SIDE_X, SCREEN_H, gfx_rgb565(16, 18, 22));
        if (!ui->link_ready) {
            message = "启动中";
        } else if (!(st->flags & CAM_FLAG_CAM_READY)) {
            message = "相机未启动";
        } else {
            message = "无信号";
        }
        draw_boot_message(ui, message);
    }

    draw_status_strip(ui, st);
    draw_rail(ui, st);
}

/* ------------------------------------------------------------------ gallery */

static R grid_rect(int index) {
    int slot = index % GRID_PER_PAGE;
    int total_w = GRID_COLS * GRID_CELL_W + (GRID_COLS - 1) * GRID_GUTTER;
    int x0 = (SCREEN_W - total_w) / 2;
    R r;

    r.x = x0 + (slot % GRID_COLS) * (GRID_CELL_W + GRID_GUTTER);
    r.y = GRID_TOP + (slot / GRID_COLS) * (GRID_CELL_H + GRID_GUTTER);
    r.w = GRID_CELL_W;
    r.h = GRID_CELL_H;
    return r;
}

static int gallery_pages(const Ui *ui) {
    if (ui->media.count <= 0) {
        return 1;
    }
    return (ui->media.count + GRID_PER_PAGE - 1) / GRID_PER_PAGE;
}

static void cell_reset(Ui *ui) {
    memset(ui->cell_valid, 0, sizeof(ui->cell_valid));
}

/* Decode one preview into its 96x72 tile, once per page visit. */
static const uint16_t *cell_image(Ui *ui, int index) {
    int slot = index % GRID_PER_PAGE;
    int tw = 0;
    int th = 0;
    const uint16_t *preview;

    if (slot < 0 || slot >= GRID_PER_PAGE || index < 0 ||
        index >= ui->media.count) {
        return NULL;
    }
    if (ui->cell_valid[slot]) {
        return ui->cell[slot];
    }

    preview = media_preview(ui->media.dir, ui->media.items[index].name, &tw, &th);
    if (preview == NULL || tw <= 0 || th <= 0) {
        return NULL;
    }
    for (int y = 0; y < GRID_CELL_H; ++y) {
        int sy = y * th / GRID_CELL_H;
        for (int x = 0; x < GRID_CELL_W; ++x) {
            int sx = x * tw / GRID_CELL_W;
            ui->cell[slot][y * GRID_CELL_W + x] =
                preview[(size_t)sy * tw + sx];
        }
    }
    ui->cell_valid[slot] = 1;
    return ui->cell[slot];
}

static void draw_back_button(Ui *ui) {
    Gfx *g = &ui->g;

    panel(g, BACK_RECT.x, BACK_RECT.y, BACK_RECT.w, BACK_RECT.h, 9,
          C_SURFACE());
    icon_back(g, BACK_RECT.x + BACK_RECT.w / 2 + 1,
              BACK_RECT.y + BACK_RECT.h / 2, 11, C_TEXT());
}

static void draw_header(Ui *ui, const char *title, const char *right) {
    Gfx *g = &ui->g;
    int th = text_line_height(16);

    draw_back_button(ui);
    text_clip(g, title, 50, (44 - th) / 2, 16, C_TEXT(), 48, 0, 200, 44);
    if (right != NULL) {
        int w = text_width(right, 12);
        text_clip(g, right, SCREEN_W - 12 - w, 22 - text_line_height(12) / 2,
                  12, C_TEXT2(), 48, 0, SCREEN_W - 60, 44);
    }
    gfx_hline(g, 0, 44, SCREEN_W, C_LINE_SOFT());
}

static void draw_page_button(Ui *ui, R r, int right, int enabled) {
    Gfx *g = &ui->g;

    panel(g, r.x, r.y, r.w, r.h, 9, enabled ? C_SURFACE() : C_BG());
    icon_chevron(g, r.x + r.w / 2, r.y + r.h / 2, 9, right,
                 enabled ? C_TEXT2() : C_TEXT3());
}

static void draw_gallery(Ui *ui, const CamShm *st) {
    Gfx *g = &ui->g;
    char pagebuf[24];
    int page = ui->gallery_page;
    int pages = gallery_pages(ui);
    int first = page * GRID_PER_PAGE;
    int count = ui->media.count;

    (void)st;
    gfx_clear(g, C_BG());
    draw_header(ui, "相册", NULL);
    text_count_unit(g, (unsigned)count, SCREEN_W - 12,
                    (44 - text_line_height(12)) / 2, 12, C_TEXT2(), 0, 0,
                    SCREEN_W, 44);

    if (count == 0) {
        icon_camera_outline(g, SCREEN_W / 2, 94, 48, C_TEXT3());
        text_clip_center(g, "还没有照片", 0, 128, SCREEN_W, 22, 16, C_TEXT(),
                         1);
        text_clip_center(g, "点按快门拍下第一张", 0, 154, SCREEN_W, 18, 12,
                         C_TEXT2(), 1);
        return;
    }

    for (int i = first; i < count && i < first + GRID_PER_PAGE; ++i) {
        R r = grid_rect(i);
        const uint16_t *tile = cell_image(ui, i);
        int selected = i == ui->gallery_sel;

        if (selected) {
            gfx_fill_round(g, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 8,
                           C_ACCENT());
        } else {
            gfx_fill_round(g, r.x - 1, r.y - 1, r.w + 2, r.h + 2, 7,
                           C_LINE_SOFT());
        }

        if (tile != NULL) {
            gfx_clip(g, r.x, r.y, r.w, r.h);
            gfx_blit565(g, tile, GRID_CELL_W, GRID_CELL_W, GRID_CELL_H, r.x,
                        r.y);
            gfx_reset_clip(g, SCREEN_W, SCREEN_H);
        } else {
            gfx_fill(g, r.x, r.y, r.w, r.h, C_SURFACE());
            icon_camera_small(g, r.x + r.w / 2, r.y + r.h / 2, C_TEXT3());
        }
        cut_corners(g, r.x, r.y, r.w, r.h, 6,
                    selected ? C_ACCENT() : C_LINE_SOFT());
    }

    if (pages > 1) {
        draw_page_button(ui, PAGE_PREV, 0, page > 0);
        draw_page_button(ui, PAGE_NEXT, 1, page + 1 < pages);
        snprintf(pagebuf, sizeof(pagebuf), "%d / %d", page + 1, pages);
        text_clip_center(g, pagebuf, 132, 208, 56, 26, 12, C_TEXT2(), 1);
    } else {
        text_clip_center(g, "点按查看", 0, 208, SCREEN_W, 26, 12, C_TEXT3(),
                         1);
    }
}

/* -------------------------------------------------------------------- photo */

static void draw_photo_button(Ui *ui, R r, int right, int enabled) {
    Gfx *g = &ui->g;

    glass_panel(g, r.x, r.y, r.w, r.h, 10);
    icon_chevron(g, r.x + r.w / 2, r.y + r.h / 2, 10, right,
                 enabled ? C_TEXT() : C_TEXT3());
}

static void draw_photo(Ui *ui, const CamShm *st) {
    Gfx *g = &ui->g;
    int index = ui->gallery_sel;
    int count = ui->media.count;
    int tw = 0;
    int th = 0;
    const uint16_t *preview = NULL;
    char footer[32];

    (void)st;
    gfx_clear(g, C_BLACK());

    if (index >= 0 && index < count) {
        preview =
            media_preview(ui->media.dir, ui->media.items[index].name, &tw, &th);
    }
    if (preview != NULL && tw > 0 && th > 0) {
        Rect fit = gfx_fit(tw, th, SCREEN_W, SCREEN_H);
        gfx_blit565_scaled(g, preview, tw, tw, th, fit.x, fit.y, fit.w,
                           fit.h);
    } else {
        icon_camera_outline(g, SCREEN_W / 2, SCREEN_H / 2 - 12, 48, C_TEXT3());
        text_clip_center(g, "无法显示", 0, SCREEN_H / 2 + 28, SCREEN_W, 20, 12,
                         C_TEXT2(), 1);
    }

    draw_scrim(g, 0, 0, SCREEN_W, 48, 170, 0, 6);
    draw_scrim(g, 0, SCREEN_H - 52, SCREEN_W, 52, 0, 180, 7);

    draw_back_button(ui);

    if (index >= 0 && index < count) {
        int th = text_line_height(12);
        text_clip(g, ui->media.items[index].name, 50, (44 - th) / 2, 12,
                  C_TEXT(), 48, 0, SCREEN_W - 96, 44);
    }

    draw_photo_button(ui, PHOTO_PREV, 0, index > 0);
    draw_photo_button(ui, PHOTO_NEXT, 1, index + 1 < count);
    snprintf(footer, sizeof(footer), "%d / %d", index + 1,
             count > 0 ? count : 1);
    text_clip_center(g, footer, 136, PHOTO_PREV.y, 48, PHOTO_PREV.h, 12,
                     C_TEXT(), 1);
}

/* ----------------------------------------------------------------- settings */

static void draw_tile(Gfx *g, int x, int y, uint16_t tint) {
    gfx_fill_round(g, x, y, 26, 26, 8, C_SURFACE_HI());
    gfx_fill_round_alpha(g, x, y, 26, 26, 8, tint, 34);
}

static R setting_row(int index) {
    R r;

    r.x = SET_X;
    r.y = SET_Y + index * SET_ROW_H;
    r.w = SET_W;
    r.h = SET_ROW_H;
    return r;
}

static void draw_setting_row(Ui *ui, int index, const char *label,
                             const char *value, uint16_t tint, int icon) {
    Gfx *g = &ui->g;
    R r = setting_row(index);
    int th = text_line_height(12);
    int ty = r.y + (r.h - th) / 2;

    if (index > 0) {
        gfx_hline(g, 58, r.y, SET_W - 50, C_LINE_SOFT());
    }

    draw_tile(g, r.x + 14, r.y + 6, tint);
    switch (icon) {
    case 0:
        icon_camera_small(g, r.x + 14 + 13, r.y + 6 + 13, tint);
        break;
    case 1:
        icon_sd(g, r.x + 14 + 13, r.y + 6 + 13, tint);
        break;
    case 2: {
        unsigned pct = ui->st.battery_pct > 100 ? 100 : ui->st.battery_pct;
        icon_battery(g, r.x + 14 + 13, r.y + 6 + 13, (int)pct, tint);
        break;
    }
    case 3:
        icon_crosshair(g, r.x + 14 + 13, r.y + 6 + 13, tint);
        break;
    default:
        icon_info(g, r.x + 14 + 13, r.y + 6 + 13, tint);
        break;
    }

    text_clip(g, label, r.x + 50, ty, 12, C_TEXT(), r.x, r.y, r.w, r.h);
    if (value != NULL) {
        int vw = text_width(value, 12);
        text_clip(g, value, r.x + r.w - 12 - vw, ty, 12, C_TEXT2(), r.x, r.y,
                  r.w, r.h);
    } else {
        icon_chevron(g, r.x + r.w - 18, r.y + r.h / 2, 8, 1, C_TEXT3());
    }
}

static void draw_settings(Ui *ui, const CamShm *st) {
    Gfx *g = &ui->g;
    char buf[48];

    gfx_clear(g, C_BG());
    draw_back_button(ui);
    text_clip_center(g, "设置", 48, 0, SCREEN_W - 96, 44, 16, C_TEXT(), 1);
    gfx_hline(g, 0, 44, SCREEN_W, C_LINE_SOFT());

    panel(g, SET_X, SET_Y, SET_W, SET_ROW_H * 5, 12, C_SURFACE());

    draw_setting_row(ui, 0, "照片规格", "2592x1944", C_ACCENT(), 0);

    if (st->free_mb > 0) {
        snprintf(buf, sizeof(buf), "%u.%u GB", (unsigned)(st->free_mb / 1024u),
                 (unsigned)((st->free_mb % 1024u) * 10u / 1024u));
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    draw_setting_row(ui, 1, "存储空间", buf, C_BLUE(), 1);

    if (st->battery_pct > 0) {
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)st->battery_pct);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    draw_setting_row(ui, 2, "电量", buf, C_GREEN(), 2);

    draw_setting_row(ui, 3, "触摸校准", NULL, C_PURPLE(), 3);
    draw_setting_row(ui, 4, "关于", APP_VERSION, C_GRAY(), 4);
}

/* -------------------------------------------------------------- calibration */

static const int CALIB_X[2] = {44, 276};
static const int CALIB_Y[2] = {84, 180};

static void draw_calibration(Ui *ui) {
    Gfx *g = &ui->g;
    int step = ui->calib_step < 2 ? ui->calib_step : 1;
    int cx = CALIB_X[step];
    int cy = CALIB_Y[step];
    int dot_y = 222;
    int text_x = 158;

    gfx_clear(g, C_BG());

    for (int y = 20; y < SCREEN_H; y += 40) {
        for (int x = 20; x < SCREEN_W; x += 40) {
            gfx_fill(g, x, y, 1, 1, gfx_rgb565(24, 27, 33));
        }
    }

    text_clip_center(g, "触摸校准", 0, 10, SCREEN_W, 22, 16, C_TEXT(), 1);
    text_clip_center(g, "每次开机都会校准", 0, 32, SCREEN_W, 18, 12, C_TEXT2(),
                     1);
    text_clip_center(g, "请点十字中心", 0, 52, SCREEN_W, 18, 12, C_ACCENT(), 1);

    if (step > 0) {
        icon_check(g, CALIB_X[0], CALIB_Y[0], 8, C_GREEN());
    }

    gfx_circle(g, cx, cy, 26, C_LINE());
    gfx_circle(g, cx, cy, 18, C_ACCENT());
    gfx_hline(g, cx - 36, cy, 10, C_ACCENT());
    gfx_hline(g, cx + 27, cy, 10, C_ACCENT());
    gfx_vline(g, cx, cy - 36, 10, C_ACCENT());
    gfx_vline(g, cx, cy + 27, 10, C_ACCENT());
    gfx_fill_circle(g, cx, cy, 3, C_ACCENT());

    for (int i = 0; i < 2; ++i) {
        if (i < step) {
            gfx_fill_circle(g, 138 + i * 10, dot_y, 3, C_GREEN());
        } else if (i == step) {
            gfx_fill_circle(g, 138 + i * 10, dot_y, 3, C_ACCENT());
        } else {
            gfx_circle(g, 138 + i * 10, dot_y, 2, C_TEXT3());
        }
    }
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d / 2", step + 1);
        text_clip(g, buf, text_x, dot_y - 9, 12, C_TEXT2(), text_x - 4,
                  dot_y - 10, 40, 22);
    }
}

static void finish_calibration(Ui *ui) {
    int x_min = 0, x_max = 0, y_min = 0, y_max = 0;
    int swap = 0, ix = 0, iy = 0;

    input_get_ranges(&x_min, &x_max, &y_min, &y_max);
    if (!touchcal_solve(ui->calib_raw_x[0], ui->calib_raw_y[0],
                        ui->calib_raw_x[1], ui->calib_raw_y[1], CALIB_X[0],
                        CALIB_Y[0], CALIB_X[1], CALIB_Y[1], x_min, x_max, y_min,
                        y_max, &swap, &ix, &iy)) {
        ui->calib = 0;
        toast(ui, "操作失败", TOAST_ERR);
        return;
    }
    input_set_transform(swap, ix, iy);
    if (touchcal_save(swap, ix, iy) != 0) {
        fprintf(stderr, "[ui] cannot write the touch calibration\n");
    }
    fprintf(stderr, "[ui] touch transform swap=%d invert_x=%d invert_y=%d\n",
            swap, ix, iy);
    ui->calib = 0;
    toast(ui, "校准完成", TOAST_OK);
}

/* --------------------------------------------------------------------- toast */

static void draw_toast(Ui *ui) {
    Gfx *g = &ui->g;
    int w;
    int x;
    int y = SCREEN_H - 38;
    int th = text_line_height(12);
    int tw;

    if (ui->toast[0] == '\0') {
        return;
    }
    if (ui->toast_until - now_ms() <= 0.0) {
        ui->toast[0] = '\0';
        return;
    }

    tw = text_width(ui->toast, 12);
    w = tw + 38;
    if (w > SCREEN_W - 16) {
        w = SCREEN_W - 16;
    }
    x = (SCREEN_W - w) / 2;

    gfx_fill_round_alpha(g, x, y + 2, w, 26, 13, C_BLACK(), 90);
    panel(g, x, y, w, 26, 13, C_SURFACE_HI());

    switch (ui->toast_kind) {
    case TOAST_OK:
        gfx_fill_circle(g, x + 15, y + 13, 7, C_GREEN());
        icon_check(g, x + 15, y + 13, 3, C_BG());
        break;
    case TOAST_ERR:
        gfx_fill_circle(g, x + 15, y + 13, 7, C_SHUTTER());
        gfx_fill(g, x + 14, y + 8, 2, 6, C_BG());
        gfx_fill(g, x + 14, y + 16, 2, 2, C_BG());
        break;
    default:
        gfx_fill_circle(g, x + 15, y + 13, 7, C_ACCENT());
        gfx_fill_circle(g, x + 15, y + 13, 2, C_BG());
        break;
    }

    text_clip(g, ui->toast, x + 27, y + (26 - th) / 2, 12, C_TEXT(), x + 24, y,
              w - 30, 26);
}

/* ------------------------------------------------------------------- events */

static int inside(R r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/*
 * Returns the hit id and fills the rect used for the press highlight.
 * `round` is the corner radius that matches the element.
 */
static int hit_test(Ui *ui, int x, int y, R *out, int *round) {
    *round = 8;

    switch (ui->view) {
    case VIEW_VIEWFINDER:
        if (inside(GEAR_RECT, x, y)) { *out = GEAR_RECT; *round = 12; return HIT_GEAR; }
        if (inside(GALLERY_RECT, x, y)) { *out = GALLERY_RECT; *round = 12; return HIT_GALLERY; }
        if (inside(SHUTTER_RECT, x, y)) { *out = SHUTTER_RECT; *round = 30; return HIT_SHUTTER; }
        if (inside(CHIP_RECT, x, y)) { *out = CHIP_RECT; *round = 9; return HIT_CHIP; }
        return HIT_NONE;

    case VIEW_GALLERY: {
        int first = ui->gallery_page * GRID_PER_PAGE;
        if (inside(BACK_RECT, x, y)) { *out = BACK_RECT; *round = 9; return HIT_BACK; }
        for (int i = first; i < ui->media.count && i < first + GRID_PER_PAGE; ++i) {
            R r = grid_rect(i);
            if (inside(r, x, y)) { *out = r; *round = 6; return HIT_CELL; }
        }
        if (gallery_pages(ui) > 1) {
            if (ui->gallery_page > 0 && inside(PAGE_PREV, x, y)) {
                *out = PAGE_PREV; *round = 9; return HIT_PAGE_PREV;
            }
            if (ui->gallery_page + 1 < gallery_pages(ui) &&
                inside(PAGE_NEXT, x, y)) {
                *out = PAGE_NEXT; *round = 9; return HIT_PAGE_NEXT;
            }
        }
        return HIT_NONE;
    }

    case VIEW_PHOTO:
        if (inside(BACK_RECT, x, y)) { *out = BACK_RECT; *round = 9; return HIT_BACK; }
        if (inside(PHOTO_PREV, x, y)) { *out = PHOTO_PREV; *round = 10; return HIT_PHOTO_PREV; }
        if (inside(PHOTO_NEXT, x, y)) { *out = PHOTO_NEXT; *round = 10; return HIT_PHOTO_NEXT; }
        return HIT_NONE;

    case VIEW_SETTINGS: {
        R row = setting_row(3);
        if (inside(BACK_RECT, x, y)) { *out = BACK_RECT; *round = 9; return HIT_BACK; }
        if (inside(row, x, y)) { *out = row; *round = 10; return HIT_TOUCH_ROW; }
        return HIT_NONE;
    }
    }
    return HIT_NONE;
}

static void open_gallery(Ui *ui) {
    media_preview_invalidate();
    media_free(&ui->media);
    media_scan_images(&ui->media, media_dir());
    ui->media_loaded = 1;
    ui->gallery_sel = 0;
    ui->gallery_page = 0;
    cell_reset(ui);
    ui->view = VIEW_GALLERY;
}

static void change_page(Ui *ui, int delta) {
    int pages = gallery_pages(ui);
    int page = ui->gallery_page + delta;

    if (page < 0 || page >= pages) {
        return;
    }
    ui->gallery_page = page;
    ui->gallery_sel = page * GRID_PER_PAGE;
    cell_reset(ui);
    media_preview_invalidate();
}

static void show_photo(Ui *ui, int index) {
    if (index < 0 || index >= ui->media.count) {
        return;
    }
    ui->gallery_sel = index;
    media_preview_invalidate();
    ui->view = VIEW_PHOTO;
}

static void step_photo(Ui *ui, int delta) {
    int next = ui->gallery_sel + delta;

    if (next < 0 || next >= ui->media.count) {
        return;
    }
    ui->gallery_sel = next;
    ui->gallery_page = next / GRID_PER_PAGE;
    cell_reset(ui);
    media_preview_invalidate();
}

static void handle_tap(Ui *ui, int x, int y) {
    switch (ui->view) {
    case VIEW_VIEWFINDER:
        if (inside(GEAR_RECT, x, y)) {
            ui->view = VIEW_SETTINGS;
        } else if (inside(GALLERY_RECT, x, y)) {
            open_gallery(ui);
        } else if (inside(SHUTTER_RECT, x, y)) {
            enqueue(ui, CAM_CMD_SHUTTER, 0);
            ui->shutter_pending = 1;
            ui->shutter_at = now_ms();
            toast(ui, "拍摄中", TOAST_INFO);
        } else if (inside(CHIP_RECT, x, y)) {
            ui->view = VIEW_SETTINGS;
        }
        break;

    case VIEW_GALLERY: {
        int first = ui->gallery_page * GRID_PER_PAGE;
        int count = ui->media.count;
        if (inside(BACK_RECT, x, y)) {
            ui->view = VIEW_VIEWFINDER;
            break;
        }
        for (int i = first; i < count && i < first + GRID_PER_PAGE; ++i) {
            if (inside(grid_rect(i), x, y)) {
                show_photo(ui, i);
                return;
            }
        }
        if (gallery_pages(ui) > 1) {
            if (inside(PAGE_PREV, x, y)) {
                change_page(ui, -1);
            } else if (inside(PAGE_NEXT, x, y)) {
                change_page(ui, 1);
            }
        }
        break;
    }

    case VIEW_PHOTO:
        if (inside(BACK_RECT, x, y)) {
            ui->view = VIEW_GALLERY;
            media_preview_invalidate();
        } else if (inside(PHOTO_PREV, x, y)) {
            step_photo(ui, -1);
        } else if (inside(PHOTO_NEXT, x, y)) {
            step_photo(ui, 1);
        }
        break;

    case VIEW_SETTINGS:
        if (inside(BACK_RECT, x, y)) {
            ui->view = VIEW_VIEWFINDER;
        } else if (inside(setting_row(3), x, y)) {
            ui_begin_calibration(ui);
        }
        break;
    }
}

void ui_touch(Ui *ui, const InputEvent *ev) {
    if (ui->calib) {
        if (ev->action == IN_UP) {
            ui->calib_raw_x[ui->calib_step] = ev->raw_x;
            ui->calib_raw_y[ui->calib_step] = ev->raw_y;
            ++ui->calib_step;
            if (ui->calib_step >= 2) {
                finish_calibration(ui);
            }
        }
        return;
    }

    switch (ev->action) {
    case IN_DOWN:
        ui->press_active = 1;
        ui->press_x = ev->x;
        ui->press_y = ev->y;
        ui->press_moved = 0;
        ui->press_id = hit_test(ui, ev->x, ev->y, &ui->press_rect,
                                &ui->press_round);
        break;
    case IN_MOVE:
        if (ui->press_active) {
            int dx = ev->x - ui->press_x;
            int dy = ev->y - ui->press_y;
            if (dx * dx + dy * dy > 24 * 24) {
                ui->press_moved = 1;
                ui->press_id = HIT_NONE;
            }
        }
        break;
    case IN_UP:
        if (ui->press_active && !ui->press_moved) {
            handle_tap(ui, ev->x, ev->y);
        }
        ui->press_active = 0;
        ui->press_id = HIT_NONE;
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------------- draw */

/* Refresh the rail thumbnail every so often if the daemon has not written the
 * preview yet; otherwise the button would stay empty until the next photo. */
static void thumb_maybe_refresh(Ui *ui) {
    double now = now_ms();
    int tw = 0;
    int th = 0;
    const uint16_t *preview;

    if (ui->thumb_valid || now < ui->thumb_try_at) {
        return;
    }
    ui->thumb_try_at = now + 2000.0;

    if (!ui->media_loaded) {
        media_scan_images(&ui->media, media_dir());
        ui->media_loaded = 1;
    }
    if (ui->media.count <= 0) {
        ui->thumb_try_at = now + 60000.0;
        return;
    }

    preview = media_preview(ui->media.dir, ui->media.items[0].name, &tw, &th);
    if (preview == NULL || tw <= 0 || th <= 0) {
        return;
    }
    for (int y = 0; y < THUMB_H; ++y) {
        int sy = y * th / THUMB_H;
        for (int x = 0; x < THUMB_W; ++x) {
            int sx = x * tw / THUMB_W;
            ui->thumb[y * THUMB_W + x] = preview[(size_t)sy * tw + sx];
        }
    }
    ui->thumb_valid = 1;
}

static void draw_press_feedback(Ui *ui) {
    if (!ui->press_active || ui->press_id == HIT_NONE ||
        ui->press_id == HIT_SHUTTER) {
        return;
    }
    gfx_fill_round_alpha(&ui->g, ui->press_rect.x + 1, ui->press_rect.y + 1,
                         ui->press_rect.w - 2, ui->press_rect.h - 2,
                         ui->press_round, C_WHITE(), 34);
}

static void remember_counts(Ui *ui, const CamShm *st, int first) {
    if (first) {
        ui->last_photo_count = st->photo_count;
        return;
    }
    if (st->photo_count != ui->last_photo_count) {
        ui->last_photo_count = st->photo_count;
        ui->media_loaded = 0;
        ui->thumb_valid = 0;
        ui->thumb_try_at = 0.0;
        cell_reset(ui);
        media_preview_invalidate();
        ui->shutter_pending = 0;
        toast(ui, "已保存照片", TOAST_OK);
    }
    if (ui->shutter_pending && now_ms() - ui->shutter_at > 12000.0) {
        ui->shutter_pending = 0;
        toast(ui, "保存失败", TOAST_ERR);
    }
}

void ui_draw(Ui *ui, const CamShm *state, const uint8_t *frame) {
    const int first = !ui->has_state;
    ui->st = *state;
    ui->has_state = 1;
    ui->frame = frame;
    ui->frame_valid = frame != NULL;

    remember_counts(ui, state, first);

    gfx_reset_clip(&ui->g, SCREEN_W, SCREEN_H);
    if (ui->calib) {
        /* If the touch device never delivers anything, do not trap the user
         * on the wizard: fall back to the saved transform. */
        if (now_ms() - ui->calib_started > 45000.0) {
            ui->calib = 0;
            toast(ui, "触摸屏无响应", TOAST_ERR);
        } else {
            draw_calibration(ui);
            draw_toast(ui);
            return;
        }
    }

    switch (ui->view) {
    case VIEW_VIEWFINDER:
        thumb_maybe_refresh(ui);
        draw_viewfinder(ui, state);
        break;
    case VIEW_GALLERY:
        draw_gallery(ui, state);
        break;
    case VIEW_PHOTO:
        draw_photo(ui, state);
        break;
    case VIEW_SETTINGS:
        draw_settings(ui, state);
        break;
    }

    draw_press_feedback(ui);
    draw_toast(ui);
}

/* ---------------------------------------------------------------------- api */

Ui *ui_create(Fb *fb) {
    Ui *ui = (Ui *)calloc(1, sizeof(Ui));
    if (ui == NULL) {
        return NULL;
    }
    ui->fb = fb;
    gfx_init(&ui->g, fb->back, fb->w, fb->w, fb->h);
    ui->view = VIEW_VIEWFINDER;
    ui->gallery_sel = 0;
    ui->toast_kind = TOAST_INFO;
    return ui;
}

void ui_destroy(Ui *ui) {
    if (ui == NULL) {
        return;
    }
    media_free(&ui->media);
    free(ui);
}

uint32_t ui_take_command(Ui *ui, uint32_t *arg) {
    uint32_t cmd;
    if (ui->pending_count <= 0) {
        return CAM_CMD_NONE;
    }
    cmd = ui->pending_cmd[0];
    if (arg != NULL) {
        *arg = ui->pending_arg[0];
    }
    for (int i = 1; i < ui->pending_count; ++i) {
        ui->pending_cmd[i - 1] = ui->pending_cmd[i];
        ui->pending_arg[i - 1] = ui->pending_arg[i];
    }
    --ui->pending_count;
    return cmd;
}

int ui_wants_quit(Ui *ui) { (void)ui; return 0; }

int ui_calibrating(const Ui *ui) { return ui->calib ? 1 : 0; }

void ui_begin_calibration(Ui *ui) {
    ui->calib = 1;
    ui->calib_started = now_ms();
    ui->calib_step = 0;
    ui->press_active = 0;
    ui->press_id = HIT_NONE;
    ui->toast[0] = '\0';
}

void ui_set_link(Ui *ui, int ready) { ui->link_ready = ready ? 1 : 0; }

void ui_set_view(Ui *ui, UiView view) {
    ui->view = view;
    if ((view == VIEW_GALLERY || view == VIEW_PHOTO) && !ui->media_loaded) {
        media_scan_images(&ui->media, media_dir());
        ui->media_loaded = 1;
        cell_reset(ui);
    }
}

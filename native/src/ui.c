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
#define STATUS_H 20
#define SIDE_W 64
#define SIDE_X (SCREEN_W - SIDE_W)

typedef struct {
    int x, y, w, h;
} R;

static const R GEAR_RECT = {264, 10, 48, 56};
static const R GALLERY_RECT = {264, 74, 48, 56};
static const R SHUTTER_RECT = {260, 138, 56, 56};
static const R CHIP_RECT = {264, 202, 48, 34};
static const R BACK_RECT = {2, 2, 40, 40};
static const R PAGE_PREV = {4, 214, 44, 22};
static const R PAGE_NEXT = {SCREEN_W - 48, 214, 44, 22};

#define GRID_TOP 44
#define GRID_CELL_W 96
#define GRID_CELL_H 72
#define GRID_GUTTER 4
#define GRID_COLS 3
#define GRID_ROWS 2
#define GRID_PER_PAGE (GRID_COLS * GRID_ROWS)

/* ----------------------------------------------------------------- colours */

static uint16_t C_BLACK(void) { return gfx_rgb565(0, 0, 0); }
static uint16_t C_WHITE(void) { return gfx_rgb565(245, 245, 245); }
static uint16_t C_DIM(void) { return gfx_rgb565(160, 160, 160); }
static uint16_t C_MUTED(void) { return gfx_rgb565(110, 110, 110); }
static uint16_t C_PANEL(void) { return gfx_rgb565(24, 24, 24); }
static uint16_t C_PANEL_LIGHT(void) { return gfx_rgb565(44, 44, 44); }
static uint16_t C_ACCENT(void) { return gfx_rgb565(255, 193, 7); }
static uint16_t C_RED(void) { return gfx_rgb565(229, 57, 53); }
static uint16_t C_SIDE(void) { return gfx_rgb565(12, 12, 12); }

/* -------------------------------------------------------------------- state */

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

    int press_active;
    int press_x, press_y;
    int press_moved;
    int press_hit;
    R press_rect;

    double toast_until;
    char toast[96];

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

static void toast(Ui *ui, const char *text) {
    snprintf(ui->toast, sizeof(ui->toast), "%s", text);
    ui->toast_until = now_ms() + 2000.0;
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

/* -------------------------------------------------------------------- icons */

static void icon_gear(Gfx *g, int cx, int cy, int r, uint16_t color) {
    static const int dirs[8][2] = {{1000, 0},  {707, 707},  {0, 1000},
                                   {-707, 707}, {-1000, 0},  {-707, -707},
                                   {0, -1000}, {707, -707}};
    gfx_circle(g, cx, cy, r, color);
    gfx_circle(g, cx, cy, r - 3, color);
    for (int i = 0; i < 8; ++i) {
        gfx_line(g, cx + dirs[i][0] * r / 1000, cy + dirs[i][1] * r / 1000,
                 cx + dirs[i][0] * (r + 5) / 1000,
                 cy + dirs[i][1] * (r + 5) / 1000, color);
    }
    gfx_fill_circle(g, cx, cy, 2, color);
}

static void icon_gallery(Gfx *g, int x, int y, int w, int h, uint16_t color) {
    gfx_frame(g, x, y, w, h, color);
    gfx_frame(g, x + 1, y + 1, w - 2, h - 2, color);
    gfx_fill_circle(g, x + w - 7, y + 7, 2, color);
    gfx_line(g, x + 3, y + h - 4, x + w / 2, y + h / 2, color);
    gfx_line(g, x + w / 2, y + h / 2, x + w - 3, y + h - 4, color);
}

static void icon_shutter(Gfx *g, int cx, int cy, int r, int busy) {
    gfx_fill_circle(g, cx, cy, r, C_WHITE());
    if (busy) {
        gfx_fill_circle(g, cx, cy, r - 4, C_MUTED());
        gfx_circle(g, cx, cy, r - 8, C_WHITE());
    } else {
        gfx_fill_circle(g, cx, cy, r - 4, C_RED());
    }
}

static void icon_back(Gfx *g, int cx, int cy, int s, uint16_t color) {
    gfx_line(g, cx + s / 2, cy - s / 2, cx - s / 2, cy, color);
    gfx_line(g, cx - s / 2, cy, cx + s / 2, cy + s / 2, color);
    gfx_line(g, cx + s / 2 - 1, cy - s / 2, cx - s / 2 - 1, cy, color);
}

static void icon_chevron(Gfx *g, int cx, int cy, int s, int right, uint16_t color) {
    int dir = right ? 1 : -1;
    gfx_line(g, cx - dir * s / 2, cy - s / 2, cx + dir * s / 2, cy, color);
    gfx_line(g, cx + dir * s / 2, cy, cx - dir * s / 2, cy + s / 2, color);
}

/* ------------------------------------------------------------------- panels */

static void draw_back_button(Ui *ui) {
    gfx_fill_round(&ui->g, BACK_RECT.x, BACK_RECT.y, BACK_RECT.w, BACK_RECT.h, 8,
                   C_PANEL());
    icon_back(&ui->g, BACK_RECT.x + BACK_RECT.w / 2 + 2,
              BACK_RECT.y + BACK_RECT.h / 2, 14, C_WHITE());
}

static void draw_press_feedback(Ui *ui) {
    if (!ui->press_active || !ui->press_hit) {
        return;
    }
    gfx_fill_alpha(&ui->g, ui->press_rect.x, ui->press_rect.y, ui->press_rect.w,
                   ui->press_rect.h, C_WHITE(), 46);
}

/* --------------------------------------------------------------- viewfinder */

static void draw_status_strip(Ui *ui, const CamShm *st) {
    Gfx *g = &ui->g;
    char buf[48];

    gfx_fill_alpha(g, 0, 0, SIDE_X, STATUS_H, C_BLACK(), 150);
    text_clip(g, "拍照", 6, 3, 12, C_WHITE(), 0, 0, SIDE_X, STATUS_H);

    if (st->free_mb > 0) {
        snprintf(buf, sizeof(buf), "%u.%uG", (unsigned)(st->free_mb / 1024u),
                 (unsigned)((st->free_mb % 1024u) * 10u / 1024u));
        text_clip(g, buf, SIDE_X - 8 - text_width(buf, 12), 3, 12, C_DIM(), 0, 0,
                  SIDE_X, STATUS_H);
    } else if (st->photo_count > 0) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)st->photo_count);
        text_clip(g, buf, SIDE_X - 8 - text_width(buf, 12), 3, 12, C_DIM(), 0, 0,
                  SIDE_X, STATUS_H);
    }

    if (st->flags & CAM_FLAG_ERROR) {
        gfx_fill_alpha(g, 0, STATUS_H, SIDE_X, 18, C_RED(), 190);
        text_clip_center(g, "操作失败", 0, STATUS_H, SIDE_X, 18, 12, C_WHITE(), 1);
    }
}

static void draw_side_column(Ui *ui, const CamShm *st) {
    Gfx *g = &ui->g;
    int busy = (st->flags & CAM_FLAG_BUSY) ? 1 : 0;

    gfx_fill(g, SIDE_X, 0, SIDE_W, SCREEN_H, C_SIDE());

    gfx_fill_round(g, GEAR_RECT.x, GEAR_RECT.y, GEAR_RECT.w, GEAR_RECT.h, 8,
                   C_PANEL());
    icon_gear(g, GEAR_RECT.x + GEAR_RECT.w / 2, GEAR_RECT.y + GEAR_RECT.h / 2, 9,
              C_WHITE());

    gfx_fill_round(g, GALLERY_RECT.x, GALLERY_RECT.y, GALLERY_RECT.w,
                   GALLERY_RECT.h, 8, C_PANEL());
    icon_gallery(g, GALLERY_RECT.x + 12, GALLERY_RECT.y + 18, 24, 20, C_WHITE());

    icon_shutter(g, SHUTTER_RECT.x + SHUTTER_RECT.w / 2,
                 SHUTTER_RECT.y + SHUTTER_RECT.h / 2, 24, busy);

    gfx_fill_round(g, CHIP_RECT.x, CHIP_RECT.y, CHIP_RECT.w, CHIP_RECT.h, 8,
                   C_PANEL());
    text_clip_center(g, "5MP", CHIP_RECT.x, CHIP_RECT.y, CHIP_RECT.w,
                     CHIP_RECT.h, 12, C_ACCENT(), 1);
}

static void draw_viewfinder(Ui *ui, const CamShm *st) {
    Gfx *g = &ui->g;

    if (ui->frame_valid && ui->frame) {
        gfx_clip(g, 0, 0, SIDE_X, SCREEN_H);
        gfx_blit565(g, (const uint16_t *)ui->frame, SCREEN_W, SCREEN_W, SCREEN_H,
                    0, 0);
        gfx_reset_clip(g, SCREEN_W, SCREEN_H);
    } else {
        const char *message;
        gfx_fill(g, 0, 0, SIDE_X, SCREEN_H, gfx_rgb565(20, 20, 22));
        if (!ui->link_ready) {
            message = "启动中";
        } else if (!(st->flags & CAM_FLAG_CAM_READY)) {
            message = "相机未启动";
        } else {
            message = "无信号";
        }
        text_clip_center(g, message, 0, 0, SIDE_X, SCREEN_H, 14, C_MUTED(), 1);
    }

    draw_status_strip(ui, st);
    draw_side_column(ui, st);
}

/* ------------------------------------------------------------------ gallery */

static R grid_rect(int index) {
    int page = index / GRID_PER_PAGE;
    int slot = index - page * GRID_PER_PAGE;
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

static void draw_gallery(Ui *ui, const CamShm *st) {
    Gfx *g = &ui->g;
    char header[32];
    int page = ui->gallery_page;
    int pages = gallery_pages(ui);
    int first = page * GRID_PER_PAGE;
    int count = ui->media.count;

    (void)st;
    gfx_clear(g, C_BLACK());
    draw_back_button(ui);

    text_clip(g, "相册", 52, 10, 14, C_WHITE(), 48, 0, 96, 40);
    snprintf(header, sizeof(header), "%d/%d", page + 1, pages);
    text_clip(g, header, SCREEN_W - 12 - text_width(header, 12), 12, 12, C_DIM(),
              48, 0, SCREEN_W - 48, 40);

    if (count == 0) {
        text_clip_center(g, "还没有照片", 0, 80, SCREEN_W, 40, 14, C_MUTED(), 1);
        return;
    }

    for (int i = first; i < count && i < first + GRID_PER_PAGE; ++i) {
        R r = grid_rect(i);
        int tw = 0, th = 0;
        const uint16_t *preview =
            media_preview(ui->media.dir, ui->media.items[i].name, &tw, &th);

        gfx_fill(g, r.x, r.y, r.w, r.h, C_PANEL());
        if (preview != NULL && tw > 0 && th > 0) {
            int dh = th * r.w / tw;
            int dy = r.y + (r.h - dh) / 2;
            gfx_clip(g, r.x, r.y, r.w, r.h);
            gfx_blit565_scaled(g, preview, tw, tw, th, r.x, dy, r.w, dh);
            gfx_reset_clip(g, SCREEN_W, SCREEN_H);
        } else {
            text_clip_center(g, "…", r.x, r.y, r.w, r.h, 14, C_MUTED(), 1);
        }

        if (i == ui->gallery_sel) {
            gfx_frame(g, r.x - 1, r.y - 1, r.w + 2, r.h + 2, C_ACCENT());
            gfx_frame(g, r.x - 2, r.y - 2, r.w + 4, r.h + 4, C_ACCENT());
        }
    }

    if (pages > 1) {
        if (page > 0) {
            icon_chevron(g, PAGE_PREV.x + PAGE_PREV.w / 2,
                         PAGE_PREV.y + PAGE_PREV.h / 2, 10, 0, C_DIM());
        }
        if (page + 1 < pages) {
            icon_chevron(g, PAGE_NEXT.x + PAGE_NEXT.w / 2,
                         PAGE_NEXT.y + PAGE_NEXT.h / 2, 10, 1, C_DIM());
        }
        snprintf(header, sizeof(header), "%d", count);
        text_clip_center(g, header, 0, 214, SCREEN_W, 22, 12, C_MUTED(), 1);
    }
}

/* -------------------------------------------------------------------- photo */

static void draw_photo(Ui *ui, const CamShm *st) {
    Gfx *g = &ui->g;
    int index = ui->gallery_sel;
    int tw = 0, th = 0;
    const uint16_t *preview = NULL;
    char footer[32];

    (void)st;
    gfx_clear(g, C_BLACK());

    if (index >= 0 && index < ui->media.count) {
        preview = media_preview(ui->media.dir, ui->media.items[index].name, &tw, &th);
    }
    if (preview != NULL && tw > 0 && th > 0) {
        Rect fit = gfx_fit(tw, th, SCREEN_W, SCREEN_H);
        gfx_blit565_scaled(g, preview, tw, tw, th, fit.x, fit.y, fit.w, fit.h);
    } else {
        text_clip_center(g, "无法显示", 0, 0, SCREEN_W, SCREEN_H, 14, C_MUTED(), 1);
    }

    gfx_fill_alpha(g, 0, 0, SCREEN_W, 24, C_BLACK(), 150);
    draw_back_button(ui);
    if (index >= 0 && index < ui->media.count) {
        text_clip(g, ui->media.items[index].name, 50, 4, 12, C_WHITE(), 48, 0,
                  SCREEN_W - 100, 24);
        snprintf(footer, sizeof(footer), "%d/%d", index + 1, ui->media.count);
        text_clip(g, footer, SCREEN_W - 8 - text_width(footer, 12), 4, 12, C_DIM(),
                  48, 0, SCREEN_W - 48, 24);
    }
}

/* ----------------------------------------------------------------- settings */

static const R SET_ROW_SPEC = {8, 24, 304, 34};
static const R SET_ROW_STORAGE = {8, 62, 304, 34};
static const R SET_ROW_BATTERY = {8, 100, 304, 34};
static const R SET_ROW_TOUCH = {8, 138, 304, 34};
static const R SET_ROW_ABOUT = {8, 176, 304, 34};

static void draw_setting_row(Ui *ui, R r, const char *label, const char *value) {
    Gfx *g = &ui->g;
    int th = text_line_height(12);
    gfx_fill_round(g, r.x, r.y, r.w, r.h, 8, C_PANEL());
    text_clip(g, label, r.x + 10, r.y + (r.h - th) / 2, 12, C_WHITE(), r.x, r.y,
              r.w, r.h);
    if (value != NULL) {
        int vw = text_width(value, 12);
        text_clip(g, value, r.x + r.w - 10 - vw, r.y + (r.h - th) / 2, 12, C_DIM(),
                  r.x, r.y, r.w, r.h);
    } else {
        icon_chevron(g, r.x + r.w - 14, r.y + r.h / 2, 8, 1, C_MUTED());
    }
}

static void draw_settings(Ui *ui, const CamShm *st) {
    char buf[48];

    gfx_clear(&ui->g, C_BLACK());
    draw_back_button(ui);
    text_clip_center(&ui->g, "设置", 48, 0, SCREEN_W - 96, 40, 14, C_WHITE(), 1);

    draw_setting_row(ui, SET_ROW_SPEC, "照片规格", "2592x1944");

    snprintf(buf, sizeof(buf), "%u.%u GB", (unsigned)(st->free_mb / 1024u),
             (unsigned)((st->free_mb % 1024u) * 10u / 1024u));
    draw_setting_row(ui, SET_ROW_STORAGE, "存储空间", buf);

    if (st->battery_pct > 0) {
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)st->battery_pct);
    } else {
        snprintf(buf, sizeof(buf), "—");
    }
    draw_setting_row(ui, SET_ROW_BATTERY, "电量", buf);

    draw_setting_row(ui, SET_ROW_TOUCH, "触摸校准", NULL);
    draw_setting_row(ui, SET_ROW_ABOUT, "关于", "USB");
}

/* -------------------------------------------------------------- calibration */

static const int CALIB_X[2] = {48, 272};
static const int CALIB_Y[2] = {88, 168};

static void draw_calibration(Ui *ui) {
    Gfx *g = &ui->g;
    int step = ui->calib_step < 2 ? ui->calib_step : 1;

    gfx_clear(g, C_BLACK());
    text_clip_center(g, "触摸校准", 0, 6, SCREEN_W, 22, 14, C_WHITE(), 1);
    text_clip_center(g, "请点十字中心", 0, 28, SCREEN_W, 20, 12, C_DIM(), 1);

    gfx_line(g, CALIB_X[step] - 14, CALIB_Y[step], CALIB_X[step] + 14,
             CALIB_Y[step], C_ACCENT());
    gfx_line(g, CALIB_X[step], CALIB_Y[step] - 14, CALIB_X[step],
             CALIB_Y[step] + 14, C_ACCENT());
    gfx_circle(g, CALIB_X[step], CALIB_Y[step], 8, C_ACCENT());

    text_clip_center(g, step == 0 ? "1/2" : "2/2", 0, SCREEN_H - 24, SCREEN_W, 20,
                     12, C_MUTED(), 1);
}

static void finish_calibration(Ui *ui) {
    int x_min = 0, x_max = 0, y_min = 0, y_max = 0;
    int swap = 0, ix = 0, iy = 0;

    input_get_ranges(&x_min, &x_max, &y_min, &y_max);
    if (!touchcal_solve(ui->calib_raw_x[0], ui->calib_raw_y[0],
                        ui->calib_raw_x[1], ui->calib_raw_y[1], CALIB_X[0],
                        CALIB_Y[0], CALIB_X[1], CALIB_Y[1], x_min, x_max, y_min,
                        y_max, &swap, &ix, &iy)) {
        toast(ui, "操作失败");
        ui->calib = 0;
        return;
    }
    input_set_transform(swap, ix, iy);
    touchcal_save(swap, ix, iy);
    ui->calib = 0;
    toast(ui, "校准完成");
}

/* --------------------------------------------------------------------- toast */

static void draw_toast(Ui *ui) {
    int w, x;
    if (ui->toast[0] == '\0') {
        return;
    }
    if (ui->toast_until - now_ms() <= 0.0) {
        ui->toast[0] = '\0';
        return;
    }
    w = text_width(ui->toast, 12) + 16;
    x = (SCREEN_W - w) / 2;
    gfx_fill_round(&ui->g, x, SCREEN_H - 42, w, 24, 8, C_PANEL_LIGHT());
    text_clip_center(&ui->g, ui->toast, x, SCREEN_H - 42, w, 24, 12, C_WHITE(), 1);
}

/* ------------------------------------------------------------------- events */

static int inside(R r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* Returns 1 and fills `out` when (x, y) is on an interactive element. */
static int hit_test(Ui *ui, int x, int y, R *out) {
    switch (ui->view) {
    case VIEW_VIEWFINDER:
        if (inside(GEAR_RECT, x, y)) { *out = GEAR_RECT; return 1; }
        if (inside(GALLERY_RECT, x, y)) { *out = GALLERY_RECT; return 1; }
        if (inside(SHUTTER_RECT, x, y)) { *out = SHUTTER_RECT; return 1; }
        if (inside(CHIP_RECT, x, y)) { *out = CHIP_RECT; return 1; }
        return 0;

    case VIEW_GALLERY: {
        int first = ui->gallery_page * GRID_PER_PAGE;
        if (inside(BACK_RECT, x, y)) { *out = BACK_RECT; return 1; }
        for (int i = first; i < ui->media.count && i < first + GRID_PER_PAGE; ++i) {
            R r = grid_rect(i);
            if (inside(r, x, y)) { *out = r; return 1; }
        }
        if (gallery_pages(ui) > 1) {
            if (ui->gallery_page > 0 && inside(PAGE_PREV, x, y)) {
                *out = PAGE_PREV;
                return 1;
            }
            if (ui->gallery_page + 1 < gallery_pages(ui) &&
                inside(PAGE_NEXT, x, y)) {
                *out = PAGE_NEXT;
                return 1;
            }
        }
        return 0;
    }

    case VIEW_PHOTO:
        if (inside(BACK_RECT, x, y)) { *out = BACK_RECT; return 1; }
        return 0;

    case VIEW_SETTINGS:
        if (inside(BACK_RECT, x, y)) { *out = BACK_RECT; return 1; }
        if (inside(SET_ROW_TOUCH, x, y)) { *out = SET_ROW_TOUCH; return 1; }
        return 0;
    }
    return 0;
}

static void open_gallery(Ui *ui) {
    media_preview_invalidate();
    media_free(&ui->media);
    media_scan_images(&ui->media, media_dir());
    ui->media_loaded = 1;
    ui->gallery_sel = 0;
    ui->gallery_page = 0;
    ui->view = VIEW_GALLERY;
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
            toast(ui, "拍摄中");
        } else if (inside(CHIP_RECT, x, y)) {
            ui->view = VIEW_SETTINGS;
        }
        break;

    case VIEW_GALLERY: {
        int count = ui->media.count;
        int first = ui->gallery_page * GRID_PER_PAGE;
        if (inside(BACK_RECT, x, y)) {
            ui->view = VIEW_VIEWFINDER;
            break;
        }
        for (int i = first; i < count && i < first + GRID_PER_PAGE; ++i) {
            if (inside(grid_rect(i), x, y)) {
                ui->gallery_sel = i;
                media_preview_invalidate();
                ui->view = VIEW_PHOTO;
                return;
            }
        }
        if (gallery_pages(ui) > 1) {
            if (ui->gallery_page > 0 && inside(PAGE_PREV, x, y)) {
                ui->gallery_page--;
                ui->gallery_sel = ui->gallery_page * GRID_PER_PAGE;
            } else if (ui->gallery_page + 1 < gallery_pages(ui) &&
                       inside(PAGE_NEXT, x, y)) {
                ui->gallery_page++;
                ui->gallery_sel = ui->gallery_page * GRID_PER_PAGE;
            }
        }
        break;
    }

    case VIEW_PHOTO:
        if (inside(BACK_RECT, x, y)) {
            ui->view = VIEW_GALLERY;
            media_preview_invalidate();
        } else if (x < SCREEN_W / 3 && ui->gallery_sel > 0) {
            ui->gallery_sel--;
            media_preview_invalidate();
        } else if (x > SCREEN_W * 2 / 3 && ui->gallery_sel + 1 < ui->media.count) {
            ui->gallery_sel++;
            media_preview_invalidate();
        }
        break;

    case VIEW_SETTINGS:
        if (inside(BACK_RECT, x, y)) {
            ui->view = VIEW_VIEWFINDER;
        } else if (inside(SET_ROW_TOUCH, x, y)) {
            ui_begin_calibration(ui);
        }
        break;
    }
}

void ui_touch(Ui *ui, const InputEvent *ev) {
    if (ui->calib) {
        if (ev->action == IN_UP || ev->action == IN_DOWN) {
            if (ev->action == IN_UP) {
                ui->calib_raw_x[ui->calib_step] = ev->raw_x;
                ui->calib_raw_y[ui->calib_step] = ev->raw_y;
                ui->calib_step++;
                if (ui->calib_step >= 2) {
                    finish_calibration(ui);
                }
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
        ui->press_hit = hit_test(ui, ev->x, ev->y, &ui->press_rect);
        break;
    case IN_MOVE:
        if (ui->press_active) {
            int dx = ev->x - ui->press_x;
            int dy = ev->y - ui->press_y;
            if (dx * dx + dy * dy > 24 * 24) {
                ui->press_moved = 1;
                ui->press_hit = 0;
            }
        }
        break;
    case IN_UP:
        if (ui->press_active && !ui->press_moved) {
            handle_tap(ui, ev->x, ev->y);
        }
        ui->press_active = 0;
        ui->press_hit = 0;
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------------- draw */

static void remember_counts(Ui *ui, const CamShm *st, int first) {
    if (first) {
        ui->last_photo_count = st->photo_count;
        return;
    }
    if (st->photo_count != ui->last_photo_count) {
        ui->last_photo_count = st->photo_count;
        ui->media_loaded = 0;
        media_preview_invalidate();
        ui->shutter_pending = 0;
        toast(ui, "已保存照片");
    }
    if (ui->shutter_pending && now_ms() - ui->shutter_at > 12000.0) {
        ui->shutter_pending = 0;
        toast(ui, "保存失败");
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
         * on the wizard: fall back to the default transform. */
        if (now_ms() - ui->calib_started > 45000.0) {
            ui->calib = 0;
            toast(ui, "操作失败");
        } else {
            draw_calibration(ui);
            draw_toast(ui);
            return;
        }
    }

    switch (ui->view) {
    case VIEW_VIEWFINDER:
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

/* --------------------------------------------------------------------- api */

Ui *ui_create(Fb *fb) {
    Ui *ui = (Ui *)calloc(1, sizeof(Ui));
    if (ui == NULL) {
        return NULL;
    }
    ui->fb = fb;
    gfx_init(&ui->g, fb->back, fb->w, fb->w, fb->h);
    ui->view = VIEW_VIEWFINDER;
    ui->gallery_sel = 0;
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
    ui->press_hit = 0;
    ui->toast[0] = '\0';
}

void ui_set_link(Ui *ui, int ready) { ui->link_ready = ready ? 1 : 0; }

void ui_set_view(Ui *ui, UiView view) {
    ui->view = view;
    if ((view == VIEW_GALLERY || view == VIEW_PHOTO) && !ui->media_loaded) {
        media_scan_images(&ui->media, media_dir());
        ui->media_loaded = 1;
    }
}

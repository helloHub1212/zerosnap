/*
 * ui.h - the on-screen interface, drawn straight into the framebuffer.
 */
#ifndef UI_H
#define UI_H

#include "cam.h"
#include "fb.h"
#include "gfx.h"
#include "input.h"

typedef enum {
    VIEW_VIEWFINDER = 0,
    VIEW_GALLERY = 1,
    VIEW_PHOTO = 2,
    VIEW_SETTINGS = 3
} UiView;

typedef struct Ui Ui;

Ui *ui_create(Fb *fb);
void ui_destroy(Ui *ui);

/* Draw one frame.  `frame` may be NULL when no preview is available yet. */
void ui_draw(Ui *ui, const CamShm *state, const uint8_t *frame);

/* Feed one touch event. */
void ui_touch(Ui *ui, const InputEvent *ev);

/* Whether the shared-memory link to the daemon is up. */
void ui_set_link(Ui *ui, int ready);

/* Pop a queued command for the daemon.  Returns CAM_CMD_NONE when empty. */
uint32_t ui_take_command(Ui *ui, uint32_t *arg);

int ui_wants_quit(Ui *ui);
int ui_calibrating(const Ui *ui);

/* Start the two-tap touchscreen calibration. */
void ui_begin_calibration(Ui *ui);

/* Test hooks. */
void ui_set_view(Ui *ui, UiView view);

#endif /* UI_H */

/*
 * zerosnap-ui - the on-screen application.
 *
 * No X11, no SDL, no pygame.  It owns /dev/fb1 and the touch device, and
 * talks to the Python camera daemon through shared memory.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cam.h"
#include "fb.h"
#include "gfx.h"
#include "input.h"
#include "ipc.h"
#include "text.h"
#include "touchcal.h"
#include "ui.h"

#define FRAME_BYTES (CAM_FRAME_W * CAM_FRAME_H * 2)

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--fb DEVICE] [--fps N] [--screenshot FILE] [--view V]\n"
            "          [--frames N] [--preview FILE] [--calibrate] [--no-calibrate]\n"
            "  --screenshot renders one frame to a PPM file and exits\n"
            "  V is one of: viewfinder, gallery, photo, settings\n",
            argv0);
}

static void write_ppm(const Fb *fb, const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
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

static void fake_frame(uint8_t *buf) {
    uint16_t *px = (uint16_t *)buf;
    for (int y = 0; y < CAM_FRAME_H; ++y) {
        for (int x = 0; x < CAM_FRAME_W; ++x) {
            int grad = 30 + (x * 90 / CAM_FRAME_W) + (y * 60 / CAM_FRAME_H);
            int sky = y < CAM_FRAME_H / 2;
            uint16_t c;
            if (sky) {
                c = gfx_rgb565(grad / 3, grad / 2, grad + 60);
            } else {
                c = gfx_rgb565(grad + 30, grad, grad / 3);
            }
            px[y * CAM_FRAME_W + x] = c;
        }
    }
}

/*
 * Load a binary PPM (P6) as the fake preview.  Used by --screenshot and by the
 * README screenshot generator; it is not part of the device runtime.
 */
static int load_ppm_preview(const char *path, uint8_t *out) {
    FILE *file = fopen(path, "rb");
    char magic[3] = {0};
    int width = 0, height = 0, maxval = 0;
    uint16_t *px = (uint16_t *)out;

    if (file == NULL) {
        return -1;
    }
    if (fscanf(file, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) {
        fclose(file);
        return -1;
    }
    if (fscanf(file, "%d %d %d", &width, &height, &maxval) != 3 || maxval != 255) {
        fclose(file);
        return -1;
    }
    fgetc(file); /* single whitespace after the header */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned char rgb[3];
            if (fread(rgb, 1, 3, file) != 3) {
                fclose(file);
                return -1;
            }
            if (x < CAM_FRAME_W && y < CAM_FRAME_H) {
                px[y * CAM_FRAME_W + x] = gfx_rgb565(rgb[0], rgb[1], rgb[2]);
            }
        }
    }
    fclose(file);
    return 0;
}

static UiView parse_view(const char *name) {
    if (strcmp(name, "gallery") == 0) {
        return VIEW_GALLERY;
    }
    if (strcmp(name, "photo") == 0) {
        return VIEW_PHOTO;
    }
    if (strcmp(name, "settings") == 0) {
        return VIEW_SETTINGS;
    }
    return VIEW_VIEWFINDER;
}

int main(int argc, char **argv) {
    Fb fb;
    Ui *ui;
    Ipc *ipc = NULL;
    CamShm state;
    uint8_t *frame;
    const char *fb_path = NULL;
    const char *shot_path = NULL;
    const char *shot_preview = NULL;
    UiView shot_view = VIEW_VIEWFINDER;
    int target_fps = 30;
    int shot_calibrate = 0;
    int no_calibrate = 0;
    long max_frames = 0; /* 0 = run until signalled */

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--fb") == 0 && i + 1 < argc) {
            fb_path = argv[++i];
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            target_fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            shot_path = argv[++i];
        } else if (strcmp(argv[i], "--view") == 0 && i + 1 < argc) {
            shot_view = parse_view(argv[++i]);
        } else if (strcmp(argv[i], "--preview") == 0 && i + 1 < argc) {
            shot_preview = argv[++i];
        } else if (strcmp(argv[i], "--calibrate") == 0) {
            shot_calibrate = 1;
        } else if (strcmp(argv[i], "--no-calibrate") == 0) {
            no_calibrate = 1;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atol(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (target_fps < 5 || target_fps > 60) {
        target_fps = 30;
    }

    if (fb_open(&fb, fb_path) != 0) {
        perror("fb_open");
        return 1;
    }
    if (text_init() != 0) {
        fprintf(stderr, "text_init failed\n");
        return 1;
    }
    ui = ui_create(&fb);
    frame = (uint8_t *)calloc(1, FRAME_BYTES);
    if (ui == NULL || frame == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    if (shot_path != NULL) {
        memset(&state, 0, sizeof(state));
        state.mode = CAM_MODE_PHOTO;
        state.video_mode = CAM_VIDEO_1080P30;
        state.free_mb = 12800;
        state.battery_pct = 76;
        state.photo_count = 12;
        state.video_count = 0;
        fake_frame(frame);
        if (shot_preview != NULL) {
            if (load_ppm_preview(shot_preview, frame) != 0) {
                fprintf(stderr, "warning: cannot read %s\n", shot_preview);
            }
        }
        ui_set_view(ui, shot_view);
        if (shot_calibrate) {
            ui_begin_calibration(ui);
        }
        ui_draw(ui, &state, frame);
        fb_present(&fb);
        write_ppm(&fb, shot_path);
        ui_destroy(ui);
        fb_close(&fb);
        free(frame);
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /*
     * The daemon may still be importing picamera2 when the UI is already on
     * screen, so linking is retried from the main loop instead of blocking
     * boot here.  Touch is opened the same way, which also means the
     * calibration wizard can appear before the camera is ready.
     */
    memset(&state, 0, sizeof(state));
    state.video_mode = CAM_VIDEO_1080P30;
    ui_set_link(ui, 0);

    {
        double last_log = now_ms();
        int frames = 0;
        double draw_ms = 0.0;
        double present_ms = 0.0;
        int frame_valid = 0;
        int32_t frame_w = 0;
        int32_t frame_h = 0;
        long total_frames = 0;
        double last_ipc_try = 0.0;
        double last_frame_at = now_ms();
        double last_diag = now_ms();
        int diag_stage = -1;
        int input_ready = 0;
        int input_warned = 0;
        double last_input_try = -1.0e9;
        const double period = 1000.0 / (double)target_fps;

        while (g_running) {
            double t0 = now_ms();
            double t1;
            double t2;
            InputEvent events[16];
            int n;

            /*
             * Touch is opened lazily and retried: evdev may not have the panel
             * when the service starts, and a replugged panel should work too.
             * Calibration runs every boot, so the saved transform is only a
             * fallback for the case where the wizard times out.
             */
            if (!input_ready && now_ms() - last_input_try > 2000.0) {
                last_input_try = now_ms();
                if (input_open(NULL) == 0) {
                    int swap_xy = 0, invert_x = 0, invert_y = 0;
                    input_ready = 1;
                    input_set_screen(fb.w, fb.h);
                    if (touchcal_load(&swap_xy, &invert_x, &invert_y)) {
                        input_set_transform(swap_xy, invert_x, invert_y);
                        fprintf(stderr,
                                "[ui] previous touch transform "
                                "swap=%d invert_x=%d invert_y=%d\n",
                                swap_xy, invert_x, invert_y);
                    }
                    if (no_calibrate) {
                        fprintf(stderr, "[ui] calibration skipped by option\n");
                    } else {
                        fprintf(stderr,
                                "[ui] touch calibration on boot: tap the two "
                                "crosses\n");
                        ui_begin_calibration(ui);
                    }
                } else if (!input_warned) {
                    fprintf(stderr, "[ui] touch device not found yet; retrying\n");
                    input_warned = 1;
                }
            }

            /*
             * Reconnect: the daemon may still be starting, may have been
             * restarted by its supervisor, or may have crashed.  Without this
             * the UI would show "no signal" forever.
             */
            if (ipc == NULL && now_ms() - last_ipc_try > 1000.0) {
                last_ipc_try = now_ms();
                ipc = ipc_open(0);
                if (ipc != NULL && !ipc_ready(ipc)) {
                    ipc_close(ipc);
                    ipc = NULL;
                }
                ui_set_link(ui, ipc != NULL);
                if (ipc != NULL) {
                    fprintf(stderr, "[ui] daemon link up\n");
                }
            }

            /* Input first: it is cheap and the fd is non-blocking. */
            n = input_ready ? input_read(events, 16) : 0;
            if (n < 0) {
                input_close();
                input_ready = 0;
                last_input_try = now_ms();
                n = 0;
                fprintf(stderr, "[ui] touch device lost; will retry\n");
            }
            for (int i = 0; i < n; ++i) {
                ui_touch(ui, &events[i]);
            }

            if (ipc != NULL) {
                ipc_read_state(ipc, &state);
                if (ipc_read_frame(ipc, frame, FRAME_BYTES, &frame_w, &frame_h)) {
                    frame_valid = 1;
                    last_frame_at = now_ms();
                }
            }

            t1 = now_ms();
            ui_draw(ui, &state, frame_valid ? frame : NULL);
            t2 = now_ms();
            fb_present(&fb);

            draw_ms += t2 - t1;
            present_ms += now_ms() - t2;
            ++frames;
            ++total_frames;

            /* Forward queued commands to the daemon. */
            for (;;) {
                uint32_t arg = 0;
                uint32_t cmd = ui_take_command(ui, &arg);
                if (cmd == CAM_CMD_NONE) {
                    break;
                }
                if (cmd == CAM_CMD_QUIT) {
                    g_running = 0;
                } else if (ipc != NULL) {
                    ipc_send(ipc, cmd, arg);
                }
            }

            /* One diagnostic line per state change, not per frame. */
            {
                double now = now_ms();
                int stage = ipc == NULL ? 0 : (frame_valid ? 2 : 1);
                if (stage != diag_stage ||
                    (stage < 2 && now - last_diag > 15000.0)) {
                    if (stage == 0) {
                        fprintf(stderr, "[ui] no shared memory yet (daemon down?)\n");
                    } else if (stage == 1) {
                        fprintf(stderr, "[ui] linked, but no frames yet "
                                        "(last frame %.0f s ago)\n",
                                (now - last_frame_at) / 1000.0);
                    } else {
                        fprintf(stderr, "[ui] receiving frames\n");
                    }
                    last_diag = now;
                    diag_stage = stage;
                }
            }

            if (max_frames > 0 && total_frames >= max_frames) {
                g_running = 0;
            }

            if (now_ms() - last_log >= 5000.0) {
                double elapsed = now_ms() - last_log;
                fprintf(stderr, "ui: %.1f fps, draw=%.1f present=%.1f ms/frame\n",
                        frames * 1000.0 / elapsed, draw_ms / (frames ? frames : 1),
                        present_ms / (frames ? frames : 1));
                last_log = now_ms();
                frames = 0;
                draw_ms = 0.0;
                present_ms = 0.0;
            }

            {
                double remaining = period - (now_ms() - t0);
                if (remaining > 1.0) {
                    usleep((useconds_t)(remaining * 1000.0));
                }
            }
        }
    }

    if (ipc != NULL) {
        ipc_send(ipc, CAM_CMD_QUIT, 0);
        ipc_close(ipc);
    }
    input_close();
    ui_destroy(ui);
    fb_close(&fb);
    free(frame);
    return 0;
}

/*
 * zerosnap-bench - measure what the panel actually accepts.
 *
 * This is the decisive experiment for the "1 fps" problem: it writes frames
 * straight into /dev/fb1 with no X11, no SDL and no pygame in the path.
 *
 *   zerosnap-bench --seconds 6
 *   zerosnap-bench --seconds 6 --rows 240 --partial
 *
 * If this reaches ~20 fps, the display path is fine and the old slowness came
 * from X11/SDL/fbcp.  If it stays near 1 fps, the panel or the SPI timing is
 * the problem and no UI rewrite will help.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fb.h"
#include "gfx.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void read_sysfs(const char *path, const char *label) {
    char buf[256];
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("  %-14s <unavailable>\n", label);
        return;
    }
    if (fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        printf("  %-14s %s\n", label, buf);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    Fb fb;
    Gfx g;
    const char *dev = NULL;
    double seconds = 6.0;
    int rows = 240;
    int animate = 1;
    uint32_t frame = 0;
    double t_start;
    double window_start;
    int window_frames = 0;
    double fill_ms = 0.0;
    double present_ms = 0.0;
    int total_frames = 0;
    double total_fill = 0.0;
    double total_present = 0.0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc) {
            rows = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--static") == 0) {
            animate = 0;
        } else if (argv[i][0] != '-') {
            dev = argv[i];
        } else {
            fprintf(stderr, "usage: %s [device] [--seconds N] [--rows N] "
                            "[--static]\n",
                    argv[0]);
            return 2;
        }
    }

    if (fb_open(&fb, dev) != 0) {
        perror("fb_open");
        return 1;
    }

    printf("framebuffer: %dx%d stride=%d bytes=%zu\n", fb.w, fb.h, fb.stride_px,
           fb.bytes);
    read_sysfs("/sys/class/graphics/fb1/name", "name");
    read_sysfs("/sys/class/graphics/fb1/virtual_size", "virtual_size");
    read_sysfs("/sys/class/graphics/fb1/rotate", "rotate");
    read_sysfs("/sys/class/graphics/fb1/stride", "stride");

    if (rows <= 0 || rows > fb.h) {
        rows = fb.h;
    }
    printf("benchmark: %.1f s, %d rows/frame, %s pattern\n", seconds, rows,
           animate ? "animated" : "static");
    printf("%-7s %-8s %-10s %-10s\n", "second", "fps", "fill_ms", "present_ms");

    gfx_init(&g, fb.back, fb.w, fb.w, rows);

    t_start = now_ms();
    window_start = t_start;

    for (;;) {
        double t0 = now_ms();
        double t1;
        double t2;

        /* A moving gradient plus a bar that sweeps: worse case for the panel
         * because nothing can be skipped by a dirty-region optimiser. */
        {
            int offset = animate ? (int)(frame * 3) : 0;
            for (int y = 0; y < rows; ++y) {
                int v = (y * 255 / fb.h + offset) & 0xFF;
                uint16_t color = gfx_rgb565(v, v / 2, 255 - v);
                uint16_t *row = fb.back + (size_t)y * fb.w;
                for (int x = 0; x < fb.w; ++x) {
                    row[x] = color;
                }
            }
            if (animate) {
                int bx = (int)(frame * 7) % fb.w;
                gfx_fill(&g, bx, rows / 2 - 10, 24, 20,
                         gfx_rgb565(255, 255, 255));
            }
        }
        t1 = now_ms();
        fb_present_rows(&fb, 0, rows);
        t2 = now_ms();

        fill_ms += t1 - t0;
        present_ms += t2 - t1;
        ++window_frames;
        ++total_frames;
        ++frame;

        {
            double now = now_ms();
            if (now - window_start >= 1000.0) {
                printf("%-7.0f %-8.1f %-10.2f %-10.2f\n", (now - t_start) / 1000.0,
                       window_frames * 1000.0 / (now - window_start),
                       fill_ms / window_frames, present_ms / window_frames);
                fflush(stdout);
                total_fill += fill_ms;
                total_present += present_ms;
                fill_ms = 0.0;
                present_ms = 0.0;
                window_frames = 0;
                window_start = now;
            }
        }

        if (now_ms() - t_start >= seconds * 1000.0) {
            break;
        }
    }

    {
        double elapsed = now_ms() - t_start;
        printf("\nsummary: %d frames in %.2f s = %.2f fps\n", total_frames,
               elapsed / 1000.0, total_frames * 1000.0 / elapsed);
        if (total_frames > 0) {
            printf("         fill %.2f ms, present %.2f ms, total %.2f ms/frame\n",
                   total_fill / total_frames, total_present / total_frames,
                   elapsed / total_frames);
        }
        printf("bytes/frame: %d (%d rows)\n", fb.w * rows * 2, rows);
        printf("spi ceiling at 31.25 MHz: %.1f fps for %d rows\n",
               31250000.0 / 8.0 / (double)(fb.w * rows * 2),
               rows);
    }

    fb_close(&fb);
    return 0;
}

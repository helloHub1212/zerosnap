/*
 * fb.h - direct /dev/fbN access, no X11, no SDL.
 */
#ifndef FB_H
#define FB_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    uint8_t *mem;    /* kernel framebuffer (mmap) */
    uint16_t *back;  /* RGB565 scratch buffer we draw into */
    int w;
    int h;
    int stride_px;
    size_t bytes;
} Fb;

/*
 * Open a framebuffer and allocate the scratch buffer.
 * path may be NULL, in which case /dev/fb1 then /dev/fb0 are tried.
 * Returns 0 on success, -1 on failure (errno set).
 */
int fb_open(Fb *fb, const char *path);

void fb_close(Fb *fb);

/* Copy the whole scratch buffer to the kernel framebuffer. */
void fb_present(Fb *fb);

/* Copy rows [y0, y1) only.  y1 is exclusive and is clamped. */
void fb_present_rows(Fb *fb, int y0, int y1);

#endif /* FB_H */

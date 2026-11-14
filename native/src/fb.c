#include "fb.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int env_int(const char *name, int fallback) {
    const char *value = getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    return atoi(value);
}

static int fb_setup(Fb *fb, int fd, const char *path) {
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    size_t bytes;
    int stride_px;

    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0 &&
        ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0) {
        if (var.bits_per_pixel != 16) {
            /* fbtft panels are RGB565; anything else would need a converter. */
            errno = EINVAL;
            return -1;
        }
        fb->w = (int)var.xres;
        fb->h = (int)var.yres;
        stride_px = (int)(fix.line_length / 2);
        if (stride_px < fb->w) {
            stride_px = fb->w;
        }
        bytes = (size_t)stride_px * (size_t)fb->h * 2u;
        if (fix.smem_len && bytes > fix.smem_len) {
            bytes = fix.smem_len;
        }
    } else {
        /*
         * Host development: allow a plain file to stand in for the panel so
         * the renderer can be tested and screenshotted without a Pi.
         */
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            return -1;
        }
        fb->w = env_int("CAMERA_FB_W", 320);
        fb->h = env_int("CAMERA_FB_H", 240);
        stride_px = fb->w;
        bytes = (size_t)stride_px * (size_t)fb->h * 2u;
        if ((size_t)st.st_size < bytes) {
            if (ftruncate(fd, (off_t)bytes) != 0) {
                return -1;
            }
        }
    }

    if (fb->w <= 0 || fb->h <= 0 || stride_px < fb->w) {
        errno = EINVAL;
        return -1;
    }

    fb->mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb->mem == MAP_FAILED) {
        fb->mem = NULL;
        return -1;
    }

    fb->back = calloc(1, bytes);
    if (fb->back == NULL) {
        munmap(fb->mem, bytes);
        fb->mem = NULL;
        errno = ENOMEM;
        return -1;
    }

    fb->stride_px = stride_px;
    fb->bytes = bytes;
    fprintf(stderr, "[fb] %s %dx%d stride=%d bytes=%zu\n", path, fb->w, fb->h,
            fb->stride_px, fb->bytes);
    return 0;
}

int fb_open(Fb *fb, const char *path) {
    static const char *fallbacks[] = {NULL, "/dev/fb1", "/dev/fb0"};
    int saved_errno = ENOENT;

    if (fb == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;

    fallbacks[0] = path;
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i) {
        const char *candidate = fallbacks[i];
        int fd;
        int flags = O_RDWR | O_CLOEXEC;
        if (candidate == NULL || (i > 0 && path != NULL)) {
            continue;
        }
        /* Host development can point at a regular file; create it on demand. */
        if (strncmp(candidate, "/dev/", 5) != 0) {
            flags |= O_CREAT;
        }
        fd = open(candidate, flags, 0600);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }
        if (fb_setup(fb, fd, candidate) == 0) {
            fb->fd = fd;
            return 0;
        }
        saved_errno = errno;
        close(fd);
    }

    errno = saved_errno;
    return -1;
}

void fb_close(Fb *fb) {
    if (fb == NULL) {
        return;
    }
    if (fb->mem) {
        munmap(fb->mem, fb->bytes);
        fb->mem = NULL;
    }
    if (fb->back) {
        free(fb->back);
        fb->back = NULL;
    }
    if (fb->fd >= 0) {
        close(fb->fd);
        fb->fd = -1;
    }
}

void fb_present(Fb *fb) {
    fb_present_rows(fb, 0, fb->h);
}

void fb_present_rows(Fb *fb, int y0, int y1) {
    if (fb == NULL || fb->mem == NULL || fb->back == NULL) {
        return;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > fb->h) {
        y1 = fb->h;
    }
    if (y0 >= y1) {
        return;
    }
    {
        const size_t row_bytes = (size_t)fb->w * 2u;
        const size_t back_stride = (size_t)fb->w * 2u;
        const size_t mem_stride = (size_t)fb->stride_px * 2u;
        const uint8_t *src_base = (const uint8_t *)fb->back;
        for (int y = y0; y < y1; ++y) {
            memcpy(fb->mem + (size_t)y * mem_stride, src_base + (size_t)y * back_stride,
                   row_bytes);
        }
    }
}

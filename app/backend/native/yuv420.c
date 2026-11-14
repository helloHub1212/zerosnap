/*
 * Fast I420 (YUV420) -> RGB888 conversion for the Pi Zero's ARM1176.
 *
 * numpy is far too slow here: the 320x180 preview conversion measured
 * 210-315 ms per frame on the device (no NEON, scalar float ops).  This
 * fixed-point C loop does the same BT.601 limited-range conversion in a few
 * milliseconds.
 *
 * Layout expected (libcamera / Picamera2 I420):
 *   Y plane: height rows of `stride` bytes
 *   U plane: height/2 rows of `stride/2` bytes
 *   V plane: height/2 rows of `stride/2` bytes
 *
 * Build (done automatically by start-camera.sh on first run):
 *   gcc -O3 -march=armv6 -mfpu=vfp -mfloat-abi=hard -shared -fPIC \
 *       -o libyuv420.so yuv420.c
 */

#include <stdint.h>

static inline uint8_t clamp_u8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

void yuv420_to_rgb(const uint8_t *src, int width, int height, int stride,
                   uint8_t *dst) {
    if (src == 0 || dst == 0 || width <= 0 || height <= 0 || stride < width) {
        return;
    }

    const uint8_t *y_plane = src;
    const uint8_t *u_plane = src + (long)stride * height;
    const int chroma_stride = stride / 2;
    const uint8_t *v_plane = u_plane + (long)chroma_stride * (height / 2);

    for (int j = 0; j < height; ++j) {
        const uint8_t *y_row = y_plane + (long)j * stride;
        const uint8_t *u_row = u_plane + (long)(j >> 1) * chroma_stride;
        const uint8_t *v_row = v_plane + (long)(j >> 1) * chroma_stride;
        uint8_t *out = dst + (long)j * width * 3;

        for (int i = 0; i < width; ++i) {
            const int c = ((int)y_row[i] - 16) * 74;
            const int d = (int)u_row[i >> 1] - 128;
            const int e = (int)v_row[i >> 1] - 128;
            out[3 * i + 0] = clamp_u8((c + 102 * e) >> 6);
            out[3 * i + 1] = clamp_u8((c - 25 * d - 52 * e) >> 6);
            out[3 * i + 2] = clamp_u8((c + 129 * d) >> 6);
        }
    }
}

/*
 * RGB888 -> RGB565 (little endian) for the native framebuffer UI.
 *
 * The gallery and the shared-memory publisher need 16 bit pixels; doing this
 * in numpy on the ARM1176 costs ~10 ms per 320x240 frame, the C loop ~1 ms.
 */
void rgb888_to_rgb565(const uint8_t *src, int width, int height,
                      uint16_t *dst) {
    const long count = (long)width * height;
    for (long i = 0; i < count; ++i) {
        const uint8_t r = src[3 * i + 0];
        const uint8_t g = src[3 * i + 1];
        const uint8_t b = src[3 * i + 2];
        dst[i] = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) |
                            (b >> 3));
    }
}

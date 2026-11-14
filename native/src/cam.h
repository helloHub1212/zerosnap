/*
 * cam.h - shared contract between the C++ UI and the Python camera daemon.
 *
 * Design rules:
 *  - No third party libraries.  Only libc / pthread / kernel headers.
 *  - The UI owns /dev/fb1 and the touch device.
 *  - The Python daemon owns the camera, the encoder and the SD card.
 *  - They talk through one POSIX shared memory segment (frames + state +
 *    commands).  No JSON, no parsing, no allocation on the hot path.
 *
 * All fields are fixed width and naturally aligned so the same memory can be
 * written from Python (struct/mmap) and read from C++.
 */
#ifndef CAM_H
#define CAM_H

#include <stdint.h>

#define CAM_SHM_NAME "/zerosnap-shm"
#define CAM_SHM_SIZE (sizeof(CamShm))

#define CAM_MAGIC 0x43414D31u /* "CAM1" */
#define CAM_VERSION 1u

#define CAM_FRAME_W 320
#define CAM_FRAME_H 240
#define CAM_FRAME_BYTES (CAM_FRAME_W * CAM_FRAME_H * 2) /* RGB565 */

#define CAM_MAX_PATH 256

/* UI -> daemon commands.  Values 2-6 are reserved: they belonged to the
 * video path, which was removed.  Keeping the numbering means an old and a
 * new build can still share a segment without misinterpreting a command. */
enum {
    CAM_CMD_NONE = 0,
    CAM_CMD_SHUTTER = 1, /* take a photo */
    CAM_CMD_REFRESH = 7, /* re-scan the media directory */
    CAM_CMD_QUIT = 8
};

/* Daemon -> UI flags. */
enum {
    CAM_FLAG_BUSY = 1u << 0,      /* capturing / finalising */
    CAM_FLAG_ERROR = 1u << 1,     /* last operation failed */
    CAM_FLAG_CAM_READY = 1u << 2, /* camera opened successfully */
    CAM_FLAG_SAVING = 1u << 3     /* writing media to the card */
};

enum {
    CAM_MODE_PHOTO = 0,
    CAM_MODE_VIDEO = 1
};

/* Reserved for the removed video path; fields below are kept so the layout
 * stays stable. */
typedef enum {
    CAM_VIDEO_1080P30 = 0,
    CAM_VIDEO_720P30 = 1,
    CAM_VIDEO_480P60 = 2
} CamVideoMode;

typedef struct {
    uint32_t magic;
    uint32_t version;

    /*
     * Frame ring.  The producer fills slot (frame_seq & 1), then increments
     * frame_seq with release semantics.  The consumer reads frame_seq, and if
     * it changed, copies slot (frame_seq & 1) before the producer can wrap.
     * Two slots give the consumer a full frame time of slack.
     */
    volatile uint32_t frame_seq;
    uint32_t frame_w;
    uint32_t frame_h;
    uint32_t frame_stride_px;
    uint32_t frame_pad;
    uint8_t frame_data[2][CAM_FRAME_BYTES];

    /*
     * State, written by the daemon after every change.  state_seq is bumped
     * last so the UI can detect a consistent snapshot.
     */
    volatile uint32_t state_seq;
    uint32_t mode;        /* reserved (was CAM_MODE_*) */
    uint32_t recording;   /* reserved, always 0 */
    uint32_t video_mode;  /* reserved, always 0 */
    uint32_t flags;       /* CAM_FLAG_* */
    uint32_t rec_seconds;
    uint32_t photo_count;
    uint32_t video_count;
    uint32_t free_mb;
    uint32_t battery_pct; /* 0-100, 0 when unknown */
    char last_photo[CAM_MAX_PATH];
    char last_video[CAM_MAX_PATH];
    char status_text[CAM_MAX_PATH]; /* short, ASCII, optional */

    /*
     * Commands, written by the UI.  cmd_seq is bumped last; the daemon
     * remembers the last value it executed.
     */
    volatile uint32_t cmd_seq;
    uint32_t cmd;
    uint32_t cmd_arg;
} CamShm;

#endif /* CAM_H */

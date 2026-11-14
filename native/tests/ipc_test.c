/*
 * ipc_test.c - host test for src/ipc.c.
 *
 * Default mode runs the full in-process producer/consumer suite.
 *
 * Extra modes used by tests/run_ipc_test.sh for the cross-language checks:
 *   read-published  open an existing segment written by camera_shm.py and
 *                   verify its state/frame contents.
 *   send-command    open an existing segment and send CAM_CMD_SET_VIDEO_MODE.
 *   size            print sizeof(CamShm).
 */
#include "ipc.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *name) {
    if (condition) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        ++failures;
    }
}

static void check_u32(uint32_t got, uint32_t want, const char *name) {
    if (got == want) {
        printf("PASS: %s (%u)\n", name, got);
    } else {
        printf("FAIL: %s (got %u, want %u)\n", name, got, want);
        ++failures;
    }
}

static void set_text(char *dst, const char *src) {
    size_t n = strlen(src);
    if (n >= CAM_MAX_PATH) {
        n = CAM_MAX_PATH - 1;
    }
    memcpy(dst, src, n);
    memset(dst + n, 0, CAM_MAX_PATH - n);
}

static uint8_t pattern_byte(size_t i, uint32_t salt) {
    return (uint8_t)((i * 13u + salt * 31u + 7u) & 0xFFu);
}

static void fill_pattern(uint8_t *buf, size_t n, uint32_t salt) {
    size_t i;
    for (i = 0; i < n; ++i) {
        buf[i] = pattern_byte(i, salt);
    }
}

/* Producer-side frame publish, exactly as the Python daemon does it. */
static uint32_t publish_frame(CamShm *s, uint32_t salt, uint32_t w,
                              uint32_t h) {
    uint32_t seq = __atomic_load_n(&s->frame_seq, __ATOMIC_RELAXED);
    uint32_t slot = (seq + 1) & 1u;

    fill_pattern(s->frame_data[slot], CAM_FRAME_BYTES, salt);
    s->frame_w = w;
    s->frame_h = h;
    s->frame_stride_px = w;
    s->frame_pad = 0;
    seq = (seq + 1) & 0xFFFFFFFFu;
    __atomic_store_n(&s->frame_seq, seq, __ATOMIC_RELEASE);
    return seq;
}

/*
 * State churn: the writer flips state_seq 10000 times while the reader
 * snapshots the state.  The payload fields are deliberately constant: a
 * snapshot that mixes generations would therefore show a field mismatch,
 * while the changing sequence exercises the acquire/re-read/retry path.
 */
typedef struct {
    CamShm *shm;
    uint32_t updates;
    int done;
} Churn;

static void *churn_main(void *arg) {
    Churn *c = (Churn *)arg;
    CamShm *s = c->shm;
    uint32_t i;

    for (i = 1; i <= c->updates; ++i) {
        __atomic_store_n(&s->state_seq, i, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int snapshot_consistent(const CamShm *snap, const char *photo,
                               const char *video, const char *status) {
    uint32_t g = snap->mode;
    return snap->recording == g && snap->video_mode == g &&
           snap->flags == g && snap->rec_seconds == g &&
           snap->photo_count == g && snap->video_count == g &&
           snap->free_mb == g && snap->battery_pct == g &&
           strcmp(snap->last_photo, photo) == 0 &&
           strcmp(snap->last_video, video) == 0 &&
           strcmp(snap->status_text, status) == 0;
}

static void run_state_churn(Ipc *ipc) {
    static const char photo[] = "/tmp/photo-0001.jpg";
    static const char video[] = "/tmp/video-0001.mp4";
    static const char status[] = "idle";
    CamShm *s = ipc_shm(ipc);
    CamShm snap;
    Churn c;
    pthread_t thread;
    int reads = 0;
    int torn = 0;
    int got;

    set_text(s->last_photo, photo);
    set_text(s->last_video, video);
    set_text(s->status_text, status);
    /*
     * Constant payload for the churn phase.  Only state_seq changes, so a
     * snapshot that returns this payload must always see these exact values.
     */
    __atomic_store_n(&s->mode, 7u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->recording, 7u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->video_mode, 7u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->flags, 7u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->rec_seconds, 7u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->photo_count, 7u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->video_count, 7u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->free_mb, 7u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->battery_pct, 7u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->state_seq, 0, __ATOMIC_RELEASE);

    c.shm = s;
    c.updates = 10000;
    c.done = 0;

    got = pthread_create(&thread, NULL, churn_main, &c);
    check(got == 0, "state churn: writer thread created");
    if (got != 0) {
        return;
    }

    for (;;) {
        got = ipc_read_state(ipc, &snap);
        if (got) {
            ++reads;
            if (!snapshot_consistent(&snap, photo, video, status)) {
                ++torn;
            }
            continue;
        }
        if (__atomic_load_n(&c.done, __ATOMIC_ACQUIRE)) {
            /*
             * The writer may have finished while the previous call was
             * burning its retries.  Make one final attempt so the test
             * observes at least the stable post-churn snapshot.
             */
            if (ipc_read_state(ipc, &snap)) {
                ++reads;
                if (!snapshot_consistent(&snap, photo, video, status)) {
                    ++torn;
                }
            }
            break;
        }
    }

    pthread_join(thread, NULL);

    check(reads > 0, "state churn: reader observed updates");
    check(torn == 0, "state churn: no torn snapshot in 10000 updates");

    /* Final stable publish, read once more. */
    {
        uint32_t final_value = 10001;
        __atomic_store_n(&s->mode, final_value, __ATOMIC_RELAXED);
        __atomic_store_n(&s->recording, final_value, __ATOMIC_RELAXED);
        __atomic_store_n(&s->video_mode, final_value, __ATOMIC_RELAXED);
        __atomic_store_n(&s->flags, final_value, __ATOMIC_RELAXED);
        __atomic_store_n(&s->rec_seconds, final_value, __ATOMIC_RELAXED);
        __atomic_store_n(&s->photo_count, final_value, __ATOMIC_RELAXED);
        __atomic_store_n(&s->video_count, final_value, __ATOMIC_RELAXED);
        __atomic_store_n(&s->free_mb, final_value, __ATOMIC_RELAXED);
        __atomic_store_n(&s->battery_pct, final_value, __ATOMIC_RELAXED);
        __atomic_store_n(&s->state_seq, final_value, __ATOMIC_RELEASE);

        got = ipc_read_state(ipc, &snap);
        check(got == 1, "state: final snapshot returned");
        check(snapshot_consistent(&snap, photo, video, status),
              "state: final snapshot consistent");
        check_u32(snap.mode, final_value, "state: final field value");
    }
}

/* ---------------------------------------------------------------------- */

static void run_frame_tests(Ipc *ipc) {
    CamShm *s = ipc_shm(ipc);
    uint8_t *dst = (uint8_t *)malloc(CAM_FRAME_BYTES);
    uint8_t *want = (uint8_t *)malloc(CAM_FRAME_BYTES);
    uint32_t seq;
    int w = -1;
    int h = -1;
    int got;
    size_t i;
    int same;

    check(dst != NULL && want != NULL, "frame: test buffers allocated");
    if (dst == NULL || want == NULL) {
        free(dst);
        free(want);
        return;
    }

    /* First publish: seq 0 -> 1, slot 1. */
    seq = publish_frame(s, 1, CAM_FRAME_W, CAM_FRAME_H);
    check_u32(seq, 1, "frame: first publish sequence");
    fill_pattern(want, CAM_FRAME_BYTES, 1);

    memset(dst, 0, CAM_FRAME_BYTES);
    got = ipc_read_frame(ipc, dst, CAM_FRAME_BYTES, &w, &h);
    check(got == 1, "frame: first read returns a new frame");
    check(w == CAM_FRAME_W && h == CAM_FRAME_H, "frame: dimensions 320x240");
    same = memcmp(dst, want, CAM_FRAME_BYTES) == 0;
    check(same, "frame: first frame bytes match");

    /* Second publish: seq 1 -> 2, slot 0. */
    seq = publish_frame(s, 2, CAM_FRAME_W, CAM_FRAME_H);
    check_u32(seq, 2, "frame: second publish sequence");
    fill_pattern(want, CAM_FRAME_BYTES, 2);

    memset(dst, 0, CAM_FRAME_BYTES);
    got = ipc_read_frame(ipc, dst, CAM_FRAME_BYTES, &w, &h);
    check(got == 1, "frame: second read returns a new frame");
    same = memcmp(dst, want, CAM_FRAME_BYTES) == 0;
    check(same, "frame: second frame bytes match");

    /* No publish: must report no new frame. */
    got = ipc_read_frame(ipc, dst, CAM_FRAME_BYTES, &w, &h);
    check(got == 0, "frame: unchanged sequence returns 0");

    /* Clamped copy: 100 bytes only. */
    seq = publish_frame(s, 3, CAM_FRAME_W, CAM_FRAME_H);
    fill_pattern(want, 100, 3);
    memset(dst, 0xAA, CAM_FRAME_BYTES);
    got = ipc_read_frame(ipc, dst, 100, &w, &h);
    check(got == 1, "frame: clamped read returns 1");
    check(memcmp(dst, want, 100) == 0, "frame: clamped read copied 100 bytes");
    check(dst[100] == 0xAA, "frame: clamped read did not overrun");
    check(w == CAM_FRAME_W && h == CAM_FRAME_H, "frame: clamped dimensions");

    /* Verify producer slot selection: seq & 1 must address the new data. */
    {
        uint32_t cur = __atomic_load_n(&s->frame_seq, __ATOMIC_ACQUIRE);
        uint32_t slot = cur & 1u;
        int slot_ok = 1;
        for (i = 0; i < 256; ++i) {
            if (s->frame_data[slot][i] != pattern_byte(i, 3)) {
                slot_ok = 0;
                break;
            }
        }
        check(slot_ok, "frame: producer writes slot (frame_seq & 1)");
    }

    free(dst);
    free(want);
}

static void run_command_test(Ipc *ipc) {
    CamShm *s = ipc_shm(ipc);
    uint32_t before = __atomic_load_n(&s->cmd_seq, __ATOMIC_RELAXED);
    uint32_t after;

    ipc_send(ipc, CAM_CMD_SHUTTER, 4242);
    after = __atomic_load_n(&s->cmd_seq, __ATOMIC_ACQUIRE);

    check_u32(after, before + 1, "command: cmd_seq incremented");
    check_u32(s->cmd, CAM_CMD_SHUTTER, "command: cmd stored");
    check_u32(s->cmd_arg, 4242, "command: cmd_arg stored");
}

/* ---------------------------------------------------------------------- */
/* Cross-language consumer: verify what camera_shm.py published.          */
/* ---------------------------------------------------------------------- */

static int run_read_published(void) {
    Ipc *ipc = ipc_open(0);
    CamShm snap;
    uint8_t *buf;
    int w = 0;
    int h = 0;
    int got;
    int i;
    size_t n;
    int pattern_ok = 1;

    if (ipc == NULL) {
        printf("FAIL: read-published: ipc_open(0): %s\n", strerror(errno));
        return 1;
    }

    check(ipc_ready(ipc), "cross: segment magic/version ready");
    check_u32(sizeof(CamShm), CAM_SHM_SIZE, "cross: sizeof(CamShm)");

    memset(&snap, 0, sizeof(snap));
    got = 0;
    for (i = 0; i < 100 && !got; ++i) {
        got = ipc_read_state(ipc, &snap);
        if (!got) {
            usleep(1000);
        }
    }
    check(got == 1, "cross: state snapshot read");
    check_u32(snap.mode, 1, "cross: state.mode");
    check_u32(snap.recording, 1, "cross: state.recording");
    check_u32(snap.video_mode, 2, "cross: state.video_mode");
    check_u32(snap.flags, 5, "cross: state.flags");
    check_u32(snap.rec_seconds, 17, "cross: state.rec_seconds");
    check_u32(snap.photo_count, 3, "cross: state.photo_count");
    check_u32(snap.video_count, 4, "cross: state.video_count");
    check_u32(snap.free_mb, 1234, "cross: state.free_mb");
    check_u32(snap.battery_pct, 88, "cross: state.battery_pct");
    check(strcmp(snap.status_text, "hello-from-python") == 0,
          "cross: state.status_text");
    check(strcmp(snap.last_photo, "/tmp/py-photo.jpg") == 0,
          "cross: state.last_photo");
    check(strcmp(snap.last_video, "/tmp/py-video.mp4") == 0,
          "cross: state.last_video");

    buf = (uint8_t *)malloc(CAM_FRAME_BYTES);
    if (buf == NULL) {
        printf("FAIL: cross: frame buffer allocation\n");
        ipc_close(ipc);
        return 1;
    }

    got = 0;
    for (i = 0; i < 100 && !got; ++i) {
        got = ipc_read_frame(ipc, buf, CAM_FRAME_BYTES, &w, &h);
        if (!got) {
            usleep(1000);
        }
    }
    check(got == 1, "cross: frame read");
    check(w == CAM_FRAME_W && h == CAM_FRAME_H, "cross: frame dimensions");
    n = (size_t)CAM_FRAME_BYTES;
    for (i = 0; i < (int)n; ++i) {
        if (buf[i] != (uint8_t)(((size_t)i * 13u + 7u) & 0xFFu)) {
            pattern_ok = 0;
            break;
        }
    }
    check(pattern_ok, "cross: frame pattern written by Python");
    free(buf);
    ipc_close(ipc);
    return failures ? 1 : 0;
}

static int run_send_command(void) {
    Ipc *ipc = ipc_open(0);
    CamShm *s;

    if (ipc == NULL) {
        printf("FAIL: send-command: ipc_open(0): %s\n", strerror(errno));
        return 1;
    }
    s = ipc_shm(ipc);
    check(ipc_ready(ipc), "send-command: segment ready");
    ipc_send(ipc, CAM_CMD_SHUTTER, 2);
    check_u32(s->cmd, CAM_CMD_SHUTTER, "send-command: cmd stored");
    check_u32(s->cmd_arg, 2, "send-command: arg stored");
    ipc_close(ipc);
    return failures ? 1 : 0;
}

/* ---------------------------------------------------------------------- */

static int run_full_suite(void) {
    Ipc *ipc;
    CamShm *s;
    int saved_errno;

    shm_unlink(CAM_SHM_NAME); /* clean slate */

    ipc = ipc_open(1);
    check(ipc != NULL, "open: ipc_open(create=1)");
    if (ipc == NULL) {
        printf("FAIL: open: %s\n", strerror(errno));
        return 1;
    }

    s = ipc_shm(ipc);
    check(s != NULL, "open: ipc_shm non-NULL");
    check_u32(s->magic, CAM_MAGIC, "open: magic written");
    check_u32(s->version, CAM_VERSION, "open: version written");
    check(ipc_ready(ipc) == 1, "open: ipc_ready");

    run_state_churn(ipc);
    run_frame_tests(ipc);
    run_command_test(ipc);

    /* Close and reopen the existing segment as the UI would. */
    ipc_close(ipc);
    ipc = ipc_open(0);
    check(ipc != NULL, "open: reopen existing segment");
    if (ipc != NULL) {
        check(ipc_ready(ipc) == 1, "open: reopened segment ready");
        check_u32(ipc_shm(ipc)->magic, CAM_MAGIC, "open: reopened magic");
        ipc_close(ipc);
    }

    /* Missing daemon: must return NULL with ENOENT, not crash. */
    shm_unlink(CAM_SHM_NAME);
    errno = 0;
    ipc = ipc_open(0);
    saved_errno = errno;
    check(ipc == NULL, "open: missing segment returns NULL");
    check(saved_errno == ENOENT, "open: missing segment sets ENOENT");
    if (ipc != NULL) {
        ipc_close(ipc);
    }

    shm_unlink(CAM_SHM_NAME);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "all";

    if (strcmp(mode, "size") == 0) {
        printf("%zu\n", sizeof(CamShm));
        return 0;
    }
    if (strcmp(mode, "read-published") == 0) {
        int rc = run_read_published();
        printf("RESULT: %s\n", (failures || rc) ? "FAIL" : "PASS");
        return (failures || rc) ? 1 : 0;
    }
    if (strcmp(mode, "send-command") == 0) {
        int rc = run_send_command();
        printf("RESULT: %s\n", (failures || rc) ? "FAIL" : "PASS");
        return (failures || rc) ? 1 : 0;
    }
    if (strcmp(mode, "all") != 0) {
        fprintf(stderr, "usage: %s [all|read-published|send-command|size]\n",
                argv[0]);
        return 2;
    }

    {
        int rc = run_full_suite();
        printf("RESULT: %s\n", (failures || rc) ? "FAIL" : "PASS");
        return (failures || rc) ? 1 : 0;
    }
}

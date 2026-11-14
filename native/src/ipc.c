/*
 * ipc.c - lock-free shared memory link to the Python camera daemon.
 *
 * The producer (Python) and the consumer (this UI) run as separate
 * processes.  The only synchronisation primitives are the sequence
 * counters in cam.h:
 *
 *   frame_seq : bumped after a full frame has been written to a slot.
 *   state_seq : bumped after the state block has been written.
 *   cmd_seq   : bumped after cmd/cmd_arg have been written.
 *
 * Both sides use release stores when bumping and acquire loads when
 * observing, so a reader that sees the new sequence also sees the data
 * written before it.
 *
 * No allocation happens on the read paths.  Frame/state copies go straight
 * from the mapping into the caller's buffer.
 */
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct Ipc {
    CamShm *shm;
    uint32_t last_state_seq;
    uint32_t last_frame_seq;
    /*
     * Second copy of the state block used to validate a snapshot.  The
     * fields from `mode` to `status_text` are contiguous in CamShm.  This
     * is fixed-size storage: no allocation on the read path.
     */
    uint8_t state_scratch[offsetof(CamShm, cmd_seq) - offsetof(CamShm, mode)];
};

#define CAM_STATE_OFFSET offsetof(CamShm, mode)
#define CAM_STATE_LEN (offsetof(CamShm, cmd_seq) - CAM_STATE_OFFSET)

/*
 * Small helper exported from libipc.so so the Python tests can compare
 * ctypes.sizeof() with the real C size.  It is not part of ipc.h on
 * purpose: it exists only for the layout sanity check.
 */
size_t cam_shm_size(void);
size_t cam_shm_size(void) { return (size_t)CAM_SHM_SIZE; }

Ipc *ipc_open(int create) {
    int flags = O_RDWR;
    int fd;
    void *map;
    Ipc *ipc;

    if (create) {
        flags |= O_CREAT;
    }

    fd = shm_open(CAM_SHM_NAME, flags, 0600);
    if (fd < 0) {
        /* ENOENT is expected while the daemon has not started yet. */
        return NULL;
    }

    if (create) {
        if (ftruncate(fd, (off_t)CAM_SHM_SIZE) != 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return NULL;
        }
    } else {
        /*
         * A stale segment from an older layout must not be mapped: touching
         * bytes past EOF would SIGBUS.  Refuse it and let the UI retry.
         */
        struct stat st;
        if (fstat(fd, &st) != 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return NULL;
        }
        if ((off_t)st.st_size < (off_t)CAM_SHM_SIZE) {
            close(fd);
            errno = EINVAL;
            return NULL;
        }
    }

    map = mmap(NULL, CAM_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }

    close(fd);

    ipc = (Ipc *)calloc(1, sizeof(*ipc));
    if (ipc == NULL) {
        munmap(map, CAM_SHM_SIZE);
        errno = ENOMEM;
        return NULL;
    }

    ipc->shm = (CamShm *)map;

    if (create) {
        CamShm *s = ipc->shm;

        memset(map, 0, CAM_SHM_SIZE);
        /*
         * Zero first, then publish the header.  A consumer racing us either
         * sees a zeroed magic (not ready) or the complete header.
         */
        __atomic_store_n(&s->version, CAM_VERSION, __ATOMIC_RELEASE);
        __atomic_store_n(&s->magic, CAM_MAGIC, __ATOMIC_RELEASE);
    }

    return ipc;
}

void ipc_close(Ipc *ipc) {
    if (ipc == NULL) {
        return;
    }
    if (ipc->shm != NULL) {
        munmap(ipc->shm, CAM_SHM_SIZE);
    }
    free(ipc);
}

CamShm *ipc_shm(Ipc *ipc) { return ipc != NULL ? ipc->shm : NULL; }

int ipc_ready(Ipc *ipc) {
    CamShm *s;

    if (ipc == NULL || ipc->shm == NULL) {
        return 0;
    }

    s = ipc->shm;
    return __atomic_load_n(&s->magic, __ATOMIC_ACQUIRE) == CAM_MAGIC &&
           __atomic_load_n(&s->version, __ATOMIC_ACQUIRE) == CAM_VERSION;
}

void ipc_send(Ipc *ipc, uint32_t cmd, uint32_t arg) {
    CamShm *s;
    uint32_t old;

    if (ipc == NULL || ipc->shm == NULL) {
        return;
    }

    s = ipc->shm;
    old = __atomic_load_n(&s->cmd_seq, __ATOMIC_RELAXED);

    s->cmd = cmd;
    s->cmd_arg = arg;

    /* Release: the daemon sees cmd/cmd_arg before the new sequence. */
    __atomic_store_n(&s->cmd_seq, old + 1, __ATOMIC_RELEASE);
}

int ipc_read_state(Ipc *ipc, CamShm *dst) {
    CamShm *s;
    const uint8_t *src_state;
    uint8_t *dst_state;
    int attempt;

    if (ipc == NULL || ipc->shm == NULL) {
        return 0;
    }

    s = ipc->shm;
    src_state = (const uint8_t *)s + CAM_STATE_OFFSET;
    dst_state = dst != NULL ? (uint8_t *)dst + CAM_STATE_OFFSET : NULL;

    /*
     * The producer writes the fields and bumps state_seq afterwards.  A
     * single copy plus a re-read catches the common case, but the producer
     * may be preempted in the tiny gap between its last field store and the
     * bump.  Copying twice and comparing closes that gap: if the producer
     * was mid-write, the second copy differs from the first and is
     * discarded.
     */
    for (attempt = 0; attempt < 8; ++attempt) {
        uint32_t before = __atomic_load_n(&s->state_seq, __ATOMIC_ACQUIRE);
        uint32_t mid;
        uint32_t after;

        if (before == ipc->last_state_seq) {
            return 0; /* nothing new since the last snapshot we returned */
        }

        if (dst != NULL) {
            memcpy(dst_state, src_state, CAM_STATE_LEN);

            mid = __atomic_load_n(&s->state_seq, __ATOMIC_ACQUIRE);
            if (mid != before) {
                continue; /* changed during the first copy: retry */
            }

            memcpy(ipc->state_scratch, src_state, CAM_STATE_LEN);

            after = __atomic_load_n(&s->state_seq, __ATOMIC_ACQUIRE);
            if (after != before) {
                continue; /* changed during the second copy: retry */
            }

            if (memcmp(dst_state, ipc->state_scratch, CAM_STATE_LEN) != 0) {
                continue; /* producer wrote without bumping yet: retry */
            }
        }

        ipc->last_state_seq = before;
        return 1;
    }

    /* Too much churn; report "no consistent snapshot" and try again later. */
    return 0;
}

int ipc_read_frame(Ipc *ipc, uint8_t *dst, int dst_bytes, int *w, int *h) {
    CamShm *s;
    int copy_bytes;
    int attempt;

    if (ipc == NULL || ipc->shm == NULL || dst == NULL || dst_bytes <= 0) {
        return 0;
    }

    s = ipc->shm;
    copy_bytes = dst_bytes < CAM_FRAME_BYTES ? dst_bytes : CAM_FRAME_BYTES;

    for (attempt = 0; attempt < 4; ++attempt) {
        uint32_t before = __atomic_load_n(&s->frame_seq, __ATOMIC_ACQUIRE);
        uint32_t slot;
        uint32_t after;

        if (before == ipc->last_frame_seq) {
            return 0; /* no new frame */
        }

        /*
         * The producer writes slot ((seq - 1) & 1) and then publishes seq,
         * so the slot belonging to this sequence is (seq & 1).
         */
        slot = before & 1u;

        memcpy(dst, s->frame_data[slot], (size_t)copy_bytes);

        after = __atomic_load_n(&s->frame_seq, __ATOMIC_ACQUIRE);
        if (after != before || (after & 1u) != slot) {
            continue; /* producer wrapped while we copied: retry */
        }

        ipc->last_frame_seq = before;
        if (w != NULL) {
            *w = (int)s->frame_w;
        }
        if (h != NULL) {
            *h = (int)s->frame_h;
        }
        return 1;
    }

    return 0;
}

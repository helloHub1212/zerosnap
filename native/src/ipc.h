/*
 * ipc.h - shared memory link to the Python camera daemon.
 */
#ifndef IPC_H
#define IPC_H

#include "cam.h"

typedef struct Ipc Ipc;

/*
 * Map the segment.  When `create` is non-zero the segment is created (mode
 * 0600) and zeroed; the UI passes 0 and waits for the daemon.
 * Returns NULL on failure (errno set).
 */
Ipc *ipc_open(int create);

void ipc_close(Ipc *ipc);

CamShm *ipc_shm(Ipc *ipc);

/* Returns 1 once the daemon has published a valid header. */
int ipc_ready(Ipc *ipc);

/*
 * Publish a command.  cmd_seq is incremented with release semantics so the
 * daemon never observes a half-written command.
 */
void ipc_send(Ipc *ipc, uint32_t cmd, uint32_t arg);

/*
 * Lock-free read of a consistent state snapshot.  Returns 1 when the sequence
 * changed since the previous call.  `dst` may be NULL to only test.
 */
int ipc_read_state(Ipc *ipc, CamShm *dst);

/* Lock-free frame grab.  Returns 1 when a new frame was copied. */
int ipc_read_frame(Ipc *ipc, uint8_t *dst, int dst_bytes, int *w, int *h);

#endif /* IPC_H */

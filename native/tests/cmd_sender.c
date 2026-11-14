#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "ipc.h"

static double ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void) {
    Ipc *ipc = NULL;
    double start = ms();
    while (ms() - start < 4000.0) {
        ipc = ipc_open(0);
        if (ipc && ipc_ready(ipc)) break;
        if (ipc) { ipc_close(ipc); ipc = NULL; }
        usleep(50000);
    }
    if (!ipc) { printf("FAIL sender: no shm\n"); return 1; }
    ipc_send(ipc, CAM_CMD_SHUTTER, 7);
    usleep(300000);
    ipc_close(ipc);
    printf("sender: sent shutter(7)\n");
    return 0;
}

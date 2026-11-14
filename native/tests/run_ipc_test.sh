#!/usr/bin/env bash
#
# Build and run the whole IPC layer test set on the host.
#
#   1. gcc with the exact required flags (-Werror added to prove "clean")
#   2. C in-process producer/consumer suite
#   3. Python layout/convert/publish self-test (checks sizeof against libipc.so)
#   4. Python producer -> C consumer (state + frame)
#   5. C command sender -> Python consumer
#
set -u
set -o pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-gcc}
CFLAGS=(-std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -Werror -Isrc)
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/camera-ipc-test.XXXXXX") || exit 1

cleanup_build() {
    rm -rf "$BUILD"
}
trap cleanup_build EXIT

status=0

step() {
    local name=$1
    shift
    printf '\n=== %s ===\n' "$name"
    if "$@"; then
        printf 'OK: %s\n' "$name"
    else
        printf 'FAILED: %s\n' "$name"
        status=1
    fi
}

printf '=== build: libipc.so (shared, exported cam_shm_size) ===\n'
if "$CC" "${CFLAGS[@]}" -fPIC -shared -o "$BUILD/libipc.so" src/ipc.c -lrt; then
    printf 'OK: build libipc.so\n'
else
    printf 'FAILED: build libipc.so\n'
    status=1
fi

printf '\n=== build: tests/ipc_test ===\n'
if "$CC" "${CFLAGS[@]}" -pthread -o "$BUILD/ipc_test" \
        tests/ipc_test.c src/ipc.c -lrt; then
    printf 'OK: build ipc_test\n'
else
    printf 'FAILED: build ipc_test\n'
    status=1
fi

if [ "$status" -ne 0 ]; then
    printf '\nFINAL RESULT: FAIL (build)\n'
    exit 1
fi

printf '\n=== layout size cross-check ===\n'
c_size=$("$BUILD/ipc_test" size) || status=1
py_size=$(python3 -c 'import sys; sys.path.insert(0, "../app"); import camera_shm; print(camera_shm.CAM_SHM_SIZE)') || status=1
printf 'C sizeof(CamShm)     = %s\n' "${c_size:-<error>}"
printf 'Python ctypes sizeof = %s\n' "${py_size:-<error>}"
if [ -n "${c_size:-}" ] && [ "$c_size" = "$py_size" ]; then
    printf 'PASS: C and Python CamShm sizes agree\n'
else
    printf 'FAIL: C and Python CamShm sizes differ\n'
    status=1
fi

step "C in-process suite" "$BUILD/ipc_test"
step "Python self-test" python3 tests/test_camera_shm.py selftest "$BUILD/libipc.so"
step "Python producer" python3 tests/test_camera_shm.py producer
step "C reads Python-published state+frame" "$BUILD/ipc_test" read-published
step "C sends command" "$BUILD/ipc_test" send-command
step "Python reads C command" python3 tests/test_camera_shm.py check-command
step "cleanup shared memory" python3 tests/test_camera_shm.py cleanup

printf '\n'
if [ "$status" -eq 0 ]; then
    printf 'FINAL RESULT: PASS\n'
else
    printf 'FINAL RESULT: FAIL\n'
fi
exit "$status"

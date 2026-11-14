#!/bin/bash
# Build the native UI for the Pi Zero.
#
# On the Pi itself:
#     sudo bash camera-native/build-arm.sh --local
#
# From a Linux host with the SD card mounted and qemu-arm-static available
# (no cross toolchain needed):
#     sudo bash camera-native/build-arm.sh --root /mnt/card
#
# The chroot mode runs the card's own gcc under qemu, so the binary matches the
# device's libc exactly and the device never has to compile anything on boot.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT=""
LOCAL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --root) ROOT="${2:-}"; shift 2 ;;
        --local) LOCAL=1; shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

log() { printf '[build] %s\n' "$*"; }

# Always build from the source directory: installers call this from elsewhere.
cd "$SCRIPT_DIR" || exit 1

build_here() {
    local dir="$1"
    cd "$dir"
    # rsync preserves source mtimes, which can be older than a previously
    # installed binary; always force a rebuild so a fixed bug is never shipped
    # as a stale binary.
    make clean >/dev/null 2>&1 || true
    make -j"$(nproc 2>/dev/null || echo 2)"
}

if [ "$LOCAL" = "1" ] || [ -z "$ROOT" ]; then
    log "building locally"
    build_here "$SCRIPT_DIR"
    install -d /opt/zerosnap/bin
    install -m 0755 "$SCRIPT_DIR/zerosnap-ui" /opt/zerosnap/bin/zerosnap-ui
    install -m 0755 "$SCRIPT_DIR/zerosnap-bench" /opt/zerosnap/bin/zerosnap-bench
    log "installed to /opt/zerosnap/bin"
    exit 0
fi

[ -d "$ROOT" ] || { log "no such root: $ROOT"; exit 1; }
[ -x "$ROOT/usr/bin/gcc" ] || { log "$ROOT/usr/bin/gcc missing"; exit 1; }

# Preferred path: clang can cross-compile straight to ARM against the card's
# sysroot, with no emulation at all.  qemu + the card's gcc is only a fallback
# (and gcc under qemu-arm is known to ICE on some hosts).
if command -v clang >/dev/null 2>&1 && command -v ld.lld >/dev/null 2>&1; then
    log "cross-compiling with clang (sysroot $ROOT)"
    CC_CMD=(clang --target=arm-linux-gnueabihf --sysroot="$ROOT"
            -mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard
            -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE
            -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
            -Isrc -fuse-ld=lld)
    mkdir -p "$ROOT/opt/zerosnap/bin"
    "${CC_CMD[@]}" -o "$ROOT/opt/zerosnap/bin/zerosnap-ui" \
        src/fb.c src/gfx.c src/text.c src/text_data.c src/input.c src/ipc.c \
        src/media.c src/touchcal.c src/ui.c src/main.c -lm -lrt -lpthread
    "${CC_CMD[@]}" -o "$ROOT/opt/zerosnap/bin/zerosnap-bench" \
        src/bench.c src/fb.c src/gfx.c -lm
    install -d "$ROOT/opt/zerosnap/native"
    rsync -a --delete --exclude build --exclude zerosnap-ui --exclude zerosnap-bench \
          --exclude '__pycache__' "$SCRIPT_DIR/" "$ROOT/opt/zerosnap/native/"
    file "$ROOT/opt/zerosnap/bin/zerosnap-ui" | sed 's/^/[build] /'
    log "done"
    exit 0
fi

if [ ! -x "$ROOT/usr/bin/qemu-arm-static" ]; then
    log "copying qemu-arm-static into the root"
    cp -f /usr/bin/qemu-arm-static "$ROOT/usr/bin/qemu-arm-static"
fi

if [ ! -e /proc/sys/fs/binfmt_misc/qemu-arm ]; then
    log "registering binfmt_misc for arm"
    printf ':qemu-arm:M::\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-arm-static:F' \
        > /proc/sys/fs/binfmt_misc/register
fi

log "syncing sources into $ROOT/opt/zerosnap/native"
mkdir -p "$ROOT/opt/zerosnap/native"
rsync -a --delete --exclude build --exclude zerosnap-ui --exclude zerosnap-bench \
      --exclude '__pycache__' "$SCRIPT_DIR/" "$ROOT/opt/zerosnap/native/"

log "building inside the chroot"
chroot "$ROOT" /bin/bash -lc 'cd /opt/zerosnap/native && make clean >/dev/null 2>&1; make -j2'

log "installing binaries"
install -d "$ROOT/opt/zerosnap/bin"
install -m 0755 "$ROOT/opt/zerosnap/native/zerosnap-ui" "$ROOT/opt/zerosnap/bin/zerosnap-ui"
install -m 0755 "$ROOT/opt/zerosnap/native/zerosnap-bench" "$ROOT/opt/zerosnap/bin/zerosnap-bench"

file "$ROOT/opt/zerosnap/bin/zerosnap-ui" | sed 's/^/[build] /'
log "done"

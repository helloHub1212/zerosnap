#!/bin/bash
# ZeroSnap installer.
#
# On the Pi:
#     sudo bash install/install.sh
#
# From a PC with the SD card mounted (cross-compiles with clang, no emulation):
#     sudo bash install/install.sh --root /mnt/card --boot /mnt/cardboot
#
# The installer is additive and idempotent.  It does not touch your photos.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
NATIVE_DIR="$REPO_DIR/native"
APP_SRC="$REPO_DIR/app"

ROOT=""
BOOT=""
PREFIX=/opt/zerosnap

while [ $# -gt 0 ]; do
    case "$1" in
        --root) ROOT="${2:-}"; shift 2 ;;
        --boot) BOOT="${2:-}"; shift 2 ;;
        --prefix) PREFIX="${2:-}"; shift 2 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$ROOT" ]; then
    ROOT="/"
    BOOT="/boot/firmware"
elif [ -z "$BOOT" ]; then
    BOOT="$ROOT/boot/firmware"
fi

INSTALL_DIR="$ROOT$PREFIX"
APP_DIR="$INSTALL_DIR/app"
BIN_DIR="$INSTALL_DIR/bin"
LIB_DIR="$INSTALL_DIR/lib"
UNIT_DIR="$ROOT/etc/systemd/system"
CONFIG_TXT="$BOOT/config.txt"
MEDIA_DIR="$BOOT/Camera"

log() { printf '[zerosnap] %s\n' "$*"; }
die() { printf '[zerosnap] error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root (sudo bash $0)"

log "prefix: $PREFIX"

# ---------------------------------------------------------------------------
# 1. Binaries
# ---------------------------------------------------------------------------
if [ "$ROOT" = "/" ]; then
    log "building on the device"
    make -C "$NATIVE_DIR" -j"$(nproc 2>/dev/null || echo 2)"
    install -d -m 0755 "$BIN_DIR"
    install -m 0755 "$NATIVE_DIR/zerosnap-ui" "$BIN_DIR/zerosnap-ui"
    install -m 0755 "$NATIVE_DIR/zerosnap-bench" "$BIN_DIR/zerosnap-bench"
else
    bash "$NATIVE_DIR/build-arm.sh" --root "$ROOT"
fi
[ -x "$BIN_DIR/zerosnap-ui" ] || die "zerosnap-ui was not built"

# ---------------------------------------------------------------------------
# 2. Python daemon
# ---------------------------------------------------------------------------
install -d -m 0755 "$APP_DIR"
if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete --exclude '*.pyc' --exclude '__pycache__/' "$APP_SRC/" "$APP_DIR/"
else
    find "$APP_DIR" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
    cp -a "$APP_SRC/." "$APP_DIR/"
fi
chown -R root:root "$APP_DIR"
python3 -m compileall -q "$APP_DIR" 2>/dev/null || true
log "daemon installed: $APP_DIR/daemon.py"

# ---------------------------------------------------------------------------
# 3. Launcher + systemd unit
# ---------------------------------------------------------------------------
install -m 0755 "$SCRIPT_DIR/start-camera.sh" "$INSTALL_DIR/start-camera.sh"
install -m 0644 "$SCRIPT_DIR/zerosnap-ui.service" "$UNIT_DIR/zerosnap-ui.service"
install -d -m 0755 "$UNIT_DIR/multi-user.target.wants"
ln -sfn /etc/systemd/system/zerosnap-ui.service \
        "$UNIT_DIR/multi-user.target.wants/zerosnap-ui.service"
log "enabled zerosnap-ui.service"

mkdir -p "$ROOT/dev/shm"
chmod 1777 "$ROOT/dev/shm" 2>/dev/null || true

# The old prototype installed itself as camera-ui.service under /opt/camera.
# Leaving it enabled would make two UIs fight over /dev/fb1.
for stale in \
    "$UNIT_DIR/display-manager.service" \
    "$UNIT_DIR/multi-user.target.wants/lightdm.service" \
    "$UNIT_DIR/multi-user.target.wants/fbcp.service" \
    "$UNIT_DIR/multi-user.target.wants/getty@tty1.service" \
    "$UNIT_DIR/multi-user.target.wants/camera-ui.service" \
    "$UNIT_DIR/camera-ui.service" \
    "$UNIT_DIR/timers.target.wants/camera-display-guard.timer" \
    "$UNIT_DIR/camera-display-guard.timer" \
    "$UNIT_DIR/camera-display-guard.service"
do
    if [ -e "$stale" ] || [ -L "$stale" ]; then
        rm -f "$stale"
        log "disabled $(basename "$stale")"
    fi
done

# ---------------------------------------------------------------------------
# 4. Panel overlay
# ---------------------------------------------------------------------------
if [ -f "$SCRIPT_DIR/overlays/tft9341.dtbo" ] && [ -d "$BOOT/overlays" ]; then
    install -m 0644 "$SCRIPT_DIR/overlays/tft9341.dtbo" "$BOOT/overlays/tft9341.dtbo"
fi

if [ -f "$CONFIG_TXT" ]; then
    cp -n "$CONFIG_TXT" "$CONFIG_TXT.zerosnap.bak" 2>/dev/null || true
    LINE='dtoverlay=tft9341:rotate=270,speed=32000000,fps=60,txbuflen=65536'
    if grep -q '^dtoverlay=tft9341' "$CONFIG_TXT"; then
        sed -i -E "s|^dtoverlay=tft9341(:.*)?$|$LINE|" "$CONFIG_TXT"
    else
        printf '%s\n' "$LINE" >> "$CONFIG_TXT"
    fi
    grep -q '^dtparam=spi=on' "$CONFIG_TXT" || printf 'dtparam=spi=on\n' >> "$CONFIG_TXT"
    if grep -q '^dtoverlay=vc4-kms-v3d' "$CONFIG_TXT"; then
        log "warning: vc4-kms-v3d is enabled; the legacy fbtft panel needs it off"
    fi
    log "panel overlay: $LINE"
else
    log "warning: $CONFIG_TXT not found"
fi

# ---------------------------------------------------------------------------
# 5. Colour conversion helper (prebuilt: boot must not run gcc)
# ---------------------------------------------------------------------------
mkdir -p "$LIB_DIR"
YUV_SRC="$APP_DIR/backend/native/yuv420.c"
if [ -f "$YUV_SRC" ]; then
    if [ "$ROOT" = "/" ]; then
        gcc -O3 -march=armv6 -mfpu=vfp -mfloat-abi=hard -shared -fPIC \
            -o "$LIB_DIR/libyuv420.so" "$YUV_SRC" && log "built libyuv420.so"
    elif command -v clang >/dev/null 2>&1 && command -v ld.lld >/dev/null 2>&1; then
        clang --target=arm-linux-gnueabihf --sysroot="$ROOT" \
              -mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard \
              -O3 -shared -fPIC -fuse-ld=lld \
              -o "$LIB_DIR/libyuv420.so" "$YUV_SRC" && log "built libyuv420.so"
    fi
fi

# ---------------------------------------------------------------------------
# 6. Media + per-user cache
# ---------------------------------------------------------------------------
install -d -m 0775 "$MEDIA_DIR/.preview"
chown 1000:1000 "$MEDIA_DIR" 2>/dev/null || true

CAMERA_HOME=""
if [ -f "$ROOT/etc/passwd" ]; then
    CAMERA_HOME="$(awk -F: '$3 >= 1000 && $6 ~ /^\/home\// {print $6; exit}' "$ROOT/etc/passwd")"
fi
install -d -m 0755 "$ROOT/etc/default"
printf '# Written by the ZeroSnap installer\nCAMERA_HOME=%s\n' "$CAMERA_HOME" \
    > "$ROOT/etc/default/zerosnap"
chmod 0644 "$ROOT/etc/default/zerosnap"
install -d -m 0755 "$ROOT/var/log/zerosnap"
log "user home: ${CAMERA_HOME:-<not found>}; log: /var/log/zerosnap/app.log"

# ---------------------------------------------------------------------------
# 7. Report
# ---------------------------------------------------------------------------
log "=== done ==="
log "ui:      $BIN_DIR/zerosnap-ui"
log "daemon:  python3 -m app.daemon (from $INSTALL_DIR)"
log "service: zerosnap-ui.service"
log "media:   $MEDIA_DIR"
log "reboot to start"
log "back to the desktop: sudo systemctl start lightdm"

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

# Set KEY=VALUE in a Raspberry Pi config file, replacing a commented or active
# assignment, or appending it when absent.
cfg_set() {
    local key="$1" value="$2" file="$3"
    if grep -qE "^[[:space:]]*#?[[:space:]]*${key}=" "$file"; then
        sed -i -E "s|^[[:space:]]*#?[[:space:]]*${key}=.*|${key}=${value}|" "$file"
    else
        printf '%s=%s\n' "$key" "$value" >> "$file"
    fi
}

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
# Drop host-version bytecode and precompile for the card's own Python.  The
# card is armhf/Python 3.11; compiling with the PC's Python 3.14 produces .pyc
# files the Pi ignores, so every module would be recompiled on first boot.
find "$APP_DIR" -type d -name __pycache__ -prune -exec rm -rf -- {} + 2>/dev/null || true
if [ "$ROOT" = "/" ]; then
    python3 -m compileall -q "$APP_DIR" 2>/dev/null || true
elif [ -x "$ROOT/usr/bin/python3" ] && command -v qemu-arm-static >/dev/null 2>&1; then
    qemu-arm-static -L "$ROOT" "$ROOT/usr/bin/python3" \
        -m compileall -q "$APP_DIR" 2>/dev/null || true
else
    log "warning: cannot precompile the daemon for the card's Python; first boot will be slower"
fi
log "daemon installed: $APP_DIR/daemon.py"

# ---------------------------------------------------------------------------
# 3. Launcher + systemd unit
# ---------------------------------------------------------------------------
install -m 0755 "$SCRIPT_DIR/start-camera.sh" "$INSTALL_DIR/start-camera.sh"
install -m 0755 "$SCRIPT_DIR/exit-to-desktop.sh" "$INSTALL_DIR/exit-to-desktop.sh"
install -m 0755 "$SCRIPT_DIR/return-to-camera.sh" "$INSTALL_DIR/return-to-camera.sh"
install -m 0644 "$SCRIPT_DIR/zerosnap-ui.service" "$UNIT_DIR/zerosnap-ui.service"
install -m 0644 "$SCRIPT_DIR/zerosnap-desktop.service" \
        "$UNIT_DIR/zerosnap-desktop.service"
install -d -m 0755 "$UNIT_DIR/multi-user.target.wants"
ln -sfn /etc/systemd/system/zerosnap-ui.service \
        "$UNIT_DIR/multi-user.target.wants/zerosnap-ui.service"
log "enabled zerosnap-ui.service (desktop helper is installed but not enabled)"

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
    # ext4/mmc/SD are built into this kernel, so the 16 MB initramfs only adds
    # firmware read time on a slow card.  display_auto_detect probes for a DSI
    # panel that this product does not have.
    cfg_set auto_initramfs 0 "$CONFIG_TXT"
    cfg_set display_auto_detect 0 "$CONFIG_TXT"
    # The firmware waits one second by default before handing over to Linux.
    cfg_set boot_delay 0 "$CONFIG_TXT"
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
# 6b. Boot tuning
#
# The Pi Zero spends ~28 s in firmware and ~21 s in userspace.  These changes
# are all idempotent and reversible; they do not remove any user feature.
# ---------------------------------------------------------------------------

# The FAT media partition is written to constantly (photos, thumbnails).  A
# dirty FAT makes the *firmware* repair it at boot, which is what makes boot
# alternate between ~15 s and ~28 s.  `flush` pushes file data on close, and
# skipping the boot fsck removes ~2 s (ext4 handles its own journal).
FSTAB="$ROOT/etc/fstab"
if [ -f "$FSTAB" ]; then
    awk 'BEGIN { OFS = "\t" }
        $1 !~ /^#/ && $2 == "/boot/firmware" {
            opts = $4
            if (opts !~ /(^|,)flush(,|$)/) opts = opts ",flush"
            if (opts !~ /(^|,)noatime(,|$)/) opts = opts ",noatime"
            $4 = opts; $5 = "0"; $6 = "0"
        }
        { print }' "$FSTAB" > "$FSTAB.zerosnap" \
        && cat "$FSTAB.zerosnap" > "$FSTAB" && rm -f "$FSTAB.zerosnap"
    log "fstab: /boot/firmware -> no fsck, flush, noatime"
fi

# Load the SPI panel/touch drivers as soon as modules-load runs.  Left to udev
# they are loaded one device event at a time and /dev/fb1 only appears ~11 s
# later, which delays the first visible UI.
install -d -m 0755 "$ROOT/etc/modules-load.d"
cat > "$ROOT/etc/modules-load.d/zerosnap-panel.conf" <<'EOF'
# ZeroSnap: bring the SPI panel and touchscreen up before udev coldplug.
spi-bcm2835
fbtft
fb_ili9340
ads7846
EOF
log "modules-load: SPI panel/touch preloaded"

# The camera UI is a plain framebuffer app and starts long before any target,
# so graphical.target's extra services no longer compete with the camera
# import.  Keep it as the default because LXDE's pcmanfm/lxpanel need udisks2
# (graphical.target.wants) at session start: without it gvfs-udisks2-volume-
# monitor segfaults and the desktop comes up black with only a mouse cursor.
# LightDM itself is NOT enabled at boot (display-manager.service is removed
# below), so this does not bring back the old 90 s startup delay.
ln -sfn /lib/systemd/system/graphical.target "$ROOT/etc/systemd/system/default.target"
log "default target: graphical.target (LightDM stays off at boot)"

# timesyncd was enabled from sysinit.target, so sysinit waited ~2 s for it even
# though no network is up that early.  Start it with multi-user instead.
SYNC_UNIT=systemd-timesyncd.service
rm -f "$ROOT/etc/systemd/system/sysinit.target.wants/$SYNC_UNIT"
install -d -m 0755 "$ROOT/etc/systemd/system/multi-user.target.wants"
ln -sfn "/lib/systemd/system/$SYNC_UNIT" \
    "$ROOT/etc/systemd/system/multi-user.target.wants/$SYNC_UNIT"
log "timesyncd: started with multi-user.target"

# Mask work that only costs boot time.  Masking (a /dev/null symlink) is the
# only reliable way to disable a unit on a card that is not running.
mask_unit() {
    local unit="$1"
    if [ -e "$ROOT/lib/systemd/system/$unit" ] || [ -e "$ROOT/usr/lib/systemd/system/$unit" ]; then
        ln -sfn /dev/null "$ROOT/etc/systemd/system/$unit"
        log "masked $unit"
    fi
}
mask_unit glamor-test.service   # 20-noglamor.conf is already installed
mask_unit sshswitch.service     # ssh is enabled; no /boot/ssh dance needed
mask_unit e2scrub_all.timer     # LVM-only, and Persistent=true runs it at boot
mask_unit fstrim.timer          # weekly Persistent timer fires at boot
mask_unit dpkg-db-backup.timer  # irrelevant on an appliance

# logrotate's stock timer is Persistent=true, so a missed run is caught up at
# boot.  Clearing that keeps the first boot of the day free of log work.
install -d -m 0755 "$ROOT/etc/systemd/system/logrotate.timer.d"
cat > "$ROOT/etc/systemd/system/logrotate.timer.d/zerosnap-no-catchup.conf" <<'EOF'
# ZeroSnap: do not run a missed rotation at boot.
[Timer]
Persistent=false
EOF

# Swapping in a 512 MB file on a single-core Pi Zero competes with the camera
# import for the SD card.  It is only needed under memory pressure, so let it
# run at idle priority instead of disabling it.
install -d -m 0755 "$ROOT/etc/systemd/system/dphys-swapfile.service.d"
cat > "$ROOT/etc/systemd/system/dphys-swapfile.service.d/zerosnap-idle.conf" <<'EOF'
# ZeroSnap: never compete with the camera start-up.
[Service]
Nice=19
IOSchedulingClass=idle
EOF
log "timers: no boot catch-up; swap at idle priority"

# `import picamera2` loads PyAV (and the libav libraries) even though only the
# video encoder uses it.  Defer that import to the first actual video use.
if command -v python3 >/dev/null 2>&1; then
    python3 "$SCRIPT_DIR/patch-picamera2.py" "$ROOT" \
        || log "warning: could not patch picamera2; camera start-up stays slower"
fi
# Precompile the patched modules with the card's own Python so the first boot
# does not spend time compiling them (the patch script drops stale bytecode).
if [ "$ROOT" != "/" ] && [ -x "$ROOT/usr/bin/python3" ] \
    && command -v qemu-arm-static >/dev/null 2>&1; then
    PYC_PKGS="$ROOT/usr/lib/python3/dist-packages/picamera2"
    if [ -d "$PYC_PKGS" ]; then
        qemu-arm-static -L "$ROOT" "$ROOT/usr/bin/python3" - "$PYC_PKGS" <<'PY' 2>/dev/null || true
import py_compile, sys
base = sys.argv[1]
for rel in ("encoders/encoder.py", "encoders/libav_h264_encoder.py",
            "encoders/libav_mjpeg_encoder.py", "outputs/pyavoutput.py"):
    try:
        py_compile.compile(base + "/" + rel, doraise=True)
    except Exception:
        pass
PY
    fi
fi

# FSCK*.REC files are left behind by the firmware's FAT repair.  Nothing uses
# them.  The unused initramfs images for other board revisions go too.
rm -f "$BOOT"/FSCK*.REC 2>/dev/null || true
rm -f "$BOOT/initramfs7" "$BOOT/initramfs7l" "$BOOT/initramfs8" 2>/dev/null || true
log "boot partition cleaned"

# ---------------------------------------------------------------------------
# 7. Desktop return configuration
#
# The settings page's "退出到桌面" row starts zerosnap-desktop.service, which
# calls exit-to-desktop.sh.  Prepare the card now (Xorg on fb1, LightDM
# autologin session = LXDE) so the first tap works without a reboot.
# ---------------------------------------------------------------------------
bash "$INSTALL_DIR/exit-to-desktop.sh" --root "$ROOT" --prepare \
    || log "warning: could not prepare the desktop session"

install -d -m 0755 "$ROOT/usr/share/applications"
install -m 0644 "$SCRIPT_DIR/zerosnap-camera.desktop" \
        "$ROOT/usr/share/applications/zerosnap-camera.desktop"

if [ -n "$CAMERA_HOME" ] && [ -d "$ROOT$CAMERA_HOME" ]; then
    install -d -m 0755 "$ROOT$CAMERA_HOME/Desktop"
    install -m 0755 "$SCRIPT_DIR/zerosnap-camera.desktop" \
            "$ROOT$CAMERA_HOME/Desktop/zerosnap-camera.desktop"
    chown 1000:1000 "$ROOT$CAMERA_HOME/Desktop/zerosnap-camera.desktop" \
        2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 8. Report
# ---------------------------------------------------------------------------
log "=== done ==="
log "ui:      $BIN_DIR/zerosnap-ui"
log "daemon:  python3 -m app.daemon (from $INSTALL_DIR)"
log "service: zerosnap-ui.service"
log "media:   $MEDIA_DIR"
log "reboot to start"
log "desktop: 设置 -> 退出到桌面 (or: sudo systemctl start zerosnap-desktop)"

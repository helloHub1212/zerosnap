#!/bin/bash
# Leave the camera and start the LXDE desktop on the SPI panel.
#
# Called by start-camera.sh when zerosnap-ui exits with status 42 (the
# "退出到桌面" row in Settings).  It can also prepare the configuration without
# starting anything, which is how install.sh sets up a card from a PC:
#
#     sudo bash exit-to-desktop.sh --prepare
#     sudo bash exit-to-desktop.sh --root /mnt/card --prepare
#
# Why not just `systemctl start lightdm`?  The camera UI owns /dev/fb1 and
# fbcp is disabled.  Xorg's 99-fbdev.conf still points at /dev/fb0 by default,
# so starting LightDM without this helper gives a black panel.  The helper
# switches Xorg to /dev/fb1, makes sure LightDM's autologin session is the
# desktop (not the old `camera` X session), and then starts LightDM.
set -u

ROOT=""
PREPARE=0

usage() {
    sed -n '2,14p' "$0"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --root) ROOT="${2:-}"; ROOT="${ROOT%/}"; shift 2 ;;
        --prepare) PREPARE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

log() { printf '[%s] %s\n' "$(date '+%F %T')" "$*"; }

# --root is an installer-only mode: edit a mounted card and never start a
# service on the host that happens to be running this script.
if [ -n "$ROOT" ]; then
    PREPARE=1
fi

if [ "$PREPARE" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    log "错误：请使用 root 运行"
    exit 1
fi

SESSION=LXDE-pi-x
XF="$ROOT/etc/X11/xorg.conf.d/99-fbdev.conf"
LIGHTDM="$ROOT/etc/lightdm/lightdm.conf"
DEFAULTS="$ROOT/etc/default/zerosnap"
PASSWD="$ROOT/etc/passwd"

# ---------------------------------------------------------------------------
# 1. Xorg must draw straight into the SPI framebuffer.
#
# fbcp copies /dev/fb0 to /dev/fb1 with vc_dispmanx_snapshot(), which is far
# too slow on a Pi Zero.  The camera already writes fb1 directly; the desktop
# has to do the same or it will be a slideshow.
# ---------------------------------------------------------------------------
if [ -f "$XF" ]; then
    if ! grep -Eq 'Option[[:space:]]+"fbdev"[[:space:]]+"/dev/fb1"' "$XF"; then
        if [ ! -f "$XF.zerosnap.bak" ]; then
            cp -a "$XF" "$XF.zerosnap.bak" 2>/dev/null || true
        fi
        tmp="$(mktemp)" || exit 1
        sed 's|Option[[:space:]]*"fbdev"[[:space:]]*"/dev/fb0"|Option "fbdev" "/dev/fb1"|' \
            "$XF" > "$tmp"
        # Write through the existing inode so ownership and mode survive.
        cat "$tmp" > "$XF"
        rm -f "$tmp"
        log "Xorg: /dev/fb0 -> /dev/fb1"
    fi
else
    log "警告：$XF 不存在，桌面可能无法显示"
fi

# LightDM's stock unit waits for /dev/dri/card0 and /dev/dri/renderD128.  The
# SPI panel uses the legacy fbdev driver, so those devices never appear and the
# start job sits there for systemd's 90 s device timeout.
#
# A drop-in cannot fix this: Wants=/After= can only *add* dependencies, never
# remove them (an empty assignment resets the list of the same file only, and
# the unit file is parsed before the drop-in).  The reliable fix is a full unit
# override in /etc/systemd/system, which takes precedence over /lib.
LIGHTDM_VENDOR="$ROOT/lib/systemd/system/lightdm.service"
[ -f "$LIGHTDM_VENDOR" ] || LIGHTDM_VENDOR="$ROOT/usr/lib/systemd/system/lightdm.service"
LIGHTDM_UNIT="$ROOT/etc/systemd/system/lightdm.service"
LIGHTDM_OLD_DROPIN="$ROOT/etc/systemd/system/lightdm.service.d/zerosnap-fb1.conf"
LIGHTDM_CHANGED=0
if [ -d "$ROOT/etc/systemd/system" ]; then
    # Remove the old, ineffective drop-in.
    if [ -f "$LIGHTDM_OLD_DROPIN" ]; then
        rm -f "$LIGHTDM_OLD_DROPIN"
        rmdir "$(dirname "$LIGHTDM_OLD_DROPIN")" 2>/dev/null || true
    fi
    # Only rewrite the override when it is missing or still contains the DRM
    # devices.  The installer pre-writes it, so the normal "exit to desktop"
    # path does not need a daemon-reload (which costs ~4 s on the Pi Zero).
    if [ ! -f "$LIGHTDM_UNIT" ] || grep -q 'dev-dri-' "$LIGHTDM_UNIT"; then
        if [ -f "$LIGHTDM_VENDOR" ]; then
            tmp="$(mktemp)" || exit 1
            # Remove the two device tokens wherever they appear (start, middle
            # or end of a dependency list) and tidy the leftover whitespace.
            sed -E 's/[[:space:]]*dev-dri-(card0|renderD128)\.device[[:space:]]*/ /g
                    s/[[:space:]]+$//
                    s/= /=/' "$LIGHTDM_VENDOR" > "$tmp"
            cat "$tmp" > "$LIGHTDM_UNIT"
            rm -f "$tmp"
        else
            cat > "$LIGHTDM_UNIT" <<'EOF'
[Unit]
Description=Light Display Manager
Documentation=man:lightdm(1)
After=systemd-user-sessions.service
Conflicts=plymouth-quit.service
After=plymouth-quit.service
OnFailure=plymouth-quit.service

[Service]
ExecStart=/usr/sbin/lightdm
Restart=always
BusName=org.freedesktop.DBus

[Install]
Alias=display-manager.service
EOF
        fi
        LIGHTDM_CHANGED=1
        log "LightDM: 不再等待 /dev/dri/card0（完整 unit 覆盖）"
    fi
fi

# ---------------------------------------------------------------------------
# 2. Find the interactive user.  install.sh writes CAMERA_HOME to
#    /etc/default/zerosnap for exactly this purpose.
# ---------------------------------------------------------------------------
CAMERA_HOME=""
if [ -f "$DEFAULTS" ]; then
    # shellcheck disable=SC1090
    . "$DEFAULTS"
fi
if [ -z "${CAMERA_HOME:-}" ] && [ -f "$PASSWD" ]; then
    CAMERA_HOME="$(awk -F: '$3 >= 1000 && $6 ~ /^\/home\// {print $6; exit}' "$PASSWD")"
fi

USER_NAME=""
USER_UID=""
USER_GID=""
USER_DIR=""
if [ -n "${CAMERA_HOME:-}" ] && [ -f "$PASSWD" ]; then
    USER_NAME="$(awk -F: -v home="$CAMERA_HOME" '$6 == home {print $1; exit}' "$PASSWD")"
    if [ -n "$USER_NAME" ]; then
        USER_UID="$(awk -F: -v user="$USER_NAME" '$1 == user {print $3; exit}' "$PASSWD")"
        USER_GID="$(awk -F: -v user="$USER_NAME" '$1 == user {print $4; exit}' "$PASSWD")"
        USER_DIR="$ROOT$CAMERA_HOME"
    fi
fi

# ---------------------------------------------------------------------------
# 3. LightDM autologin session + the two per-user copies.
#
# Three files can override each other: lightdm.conf, ~/.dmrc and AccountsService.
# The old camera installer set all three to `camera`; if any one is left behind,
# LightDM starts the camera again instead of the desktop.
# ---------------------------------------------------------------------------
python3 - "$ROOT" "$SESSION" "$USER_NAME" "$USER_DIR" "$USER_UID" "$USER_GID" <<'PY'
import os
import re
import sys
import tempfile

root, session, user, user_dir, uid, gid = sys.argv[1:7]
uid = int(uid) if uid else 0
gid = int(gid) if gid else 0


def read_lines(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return handle.readlines()
    except FileNotFoundError:
        return []


def write_lines(path, lines, want_uid=0, want_gid=0, mode=0o644):
    directory = os.path.dirname(path) or "."
    try:
        os.makedirs(directory, exist_ok=True)
    except OSError:
        pass
    try:
        st = os.stat(path)
        want_mode = st.st_mode & 0o7777
        if not os.access(path, os.W_OK):
            want_mode = mode
    except FileNotFoundError:
        st = None
        want_mode = mode

    fd, tmp = tempfile.mkstemp(prefix=".zerosnap-", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.writelines(lines)
        os.chmod(tmp, want_mode)
        try:
            os.chown(tmp, st.st_uid if st is not None else want_uid,
                     st.st_gid if st is not None else want_gid)
        except PermissionError:
            pass
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def replace_key(lines, key, value, section):
    """Replace `key=...`, or add it to [section]."""
    out = []
    found = False
    for line in lines:
        stripped = line.strip()
        if not stripped.startswith("#") and re.match(rf"^{re.escape(key)}\s*=", stripped):
            if not found:
                out.append(f"{key}={value}\n")
                found = True
            continue
        out.append(line)

    if found:
        return out

    header = f"[{section}]"
    for index, line in enumerate(out):
        if line.strip() == header:
            out.insert(index + 1, f"{key}={value}\n")
            return out

    if out and not out[-1].endswith("\n"):
        out[-1] += "\n"
    if out and out[-1].strip():
        out.append("\n")
    out.append(header + "\n")
    out.append(f"{key}={value}\n")
    return out


# lightdm.conf: run the desktop session, not the old camera X session.
lightdm = os.path.join(root, "etc/lightdm/lightdm.conf")
if os.path.exists(lightdm):
    write_lines(lightdm, replace_key(read_lines(lightdm), "autologin-session",
                                     session, "Seat:*"))
    print(f"[exit-to-desktop] lightdm autologin-session={session}")
else:
    print(f"[exit-to-desktop] warning: {lightdm} not found")

if user and user_dir:
    # ~/.dmrc (owned by the desktop user).
    dmrc = os.path.join(user_dir, ".dmrc")
    write_lines(dmrc, replace_key(read_lines(dmrc), "Session", session, "Desktop"),
                want_uid=uid, want_gid=gid, mode=0o644)
    print(f"[exit-to-desktop] {dmrc}: Session={session}")

    # AccountsService overrides .dmrc when it exists.
    accounts = os.path.join(root, f"var/lib/AccountsService/users/{user}")
    write_lines(accounts,
                replace_key(read_lines(accounts), "XSession", session, "User"),
                want_uid=0, want_gid=0, mode=0o600)
    print(f"[exit-to-desktop] {accounts}: XSession={session}")
PY

# ---------------------------------------------------------------------------
# 4. Prepare-only (installer) stops here.
# ---------------------------------------------------------------------------
if [ "$PREPARE" -eq 1 ]; then
    log "桌面配置已准备"
    exit 0
fi

# ---------------------------------------------------------------------------
# 5. Start LightDM without blocking this transient unit.
#
# zerosnap-ui.service has Conflicts=lightdm.service, so queuing the LightDM
# start is enough to stop the camera.  This helper runs from its own unit
# (zerosnap-desktop.service), not from the camera's cgroup, so it survives that
# stop and can finish the configuration.
# ---------------------------------------------------------------------------
if ! command -v systemctl >/dev/null 2>&1; then
    log "错误：没有 systemctl，无法启动桌面"
    exit 1
fi

if systemctl is-enabled --quiet fbcp.service 2>/dev/null; then
    systemctl disable fbcp.service >>/dev/null 2>&1 || true
    log "已停用 fbcp.service"
fi
systemctl stop fbcp.service >>/dev/null 2>&1 || true

# LXDE's pcmanfm/lxpanel use GVolumeMonitor.  If udisksd is not already up,
# gvfs-udisks2-volume-monitor hits a known SEGV and the desktop comes up black
# with only a mouse cursor.  graphical.target normally starts these at boot;
# make sure here as well (cheap when they are already active).
for unit in udisks2.service accounts-daemon.service; do
    if ! systemctl is-active --quiet "$unit" 2>/dev/null; then
        systemctl start "$unit" >>/dev/null 2>&1 || true
        log "已启动 $unit"
    fi
done

# Pick up a freshly written override.  In the common case the installer
# already wrote it, so skip the ~4 s daemon-reload unless the unit that systemd
# currently has loaded still asks for the DRM devices.
if [ "$LIGHTDM_CHANGED" -eq 1 ] \
    || systemctl show -p Wants --value lightdm.service 2>/dev/null | grep -q 'dev-dri'; then
    systemctl daemon-reload >>/dev/null 2>&1 || true
fi

log "启动 LightDM 桌面"
systemctl --no-block start lightdm.service

#!/bin/bash
# Desktop launcher: stop the LXDE session and go back to ZeroSnap.
#
# The Pi image gives the normal user passwordless sudo, so the desktop icon can
# call this file directly.  `--no-block` matters: LightDM's session is torn down
# as zerosnap-ui.service starts (Conflicts=lightdm.service), which kills the
# terminal this command was launched from.
set -u

if [ "$(id -u)" -ne 0 ]; then
    if sudo -n true 2>/dev/null; then
        exec sudo -n "$0" "$@"
    fi
    if command -v pkexec >/dev/null 2>&1; then
        exec pkexec "$0" "$@"
    fi
    echo "需要 root 权限才能切换回相机" >&2
    exit 1
fi

if ! command -v systemctl >/dev/null 2>&1; then
    echo "没有 systemctl，无法切换回相机" >&2
    exit 1
fi

systemctl --no-block start zerosnap-ui.service

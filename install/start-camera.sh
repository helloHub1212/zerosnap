#!/bin/bash
# Native camera UI launcher: no X11, no SDL, no window manager.
#
# The C++ binary owns /dev/fb1 and the touch device; a Python daemon owns the
# camera and the encoder.  They talk through /dev/shm.  This script starts
# both, keeps the daemon alive while the UI runs, and leaves the console
# visible (so the error is readable on the LCD) if the UI keeps crashing.
set -uo pipefail

APP_DIR=/opt/zerosnap
BIN_DIR="$APP_DIR/bin"
UI="$BIN_DIR/zerosnap-ui"
DAEMON_PID=""

# The installer records which user owns the media/session so the daemon writes
# its cache where the person holding the device will look for it.  The old
# build hardcoded /home/camera, which is not a user on this card.
if [ -f /etc/default/zerosnap ]; then
    # shellcheck disable=SC1091
    . /etc/default/zerosnap
fi
if [ -z "${CAMERA_HOME:-}" ] || [ ! -d "${CAMERA_HOME:-}" ]; then
    CAMERA_HOME="$(getent passwd | awk -F: '$3 >= 1000 && $6 ~ /^\/home\// {print $6; exit}')"
fi
if [ -n "${CAMERA_HOME:-}" ] && [ -d "$CAMERA_HOME" ]; then
    HOME="$CAMERA_HOME"
else
    HOME="${HOME:-/root}"
fi
export HOME

CACHE_DIR="$HOME/.cache/zerosnap"
mkdir -p "$CACHE_DIR"

# One fixed, always-findable log, plus a symlink in the user's cache so both
# "~/.cache/zerosnap/app.log" and "/var/log/zerosnap/app.log" work.
mkdir -p /var/log/zerosnap
LOG=/var/log/zerosnap/app.log
ln -sfn "$LOG" "$CACHE_DIR/app.log" 2>/dev/null || true
export CAMERA_LOG_PATH="$LOG"
COUNTER="$CACHE_DIR/crash-count"
FIRST_FAIL="$CACHE_DIR/crash-first"

log() {
    printf '%s %s\n' "$(date '+%F %T')" "$*" >> "$LOG"
}

{
    printf '\n===== native camera start %s =====\n' "$(date '+%F %T %Z')"
    printf 'user: %s (uid=%s)\n' "$(id -un)" "$(id -u)"
    printf 'kernel: %s\n' "$(uname -a)"
    printf 'python: %s\n' "$(python3 --version 2>&1 || true)"
} >> "$LOG"

# The kernel console shares fb1 with us; stop it from painting log lines over
# the camera UI.  Panics still get through.
if [ -w /proc/sys/kernel/printk ]; then
    printf '1 4 1 7\n' > /proc/sys/kernel/printk || true
fi
mkdir -p /dev/shm
chmod 1777 /dev/shm 2>/dev/null || true

wait_for_fb() {
    local count=0
    while [ "$count" -lt 80 ]; do
        if [ -e /dev/fb1 ]; then
            return 0
        fi
        sleep 0.1
        count=$((count + 1))
    done
    return 1
}

if wait_for_fb; then
    log "framebuffer /dev/fb1 已就绪"
else
    log "警告：等待 /dev/fb1 超时（8 秒），继续启动"
fi

# Compile the small C helpers on first run (I420->RGB, RGB888->RGB565).
NATIVE_SRC="$APP_DIR/app/backend/native/yuv420.c"
NATIVE_LIB=/opt/zerosnap/lib/libyuv420.so
# The installer prebuilds this.  Only compile when it is missing or stale:
# compiling at boot used to delay the daemon by 8 s, which is exactly how the
# UI missed the shared-memory window.
if [ -f "$NATIVE_SRC" ] && { [ ! -f "$NATIVE_LIB" ] || [ "$NATIVE_SRC" -nt "$NATIVE_LIB" ]; }; then
    if command -v gcc >/dev/null 2>&1; then
        log "编译颜色转换加速模块"
        mkdir -p /opt/zerosnap/lib 2>/dev/null || true
        if ! gcc -O3 -march=armv6 -mfpu=vfp -mfloat-abi=hard -shared -fPIC \
                -o "$NATIVE_LIB.tmp" "$NATIVE_SRC" >>"$LOG" 2>&1; then
            rm -f "$NATIVE_LIB.tmp"
            NATIVE_LIB="$CACHE_DIR/libyuv420.so"
            log "警告：/opt 不可写，改编译到 $NATIVE_LIB"
            gcc -O3 -march=armv6 -mfpu=vfp -mfloat-abi=hard -shared -fPIC \
                -o "$NATIVE_LIB.tmp" "$NATIVE_SRC" >>"$LOG" 2>&1 || rm -f "$NATIVE_LIB.tmp"
        fi
        if [ -f "$NATIVE_LIB.tmp" ]; then
            mv -f "$NATIVE_LIB.tmp" "$NATIVE_LIB"
            log "颜色转换模块就绪：$NATIVE_LIB"
        fi
    fi
fi

if [ ! -x "$UI" ]; then
    log "错误：找不到 $UI（先在电脑上运行 native/build-arm.sh）"
    printf 'zerosnap-ui 不存在：%s\n' "$UI"
    exit 1
fi

export PYTHONPATH="$APP_DIR${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONUNBUFFERED=1
cd "$APP_DIR" || exit 1

# The daemon is supervised in its own process group: if picamera2 or the
# encoder dies, it comes back on its own instead of leaving the UI on
# "camera not started" until the next reboot.
daemon_supervisor() {
    local n=0
    while true; do
        python3 -m app.daemon >> "$LOG" 2>&1
        local rc=$?
        n=$((n + 1))
        log "daemon 退出（rc=$rc，第 $n 次），2 秒后重启"
        sleep 2
    done
}

start_daemon() {
    set -m
    daemon_supervisor &
    DAEMON_PID=$!
    set +m
    log "daemon 监督进程已启动（pgid=$DAEMON_PID）"
}

stop_daemon() {
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        # Negative PGID: the supervisor and whatever python it spawned.
        kill -TERM -- "-$DAEMON_PID" 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
            kill -0 "$DAEMON_PID" 2>/dev/null || break
            sleep 0.2
        done
        kill -KILL -- "-$DAEMON_PID" 2>/dev/null || true
    fi
    DAEMON_PID=""
}

cleanup() {
    stop_daemon
}
trap cleanup EXIT INT TERM

read_int() {
    local value=""
    if [ -f "$1" ]; then
        value="$(cat "$1" 2>/dev/null || true)"
    fi
    if [[ "$value" =~ ^[0-9]{1,12}$ ]]; then
        printf '%s' "$value"
    else
        printf '%s' "${2:-0}"
    fi
}

count="$(read_int "$COUNTER" 0)"
first="$(read_int "$FIRST_FAIL" 0)"
if [ "$count" -ge 3 ] && [ "$first" -gt 0 ]; then
    now="$(date +%s)"
    if [ $((now - first)) -le 120 ]; then
        log "启动在 120 秒内连续失败 $count 次，停止重试；console 留在 tty1 上便于排查"
        printf '0\n' > "$COUNTER"
        printf '0\n' > "$FIRST_FAIL"
        exit 1
    fi
fi

start_daemon

started="$(date +%s)"
"$UI" >> "$LOG" 2>&1
rc=$?
finished="$(date +%s)"
duration=$((finished - started))
[ "$duration" -lt 0 ] && duration=0
stop_daemon

if [ "$rc" -eq 0 ]; then
    log "zerosnap-ui 正常退出"
    printf '0\n' > "$COUNTER"
    printf '0\n' > "$FIRST_FAIL"
    exit 0
fi

log "zerosnap-ui 异常退出（rc=$rc，运行 ${duration}s）"

if [ "$duration" -gt 60 ]; then
    count=0
    first=0
else
    count="$(read_int "$COUNTER" 0)"
    first="$(read_int "$FIRST_FAIL" 0)"
    now="$(date +%s)"
    if [ "$count" -eq 0 ] || [ "$first" -le 0 ] || [ $((now - first)) -gt 120 ]; then
        count=1
        first="$now"
    else
        count=$((count + 1))
    fi
fi
printf '%s\n' "$count" > "$COUNTER"
printf '%s\n' "$first" > "$FIRST_FAIL"

if [ "$count" -ge 3 ]; then
    log "120 秒内连续失败 $count 次，停止重启；console 留在 tty1 上便于排查"
    printf '0\n' > "$COUNTER"
    printf '0\n' > "$FIRST_FAIL"
    exit 1
fi

exit "$rc"

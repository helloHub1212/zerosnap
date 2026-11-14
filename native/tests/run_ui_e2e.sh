#!/bin/bash
# End-to-end check of the C++ UI: a fake daemon publishes a frame through
# /dev/shm, the UI must render it into the framebuffer.
#
# Runs entirely on the build host: the "framebuffer" is a plain file.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
WORK=/tmp/opencode/ui_e2e
PY="${PYTHON:-python3}"

rm -rf "$WORK"
mkdir -p "$WORK"

cd "$ROOT" || exit 1
make -s zerosnap-ui || { echo "FAIL build"; exit 1; }

"$PY" - "$ROOT" >"$WORK/producer.log" 2>&1 <<'PY' &
import os, sys, time
root = sys.argv[1]
sys.path.insert(0, os.path.join(root, "..", "app"))
from camera_shm import CameraShm

shm = CameraShm(create=True)
w, h = 320, 240
# A solid magenta frame: easy to assert on after the UI has drawn it.
pix = bytes([0x1F, 0xF8]) * (w * h)  # RGB565 0xF81F, magenta
frame = bytearray(pix)
shm.publish_state(
    mode=0, recording=0, video_mode=0, flags=4, rec_seconds=0,
    photo_count=2, video_count=1, free_mb=9000, battery_pct=50,
    last_photo="IMG_0002.jpg", last_video="VID_0001.mp4", status_text="",
)
start = time.time()
while time.time() - start < 6.0:
    # Toggle the top-left pixel so the sequence number really changes.
    frame[0] ^= 0x01
    shm.publish_frame(frame, w, h)
    time.sleep(0.03)
shm.close()
PY
PRODUCER=$!

# Give the producer a moment to create the segment.
for _ in $(seq 1 50); do
    [ -e /dev/shm/zerosnap-shm ] && break
    sleep 0.1
done

"$PY" - "$ROOT" >"$WORK/ui.log" 2>&1 -- || true
"$ROOT/zerosnap-ui" --fb "$WORK/fb.img" --frames 45 >>"$WORK/ui.log" 2>&1
UI_RC=$?
kill "$PRODUCER" 2>/dev/null || true
wait "$PRODUCER" 2>/dev/null || true

"$PY" - "$WORK/fb.img" <<'PY'
import sys
path = sys.argv[1]
data = open(path, "rb").read()
if len(data) < 320 * 240 * 2:
    print("FAIL framebuffer too small")
    raise SystemExit(1)
# Centre of the preview area should be the magenta frame we published.
off = (120 * 320 + 120) * 2
value = data[off] | (data[off + 1] << 8)
r = (value >> 11) & 0x1F
g = (value >> 5) & 0x3F
b = value & 0x1F
ok = r > 20 and g < 8 and b > 20
print(("PASS" if ok else "FAIL") + f" centre pixel rgb565=0x{value:04x} (r={r} g={g} b={b})")
# The right-hand column must be the dark sidebar, not the frame.
off2 = (236 * 320 + 316) * 2
v2 = data[off2] | (data[off2 + 1] << 8)
print(("PASS" if v2 < 0x1084 else "FAIL") + f" sidebar pixel rgb565=0x{v2:04x}")
raise SystemExit(0 if ok else 1)
PY
ASSERT_RC=$?

echo "ui exit=$UI_RC producer log:"
sed 's/^/  /' "$WORK/producer.log" | tail -5
sed 's/^/  /' "$WORK/ui.log" | tail -5

if [ "$UI_RC" -ne 0 ]; then
    echo "FAIL zerosnap-ui exited $UI_RC"
    exit 1
fi
if [ "$ASSERT_RC" -ne 0 ]; then
    echo "FAIL framebuffer assertions"
    exit 1
fi
echo "UI E2E PASS"

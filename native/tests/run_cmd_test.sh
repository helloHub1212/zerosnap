#!/bin/bash
# Cross-language command test: C++ sends a shutter command, Python receives it.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT=/tmp/opencode/cmd_test
PY="${PYTHON:-python3}"
mkdir -p "$OUT"
cd "$ROOT" || exit 1
rm -f /dev/shm/zerosnap-shm 2>/dev/null || true
gcc -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -Isrc \
    -o "$OUT/sender" tests/cmd_sender.c src/ipc.c -lrt || exit 1
"$PY" tests/cmd_receiver.py & RECV=$!
sleep 0.6
"$OUT/sender"
wait $RECV
rc=$?
rm -f /dev/shm/zerosnap-shm 2>/dev/null || true
if [ $rc -ne 0 ]; then echo "CMD TEST FAILED"; exit 1; fi
echo "CMD TEST PASS"

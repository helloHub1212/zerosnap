#!/bin/bash
# Verify the two-tap calibration recovers every axis transform.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT=/tmp/opencode/touchcal_test
mkdir -p /tmp/opencode
cd "$ROOT" || exit 1
gcc -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -Isrc \
    -o "$OUT" tests/touchcal_test.c src/touchcal.c || exit 1
"$OUT"

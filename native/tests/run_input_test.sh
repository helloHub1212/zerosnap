#!/bin/sh
# Build and run the host-side input parser tests.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-gcc}
BIN="${TMPDIR:-/tmp}/zerosnap-input-test.$$"

trap 'rm -f "$BIN"' EXIT INT TERM

"$CC" -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE \
    -o "$BIN" \
    "$ROOT/tests/input_test.c" \
    "$ROOT/src/input.c"

"$BIN"

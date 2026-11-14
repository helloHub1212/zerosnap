#!/usr/bin/env bash
# Regenerate the font tables, then compile and run the host-side text test.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

py="${PYTHON:-/tmp/opencode/venv/bin/python}"
if [[ ! -x "$py" ]]; then
    py=python3
fi

if [ -f assets/DroidSansFallbackFull.ttf ] && [ -f assets/Piboto-Regular.ttf ]; then
    "$py" tools/gen_font.py
else
    echo "text_test: bundled fonts not present, testing the committed text_data.c"
fi

out="/tmp/opencode"
mkdir -p "$out"

cflags=(-std=c11 -Wall -Wextra -Werror -Isrc)

gcc "${cflags[@]}" -O2 \
    src/text.c src/text_data.c tests/text_test.c \
    -o "$out/text_test"
"$out/text_test"

# Second pass with sanitizers so clipped draws are proven in-bounds.  On
# 32-bit ARM with the distro toolchain libasan may need libatomic; skip the
# pass instead of failing the suite when it cannot link.
if gcc "${cflags[@]}" -O1 -g -fsanitize=address,undefined \
        -fno-omit-frame-pointer \
        src/text.c src/text_data.c tests/text_test.c \
        -o "$out/text_test_asan" 2>"$out/text_asan_build.log"; then
    "$out/text_test_asan"
else
    echo "text_test: sanitizer build skipped ($(head -1 "$out/text_asan_build.log"))"
fi

echo "text_test: PPM at /tmp/opencode/text_test.ppm"

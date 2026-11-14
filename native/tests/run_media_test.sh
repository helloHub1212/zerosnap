#!/bin/sh
# Build src/media.c plus tests/media_test.c on the host and run the tests.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname -- "$here")
out=/tmp/opencode/media_test
cc=${CC:-gcc}
cflags="-std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE"

mkdir -p "$out"

# media.c must compile clean on its own, not just through the test.
"$cc" $cflags -c "$root/src/media.c" -o "$out/media.o"

"$cc" $cflags -o "$out/media_test" \
    "$root/src/media.c" "$root/tests/media_test.c"

exec "$out/media_test"

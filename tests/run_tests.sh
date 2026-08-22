#!/bin/sh
# Full test sweep: unit tests, then golden-frame comparison.
# Requires nothing but a C99 compiler, sha256sum and sh.
set -e
cd "$(dirname "$0")/.."

CC=${CC:-cc}
CFLAGS="-std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=199309L"
mkdir -p build

echo "== building (warnings are errors) =="
$CC $CFLAGS -Icore/include -o build/test_core \
    tests/test_core.c core/src/vn_gfx.c core/src/vn_text.c

echo "== unit tests =="
./build/test_core

echo "== golden frames =="
make -s frames >/dev/null
if [ ! -f tests/golden/manifest.sha256 ]; then
    echo "  no manifest; run tests/bless.sh to create one"
    exit 1
fi
# The renderer uses no floating point and no wall-clock input, so frames
# must be bit-identical on every platform. A mismatch is a real
# regression, not a tolerance problem.
( cd build/frames && sha256sum -c ../../tests/golden/manifest.sha256 )

echo "== all green =="

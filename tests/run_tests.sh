#!/bin/sh
# Full test sweep. Requires nothing but a C99 compiler, sha256sum and sh.
set -e
cd "$(dirname "$0")/.."

CC=${CC:-cc}
CFLAGS="-std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=199309L"
CORE="core/src/vn_gfx.c core/src/vn_text.c core/src/vn_vm.c core/src/vn_save.c"
mkdir -p build

echo "== building (warnings are errors) =="
$CC $CFLAGS -Icore/include -o build/vnc tools/vnc/vnc.c
$CC $CFLAGS -Icore/include -o build/test_core tests/test_core.c $CORE
$CC $CFLAGS -Icore/include -o build/test_vm   tests/test_vm.c   $CORE
$CC $CFLAGS -Icore/include -o build/test_save tests/test_save.c $CORE

echo "== compiling test scripts =="
./build/vnc demo/script/demo.vn build/demo.vnb build/demo.vnstr
for v in base edit_other edit_same add_var; do
    ./build/vnc tests/data/$v.vn build/$v.vnb build/$v.vnstr
done

echo "== unit tests =="
./build/test_core
./build/test_vm
./build/test_save build/demo.vnb build/demo.vnstr \
    build/base.vnb build/base.vnstr \
    build/edit_other.vnb build/edit_other.vnstr \
    build/edit_same.vnb build/edit_same.vnstr \
    build/add_var.vnb build/add_var.vnstr

echo "== compiler determinism =="
# Same source must produce byte-identical output, or golden frames and
# save hashes are meaningless.
./build/vnc demo/script/demo.vn build/determinism.vnb build/determinism.vnstr >/dev/null 2>&1
cmp build/demo.vnb build/determinism.vnb
cmp build/demo.vnstr build/determinism.vnstr
echo "  bytecode is reproducible"

echo "== decompiler agrees with the VM =="
# The decompiler and the VM share one instruction-length table. If they
# ever disagreed, execution and save offsets would silently corrupt, so
# check that a full walk lands exactly on the end of the code section.
./build/vnc -d build/demo.vnb build/demo.vnstr > build/demo.disasm
if grep -q "bad opcode" build/demo.disasm; then
    echo "  FAIL: decompiler could not walk the whole code section"
    exit 1
fi
echo "  walked $(grep -c '^  [0-9a-f]\{4\}:' build/demo.disasm) instructions cleanly"

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

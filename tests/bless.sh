#!/bin/sh
# Regenerate the golden-frame manifest. Run this deliberately, only
# after eyeballing the frames and agreeing the new output is correct.
set -e
cd "$(dirname "$0")/.."
make -s frames >/dev/null
mkdir -p tests/golden
( cd build/frames && sha256sum *.ppm > ../../tests/golden/manifest.sha256 )
echo "blessed $(wc -l < tests/golden/manifest.sha256) frames"

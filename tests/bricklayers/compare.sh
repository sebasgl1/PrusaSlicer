#!/bin/bash
# Compares the C++ Brick Layers port against the reference bricklayers.py.
#
#   tests/bricklayers/compare.sh /path/to/BrickLayers [file.gcode ...]
#
# Builds the standalone driver, then runs both implementations with several
# parameter sets and chunk sizes and checks that the outputs are byte-identical.
# Without explicit files it uses the samples shipped with the BrickLayers repo.
set -euo pipefail

BL_REPO=${1:?usage: compare.sh /path/to/GeekDetour/BrickLayers [file.gcode ...]}
shift
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

g++ -std=c++17 -O2 -Wall -Wextra -I "$ROOT/src/libslic3r/src" \
    "$ROOT/src/libslic3r/src/libslic3r/GCode/BrickLayers.cpp" \
    "$ROOT/tests/bricklayers/bricklayers_cli.cpp" -o "$WORK/bricklayers_cli"

FILES=("$@")
if [ ${#FILES[@]} -eq 0 ]; then
    FILES=("$BL_REPO"/sample_gcode/*.gcode)
fi

PARAMS=(
    "-extrusionMultiplier 1.05 -startAtLayer 3"
    "-extrusionMultiplier 1.1 -startAtLayer 1"
    "-extrusionMultiplier 0.97 -startAtLayer 7"
    "-extrusionMultiplier 1.05 -startAtLayer 2 -ignoreLayers 10 11 12"
)

fail=0
for f in "${FILES[@]}"; do
    for p in "${PARAMS[@]}"; do
        # shellcheck disable=SC2086
        python3 "$BL_REPO/bricklayers.py" "$f" -outputFile "$WORK/py.gcode" $p -noHeader -noLogging -verbosity 0
        for chunk in 0 7 65536; do
            # shellcheck disable=SC2086
            "$WORK/bricklayers_cli" "$f" "$WORK/cpp.gcode" $p -chunk $chunk 2>/dev/null
            if cmp -s "$WORK/py.gcode" "$WORK/cpp.gcode"; then
                echo "ok    [$p] chunk=$chunk $(basename "$f")"
            else
                echo "DIFF  [$p] chunk=$chunk $(basename "$f")"
                fail=1
            fi
        done
    done
done
exit $fail

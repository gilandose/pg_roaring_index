#!/usr/bin/env bash
set -euo pipefail

# Download the CRoaring amalgamation into src/vendor/croaring/.
# Usage: bash scripts/fetch-croaring.sh [version]

VERSION="${1:-4.2.1}"
BASE="https://github.com/RoaringBitmap/CRoaring/releases/download/v${VERSION}"
DEST="src/vendor/croaring"

mkdir -p "$DEST"

echo "Fetching CRoaring v${VERSION}..."
curl -fsSL "$BASE/roaring.c" -o "$DEST/roaring.c"
curl -fsSL "$BASE/roaring.h" -o "$DEST/roaring.h"

echo "Done. Files in $DEST/"

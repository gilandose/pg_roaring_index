#!/usr/bin/env bash
# Copy project git hooks into .git/hooks/ and make them executable.
# Run once after cloning: bash scripts/install-hooks.sh

set -euo pipefail
HOOKS_SRC="scripts/hooks"
HOOKS_DST=".git/hooks"

if [ ! -d "$HOOKS_SRC" ]; then
    echo "No scripts/hooks/ directory found — nothing to install."
    exit 0
fi

for hook in "$HOOKS_SRC"/*; do
    name="$(basename "$hook")"
    cp "$hook" "$HOOKS_DST/$name"
    chmod +x "$HOOKS_DST/$name"
    echo "Installed $name"
done

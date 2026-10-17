#!/usr/bin/env bash
# Exports this fork's committed tree (HEAD) as an engine source directory, without the GUI's large resource folders,
# exactly like Nozzle It All's platform scripts export their pinned commit. Usage: export_source.sh DEST
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; DEST="$1"; REV="$(git -C "$ROOT" rev-parse HEAD)"
if [ "$(cat "$DEST/.engine-rev" 2>/dev/null)" != "$REV" ]; then
  rm -rf "$DEST"; mkdir -p "$DEST"
  git -C "$ROOT" archive --format=tar "$REV" -- . ':!resources/profiles' ':!resources/web' ':!resources/images' \
    ':!resources/calib' ':!resources/handy_models' | tar -x -C "$DEST"
  echo "$REV" > "$DEST/.engine-rev"
fi
echo "$DEST @ $REV"

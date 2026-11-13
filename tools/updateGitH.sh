#!/bin/bash
# Writes main/gitVersion.h (GIT_VERSION, from git describe), only when it
# changed, so builds that include it don't recompile for nothing. Run by
# main/CMakeLists.txt and csim/a800/Makefile on every build, and by
# boot.sh/bench-idf.sh; works from any directory.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/main/gitVersion.h"
TMP="$(mktemp "${TMPDIR:-/tmp}/gitVersion.h.XXXXXX")"
trap 'rm -f "$TMP"' EXIT
printf '#define GIT_VERSION "%s"\n' "$(git -C "$ROOT" describe --abbrev=6 --dirty --always)" > "$TMP"
cmp -s "$TMP" "$OUT" || cat "$TMP" > "$OUT"

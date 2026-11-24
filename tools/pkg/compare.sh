#!/usr/bin/env bash
# Integration test of tools/pkg: extracts a package and compares it, file by file and byte by
# byte, with a reference extraction of the same package (by another tool, say).
# Usage: compare.sh PKG REFERENCE_DIR [PKG_TOOL]
set -euo pipefail

(($# == 2 || $# == 3)) || { echo "usage: $0 PKG REFERENCE_DIR [PKG_TOOL]" >&2; exit 2; }
readonly ROOT=$(cd "$(dirname "$0")/../.." && pwd)
readonly PKG=$1 REFERENCE=$2 TOOL=${3:-$ROOT/.build/out/pspkg}
readonly OUT=$(mktemp -d "$ROOT/.build/pkg-compare.XXXXXX")
trap 'rm -rf "$OUT"' EXIT

"$TOOL" extract "$PKG" "$OUT/game" >/dev/null
# Every folder of the reference is one the package lists (sce_sys is made from entries; the
# package may hold empty folders an extraction leaves out).
"$TOOL" list "$PKG" | sed -n 's/^folder //p' | sort >"$OUT/listed"
(cd "$REFERENCE" && find . -type d | sed 's#^\./##' | grep -v '^sce_sys' | sort) >"$OUT/expected"
missing=$(comm -23 "$OUT/expected" "$OUT/listed")
if [[ -n $missing ]]; then
    echo "folders of $REFERENCE the package does not list:" >&2
    head <<<"$missing" >&2
    exit 1
fi
if diff -rq "$REFERENCE" "$OUT/game"; then
    echo "same: $(find "$OUT/game" -type f | wc -l) files, $(wc -l <"$OUT/listed") folders"
else
    echo "differs from $REFERENCE" >&2
    exit 1
fi

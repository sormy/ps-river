#!/usr/bin/env bash
# Unpacks a PS4 pkg built from a dump (a "fake" pkg) into games/<NAME>, with tools/pkg (built by
# ./build.sh).
# Usage: unpack.sh game.pkg [NAME], NAME defaulting to the title id (<TITLE>-UPDATE for an update).
set -euo pipefail

readonly ROOT=$(cd "$(dirname "$0")" && pwd)
readonly GAMES=$ROOT/games
readonly PKG_TOOL=$ROOT/.build/out/pspkg

die() {
    echo "error: $*" >&2
    exit 1
}

title_of() {
    head -c 4096 "$1" | grep -aoE 'CUSA[0-9]{5}' | head -1
}

# A pkg built from a dump carries the project it was built from (*.gp4), listing every file.
check_complete() {
    local game=$1 project listed missing
    for project in "$game"/*.gp4; do
        [[ -f $project ]] || continue
        listed=0 missing=0
        while read -r path; do
            listed=$((listed + 1))
            [[ -e $game/$path ]] || missing=$((missing + 1))
        done < <(grep -oE 'targ_path="[^"]+"' "$project" | cut -d'"' -f2 | grep -v '^sce_sys/')
        ((missing == 0)) ||
            die "$(basename "$project") lists $listed files, $missing are not in the pkg: an incomplete copy"
    done
}

main() {
    (($# == 1 || $# == 2)) || die "usage: $0 game.pkg [NAME]"
    local pkg=$1 name=${2:-$(title_of "$1")}
    [[ -n $name ]] || die "$pkg has no title id: not a PS4 pkg"
    local target=$GAMES/$name staging=$GAMES/.unpack-$name
    [[ ! -e $target ]] || die "$target exists already"

    [[ -x $PKG_TOOL ]] || die "$PKG_TOOL missing: run ./build.sh first"
    rm -rf "$staging"
    "$PKG_TOOL" extract "$pkg" "$staging" ||
        die "could not unpack $pkg: only pkgs built from a dump open with the zero passcode"
    [[ -f $staging/sce_sys/param.sfo ]] || die "$pkg unpacked without a param.sfo"
    check_complete "$staging"
    mv "$staging" "$target"
    echo "Unpacked to $target"
}

main "$@"

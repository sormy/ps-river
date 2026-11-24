#!/usr/bin/env bash
# Plays a PS4 game, unpacked or straight from its pkg (and its update's). Flat by default (a VR title plays on the screen, its camera
# behind the hero); FLAT=0 plays in a headset instead. Per-title settings come from
# flat/<TITLE>.env (SHADPS4_*=value lines; anything already in the environment wins) and
# flat/<TITLE>.mods (edits to the game's own data files, laid over it as <TITLE>-mods). The
# emulator is pointed at .build/run, where <TITLE> (the pkg or folder), <TITLE>-UPDATE and
# <TITLE>-mods lie side by side: it lays the update and the edits over the game by those names.
# Usage: play.sh /path/to/CUSA12392 | play.sh GAME.pkg [UPDATE.pkg] (without UPDATE.pkg, the
# newest update of the title among the pkgs next to GAME.pkg, if any)
set -euo pipefail

readonly ROOT=$(cd "$(dirname "$0")" && pwd)
readonly EMULATOR=$ROOT/.build/out/shadps4
readonly PKG_TOOL=$ROOT/.build/out/pspkg
readonly TITLE_SETTINGS=$ROOT/flat
# RUN_DIR: the run folder (another one for each game run at the same time, as tests do).
readonly RUN=${RUN_DIR:-$ROOT/.build/run}
readonly SFO_MAGIC=00505346

die() {
    echo "error: $*" >&2
    exit 1
}

# Prints the title id of an unpacked game folder.
check_game() {
    local game=$1 sfo=$1/sce_sys/param.sfo
    [[ -f $game/eboot.bin && -f $sfo ]] ||
        die "$game has no eboot.bin + sce_sys/param.sfo: not an unpacked game folder"
    [[ $(head -c 4 "$sfo" | od -An -tx1 | tr -d ' \n') == "$SFO_MAGIC" ]] ||
        die "$sfo is unreadable: the copy is still encrypted; it needs a decrypted dump"
    grep -aoE 'CUSA[0-9]{5}' "$sfo" | head -1
}

# Exports the title's SHADPS4_*=value lines, unless already set; anything else is refused.
load_title_settings() {
    local file=$TITLE_SETTINGS/$1.env line name
    [[ -f $file ]] || return 0
    while IFS= read -r line || [[ -n $line ]]; do
        [[ -z ${line//[[:space:]]/} || $line == \#* ]] && continue
        [[ $line =~ ^(SHADPS4_[A-Z0-9_]+)=([A-Za-z0-9._,:/-]*)$ && $line != *..* ]] ||
            die "$file: not a setting: $line"
        name=${BASH_REMATCH[1]}
        [[ -n ${!name+set} ]] || export "$name=${BASH_REMATCH[2]}"
    done <"$file"
}

# Rebuilds <game>-mods (which the emulator lays over the game) from flat/<TITLE>.mods: lines of
# "file | tag | value", each setting every <tag>…</tag> of that game file (taken from the update
# if it has one) to the value. Only the edits are kept here, never the game's files.
build_mods() {
    local game=$1 mods=$1-mods list
    rm -rf "$mods"
    # GAME_OVERLAY: a folder of game files laid over the game as well (test scenarios'
    # moved levels); the edits apply to them too.
    if [[ -n ${GAME_OVERLAY:-} ]]; then
        mkdir -p "$mods"
        cp -r "$GAME_OVERLAY"/. "$mods"/
    fi
    [[ ${FLAT:-1} == 1 ]] || return 0
    for list in "$TITLE_SETTINGS/$2.mods" ${GAME_MODS:-}; do
        [[ -f $list ]] && apply_mods "$game" "$mods" "$list"
    done
    return 0
}

# The newest update (param.sfo CATEGORY gp) of title $2 among the pkgs in folder $1, if any.
find_update() {
    local folder=$1 title=$2 package
    for package in "$folder"/*.pkg; do
        [[ $("$PKG_TOOL" sfo "$package" TITLE_ID 2>/dev/null) == "$title" &&
            $("$PKG_TOOL" sfo "$package" CATEGORY 2>/dev/null) == gp ]] || continue
        echo "$("$PKG_TOOL" sfo "$package" APP_VER) $package"
    done | sort -V | tail -1 | cut -d' ' -f2-
}

# Copies a game file to $3, the update's if it has one: from the pkgs played, else the folders.
copy_game_file() {
    local game=$1 file=$2 target=$3 package source
    for package in "${update_pkg:-}" "${base_pkg:-}"; do
        [[ -n $package ]] && "$PKG_TOOL" cat "$package" "$file" >"$target" 2>/dev/null && return 0
    done
    for source in "$game-UPDATE/$file" "$game/$file"; do
        [[ -f $source ]] && cp "$source" "$target" && return 0
    done
    return 1
}

# SHADPS4_VR_FLAT_FADE_PICTURE_FROM (title settings): a texture in the game, made a white picture
# for this run and shown over flat play's fade to black (SHADPS4_VR_FLAT_FADE_PICTURE).
fade_picture() {
    local from=${SHADPS4_VR_FLAT_FADE_PICTURE_FROM:-} texture=$RUN/fade-picture.gnf
    [[ ${FLAT:-1} == 1 && -n $from ]] || return 0
    if copy_game_file "$1" "$from" "$texture" &&
        python3 "$ROOT/tools/gnf_picture.py" "$texture" "$RUN/fade-picture.png"; then
        export SHADPS4_VR_FLAT_FADE_PICTURE=$RUN/fade-picture.png
    else
        echo "$0: no fade picture from $from" >&2
    fi
}

apply_mods() {
    local game=$1 mods=$2 list=$3 line file group tag value source
    while IFS= read -r line || [[ -n $line ]]; do
        [[ -z ${line//[[:space:]]/} || $line == \#* ]] && continue
        [[ $line =~ ^([A-Za-z0-9_./-]+)\ *\|\ *(([A-Za-z0-9_]+)/)?([A-Za-z0-9_]+)\ *\|\ *([-+0-9.\ A-Za-z]*[^\ ])\ *$ ]] ||
            die "$list: not an edit: $line"
        file=${BASH_REMATCH[1]} group=${BASH_REMATCH[3]} tag=${BASH_REMATCH[4]} value=${BASH_REMATCH[5]}
        [[ $file != *..* ]] || die "$list: not a path in the game: $file"
        if [[ ! -f $mods/$file ]]; then
            mkdir -p "$(dirname "$mods/$file")"
            copy_game_file "$game" "$file" "$mods/$file" || die "$list: the game has no $file"
        fi
        grep -q "<$tag>" "$mods/$file" || die "$list: $file has no <$tag>"
        if [[ -n $group ]]; then
            grep -q "<$group>" "$mods/$file" || die "$list: $file has no <$group>"
            sed -i -E "/<$group>/,/<\/$group>/ s#(<$tag>)[^<]*(</$tag>)#\1$value\2#g" "$mods/$file"
        else
            sed -i -E "s#(<$tag>)[^<]*(</$tag>)#\1$value\2#g" "$mods/$file"
        fi
    done <"$list"
}

(($# == 1)) || [[ $# == 2 && $1 == *.pkg ]] || die "usage: $0 /path/to/CUSA12392 | $0 GAME.pkg [UPDATE.pkg]"
[[ -x $EMULATOR ]] || die "$EMULATOR missing: run ./build.sh first"
rm -rf "$RUN"
mkdir -p "$RUN"
if [[ $1 == *.pkg ]]; then
    [[ -x $PKG_TOOL ]] || die "$PKG_TOOL missing: run ./build.sh first"
    base_pkg=$(realpath "$1")
    title=$("$PKG_TOOL" sfo "$base_pkg" TITLE_ID) || die "$1 is not a pkg that opens"
    update_pkg=${2:+$(realpath "$2")}
    update_pkg=${update_pkg:-$(find_update "$(dirname "$base_pkg")" "$title")}
    ln -s "$base_pkg" "$RUN/$title.pkg"
    if [[ -n $update_pkg ]]; then
        echo "Update: $update_pkg"
        ln -s "$update_pkg" "$RUN/$title-UPDATE.pkg"
    fi
    boot=$RUN/$title.pkg
else
    title=$(check_game "${1%/}")
    ln -s "$(realpath "$1")" "$RUN/$title"
    [[ ! -d ${1%/}-UPDATE ]] || ln -s "$(realpath "${1%/}-UPDATE")" "$RUN/$title-UPDATE"
    boot=$RUN/$title/eboot.bin
fi
game=$RUN/$title

export SHADPS4_VR_FLAT=${FLAT:-1}
export SHADPS4_VR_FPS_CAP=60
# Streamed (Sunshine says so), the virtual monitor takes the client's resolution.
if [[ -n ${SUNSHINE_CLIENT_WIDTH:-} ]]; then
    "$ROOT/host/display-mode.sh" "$SUNSHINE_CLIENT_WIDTH" "${SUNSHINE_CLIENT_HEIGHT:-1080}"
fi

# The width each eye is drawn at: 2880 by default; for a stream wider than 2560 (Sunshine says
# how wide its client is) wide enough that the picture shown keeps the stream's detail, up to
# the title's largest, 4320.
eye_width() {
    local client=${SUNSHINE_CLIENT_WIDTH:-0}
    if ((client > 2560)); then
        echo $((client * 9 / 8 > 4320 ? 4320 : client * 9 / 8))
    else
        echo 2880
    fi
}
export SHADPS4_TITLE_EYE_WIDTH=${EYE_WIDTH:-$(eye_width)}
load_title_settings "$title"
# GAME_MODS: a further list of edits like flat/<TITLE>.mods (test scenarios, tools/scenario.py).
build_mods "$game" "$title"
fade_picture "$game"

# GAME_ARGS: arguments for the game itself (its own switches, e.g. -room <level>), for tests.
read -ra game_args <<<"${GAME_ARGS:-}"
# FULLSCREEN=false: in a window (several games side by side, as tests run them).
exec "$EMULATOR" --fullscreen "${FULLSCREEN:-true}" -g "$boot" ${game_args[@]+-- "${game_args[@]}"}

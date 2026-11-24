#!/usr/bin/env bash
# Builds the PS4 emulator: shadPS4 at a pinned upstream commit with patches/shadps4/ on top, in
# order: the shadps4-arm64 port, AstroQuest (PSVR emulation) and flat play. Runs the unit tests.
# Also builds tools/pkg (the package reader unpack.sh uses) on the emulator's own libressl.
# Each patch becomes a commit named after it: tools/export_patches.sh writes them back.
set -euo pipefail

readonly UPSTREAM_URL=https://github.com/shadps4-emu/shadPS4.git
readonly UPSTREAM_COMMIT=388cff5177f82e75a667d529212d34f3c255b7fc
readonly OPENXR_URL=https://github.com/KhronosGroup/OpenXR-SDK.git
readonly OPENXR_RELEASE=release-1.1.63

readonly ROOT=$(cd "$(dirname "$0")" && pwd)
readonly SRC=$ROOT/.build/src
readonly OUT=$ROOT/.build/out
readonly PATCHES=$ROOT/patches/shadps4
readonly COMMITTER=(-c user.name="ps-river build" -c user.email=build@localhost)

fetch() {
    if [[ ! -d $SRC/.git ]]; then
        git init -q "$SRC"
        git -C "$SRC" remote add origin "$UPSTREAM_URL"
    fi
    git -C "$SRC" remote set-url origin "$UPSTREAM_URL"
    git -C "$SRC" fetch -q --depth 1 origin "$UPSTREAM_COMMIT"
    git -C "$SRC" checkout -q --force --detach "$UPSTREAM_COMMIT"
    git -C "$SRC" clean -ffdq
}

apply_patches() {
    local patch
    for patch in "$PATCHES"/*.patch; do
        git -C "$SRC" apply --index --whitespace=nowarn "$patch"
        git -C "$SRC" "${COMMITTER[@]}" commit -q --no-verify -m "$(basename "$patch" .patch)"
    done
}

# After the patches: they change which submodules there are.
fetch_externals() {
    git -C "$SRC" submodule sync -q --recursive
    git -C "$SRC" submodule update --init --recursive --depth 1 --jobs 8
    if [[ ! -d $SRC/externals/openxr-sdk ]]; then
        git clone -q --depth 1 --branch "$OPENXR_RELEASE" "$OPENXR_URL" "$SRC/externals/openxr-sdk"
    fi
}

# Sources kept here rather than in a patch, linked into the tree (an edit in either place is the
# same file): shadps4/ at the paths it mirrors, and tools/pkg's library (all but its tool and
# test) as core/file_format/pkg, which the emulator reads games' packages with.
link_sources() {
    local file library=$SRC/src/core/file_format/pkg
    while read -r file; do
        file=${file#./}
        mkdir -p "$(dirname "$SRC/$file")"
        ln -sf "$ROOT/shadps4/$file" "$SRC/$file"
    done < <(cd "$ROOT/shadps4" && find . -type f)
    mkdir -p "$library"
    for file in "$ROOT"/tools/pkg/*.{h,cpp}; do
        case $(basename "$file") in main.cpp | test.cpp) ;; *) ln -sf "$file" "$library/" ;; esac
    done
}

compile() {
    # A build of sources elsewhere (an earlier layout) cannot be reused.
    if [[ -f $OUT/CMakeCache.txt ]] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$SRC" "$OUT/CMakeCache.txt"; then
        rm -rf "$OUT/CMakeCache.txt" "$OUT/CMakeFiles"
    fi
    cmake -S "$SRC" -B "$OUT" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DENABLE_DISCORD_RPC=OFF -DENABLE_UPDATER=OFF
    cmake --build "$OUT" --target shadps4
}

# The pkg tool, on the emulator's own libressl (it runs its tests).
compile_pkg() {
    CXX=clang++ "$ROOT/tools/pkg/build.sh" "$OUT" "$SRC/externals/libressl/include" \
        "$OUT/externals/libressl/crypto/libcrypto.a"
}

run_tests() {
    local test
    for test in flat_camera_test unseen_eye_test; do
        clang++ -std=c++20 -Wall -Wextra -I "$SRC/src" "$SRC/tools/tests/$test.cpp" -o "$OUT/$test"
        "$OUT/$test"
    done
}

fetch
apply_patches
fetch_externals
link_sources
compile
compile_pkg
run_tests
echo "Built $OUT/shadps4"

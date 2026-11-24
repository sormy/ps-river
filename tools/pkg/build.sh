#!/usr/bin/env bash
# Builds the pspkg tool and its unit test (pspkg_test, then run) into OUT, needing nothing but a
# C++20 compiler, libcrypto (OpenSSL or LibreSSL) and zlib.
# Usage: build.sh [OUT [CRYPTO_INCLUDE CRYPTO_LIBRARY]]; OUT defaults to .build/pspkg, libcrypto is
# the system's (pkg-config, or Homebrew's OpenSSL) unless given.
set -euo pipefail

readonly HERE=$(cd "$(dirname "$0")" && pwd)
readonly OUT=${1:-$(cd "$HERE/../.." && pwd)/.build/pspkg}
readonly CXX=${CXX:-c++}

die() {
    echo "error: $*" >&2
    exit 1
}

crypto=()
if (($# >= 3)); then
    crypto=(-I "$2" "$3")
elif pkg-config --exists libcrypto 2>/dev/null; then
    read -ra crypto <<<"$(pkg-config --cflags --libs libcrypto)"
elif command -v brew >/dev/null && [[ -d $(brew --prefix openssl@3 2>/dev/null) ]]; then
    prefix=$(brew --prefix openssl@3)
    crypto=(-I "$prefix/include" -L "$prefix/lib" -lcrypto)
else
    die "no libcrypto: install OpenSSL's development files (libssl-dev, openssl-devel, openssl@3)"
fi

# The tool's or the test's sources: all but the other one's main.
sources() {
    local left_out=$1 source
    for source in "$HERE"/*.cpp; do
        [[ $(basename "$source") == "$left_out" ]] || echo "$source"
    done
}

mkdir -p "$OUT"
build() {
    local name=$1 left_out=$2 files=() file
    while IFS= read -r file; do files+=("$file"); done < <(sources "$left_out")
    "$CXX" -std=c++20 -O2 -Wall -Wextra "${files[@]}" "${crypto[@]}" -lz -lpthread -o "$OUT/$name"
}
build pspkg test.cpp
build pspkg_test main.cpp
"$OUT/pspkg_test"
echo "Built $OUT/pspkg"

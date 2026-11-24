#!/usr/bin/env bash
# Writes the patch commits in .build/src (build.sh makes one per file in patches/shadps4/) back to
# their files, and removes the files no commit is left for. To change a patch: commit the change
# in .build/src as a fixup of its commit, fold it in, then run this:
#   git commit --fixup=<commit>; git -c sequence.editor=: rebase -q -i --autosquash <pinned commit>
set -euo pipefail

readonly ROOT=$(cd "$(dirname "$0")/.." && pwd)
readonly SRC=$ROOT/.build/src
readonly PATCHES=$ROOT/patches/shadps4
# Linked in by build.sh (shadps4/ and tools/pkg's library), not part of any patch.
NOT_PATCHED=(':!src/core/file_format/pkg')
while read -r file; do
    NOT_PATCHED+=(":!${file#./}")
done < <(cd "$ROOT/shadps4" && find . -type f)

upstream=$(sed -n 's/^readonly UPSTREAM_COMMIT=//p' "$ROOT/build.sh")
if [[ -n $(git -C "$SRC" status --porcelain --untracked-files=no -- . "${NOT_PATCHED[@]}") ]]; then
    echo "error: commit the changes in $SRC first" >&2
    exit 1
fi

written=()
while read -r commit; do
    name=$(git -C "$SRC" log -1 --format=%s "$commit")
    git -C "$SRC" diff --binary "$commit^" "$commit" -- . "${NOT_PATCHED[@]}" >"$PATCHES/$name.patch"
    written+=("$name.patch")
done < <(git -C "$SRC" rev-list --reverse "$upstream..HEAD")

for patch in "$PATCHES"/*.patch; do
    [[ " ${written[*]} " == *" $(basename "$patch") "* ]] || rm "$patch"
done
printf 'wrote %s\n' "${written[@]}"

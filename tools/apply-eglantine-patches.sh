#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies the patches in tools/eglantine-patches/ to the Eglantine submodule's working tree, which is
# what the build reads. The Zortos core is referenced directly by the submodule;
# this hook supports optional local patches. Safe to run again: applied patches are left alone.
# --revert takes them out (make update does, before it moves the submodule).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
mode=${1:-apply}
shopt -s nullglob
patches=("$root"/tools/eglantine-patches/*.patch)
((${#patches[@]})) || exit 0
[[ $mode == --revert ]] && mapfile -t patches < <(printf '%s\n' "${patches[@]}" | sort -r)
# A second run (CI runs this from deps and again from the build) cannot tell an applied patch from a broken one
# when a later patch edits the same lines: its reverse check then fails too. The whole set's hash, written after a
# full apply, says the tree already holds exactly these patches.
stamp="$root/Eglantine/.eglantine-patches-applied"
sum=$(cat "${patches[@]}" | sha256sum | cut -c1-16)
if [[ $mode != --revert && -f $stamp && $(cat "$stamp") == "$sum" ]]; then exit 0; fi
[[ $mode == --revert ]] && rm -f "$stamp"
for patch in "${patches[@]}"; do
    name=$(basename "$patch")
    if git -C Eglantine apply --reverse --check "$patch" >/dev/null 2>&1; then
        if [[ $mode == --revert ]]; then
            git -C Eglantine apply --reverse "$patch"
            echo "Eglantine patch taken out: $name"
        fi
    elif [[ $mode == --revert ]]; then
        echo "Eglantine patch not in: $name"
    elif git -C Eglantine apply --check "$patch" >/dev/null 2>&1; then
        git -C Eglantine apply "$patch"
        echo "Eglantine patch applied: $name"
    else
        echo "Eglantine patch does not apply: $name (Eglantine moved; make the patch again)" >&2
        git -C Eglantine apply --check -v "$patch" 2>&1 | grep -iE "error|fail|does not|already" | head -20 >&2 || true
        exit 1
    fi
done
[[ $mode == --revert ]] || echo "$sum" > "$stamp"

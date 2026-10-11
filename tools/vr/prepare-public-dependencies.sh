#!/usr/bin/env bash
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
rt64="$repo/lib/rt64"
plume="$rt64/src/contrib/plume"
rt64_base=43373749dac9bbc1b653e6a02aed40a9e1783bed
plume_base=d890ac899e505fb30040e037a4037cdeca68f033
rt64_patch="$repo/third_party/vr-dependencies/rt64-vr.patch"
plume_patch="$repo/third_party/vr-dependencies/plume-vr.patch"
rt64_patch_sha256=b12913902ff8578b7b288ec306996f633510e2bd4554bd9b2a9a198564325bcb
plume_patch_sha256=99fbdb9731416e35cbc5854bf977cf8fc3679655246462f6e6fed145d18294db

check_hash() {
	local file="$1" expected="$2" actual
	actual="$(shasum -a 256 "$file" | awk '{print $1}')"
	[[ "$actual" == "$expected" ]] || {
		echo "unexpected SHA-256 for $file: $actual" >&2
		exit 1
	}
}

check_hash "$rt64_patch" "$rt64_patch_sha256"
check_hash "$plume_patch" "$plume_patch_sha256"

status_paths() {
	git -C "$1" status --porcelain --untracked-files=all | awk '{print $2}' | LC_ALL=C sort
}

matches_overlay_paths() {
	local dependency="$1" patch="$2" actual expected extra
	actual="$(status_paths "$dependency")"
	expected="$(
		{
			git -C "$repo" apply --numstat "$patch" | awk '{print $3}'
			for extra in "${@:3}"; do printf '%s\n' "$extra"; done
		} | LC_ALL=C sort
	)"
	[[ -n "$actual" && "$actual" == "$expected" ]]
}

overlays_are_applied() {
	[[ "$(git -C "$rt64" rev-parse HEAD 2>/dev/null || true)" == "$rt64_base" ]] &&
	[[ -e "$plume/.git" ]] &&
	[[ "$(git -C "$plume" rev-parse HEAD 2>/dev/null || true)" == "$plume_base" ]] &&
	git -C "$rt64" apply --reverse --check "$rt64_patch" >/dev/null 2>&1 &&
	git -C "$plume" apply --reverse --check "$plume_patch" >/dev/null 2>&1 &&
	matches_overlay_paths "$rt64" "$rt64_patch" src/contrib/plume &&
	matches_overlay_paths "$plume" "$plume_patch"
}

# Inspect initialized renderer worktrees before submodule update can move them.
# A repeated run is accepted only when status exactly matches both pinned patches.
if [[ -e "$rt64/.git" ]]; then
	rt64_status="$(status_paths "$rt64")"
	if [[ -n "$rt64_status" ]]; then
		if overlays_are_applied; then
			echo "public RT64/Plume VR overlays are already applied"
			exit 0
		fi
		non_plume_changes="$(printf '%s\n' "$rt64_status" | awk '$0 != "src/contrib/plume"')"
		if [[ -n "$non_plume_changes" ]]; then
			echo "refusing to update a modified RT64 checkout: $rt64" >&2
			echo "preserve or clean its local changes, then retry" >&2
			exit 1
		fi
	fi
fi
if [[ -e "$plume/.git" ]] && [[ -n "$(status_paths "$plume")" ]]; then
	echo "refusing to update a modified Plume checkout: $plume" >&2
	echo "preserve or clean its local changes, then retry" >&2
	exit 1
fi

git -C "$repo" submodule sync -- lib/rt64
git -C "$repo" submodule update --init --depth=1 -- lib/rt64
git -C "$rt64" submodule sync -- src/contrib/plume
git -C "$rt64" submodule update --init --depth=1 -- src/contrib/plume

[[ "$(git -C "$rt64" rev-parse HEAD)" == "$rt64_base" ]] || {
	echo "lib/rt64 must be at public RT64 base $rt64_base" >&2
	exit 1
}
[[ "$(git -C "$plume" rev-parse HEAD)" == "$plume_base" ]] || {
	echo "RT64 Plume submodule must be at public base $plume_base" >&2
	exit 1
}

# Repeated builds may reuse the already-prepared checkout. Accept it only when
# both complete overlays reverse-check against the pinned sources.
if git -C "$rt64" apply --reverse --check "$rt64_patch" >/dev/null 2>&1 &&
	git -C "$plume" apply --reverse --check "$plume_patch" >/dev/null 2>&1; then
	echo "public RT64/Plume VR overlays are already applied"
	exit 0
fi

for dependency in "$rt64" "$plume"; do
	if [[ -n "$(git -C "$dependency" status --porcelain --untracked-files=all)" ]]; then
		echo "refusing to patch a modified dependency checkout: $dependency" >&2
		echo "preserve or clean its local changes, then retry" >&2
		exit 1
	fi
done

git -C "$rt64" apply --check "$rt64_patch"
git -C "$plume" apply --check "$plume_patch"
git -C "$plume" apply "$plume_patch"
git -C "$rt64" apply "$rt64_patch"
echo "applied public RT64/Plume VR overlays"

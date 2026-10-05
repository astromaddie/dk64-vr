#!/usr/bin/env bash
# Stages the Linux build for the Steam Frame, run inside the build container:
#
#   tools/package_linux.sh <build-dir> <out-dir>
#
# The package holds the DK64Recompiled binary (RUNPATH $ORIGIN/lib), the assets/ directory and
# controller database it loads from beside itself, the shared libraries it needs that the Frame's
# Steam Linux Runtime does not guarantee (see check_linux_abi.py), the launcher, a
# BUILD-METADATA.json, and nothing else. It never contains the ROM, extracted assets or saves:
# those live in $XDG_DATA_HOME/DK64Recompiled/.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:?build dir}"
out="${2:?output dir}"

rm -rf "$out"
mkdir -p "$out/lib"
cp "$build/DK64Recompiled" "$out/DK64Recompiled"
cp -R "$root/assets" "$out/assets"
cp "$root/recompcontrollerdb.txt" "$root/LICENSE" "$out/"
cp "$root/tools/steam_frame/LAUNCH-STEAM-FRAME.sh" "$out/LAUNCH-STEAM-FRAME.sh"
chmod +x "$out/DK64Recompiled" "$out/LAUNCH-STEAM-FRAME.sh"
# Debug info and the symbol table stay in the build directory
strip --strip-unneeded "$out/DK64Recompiled"

# The libraries the binary needs that are not system libraries
for lib in $(python3 "$root/tools/check_linux_abi.py" --list-unbundled "$out/DK64Recompiled"); do
	path="$(ldd "$out/DK64Recompiled" | awk -v lib="$lib" '$1 == lib { print $3 }')"
	[ -n "$path" ] || { echo "cannot find $lib" >&2; exit 1; }
	cp -L "$path" "$out/lib/$lib"
done

# Package allowlist: no ROMs, game data or saves
if find "$out" \( -iname '*.z64' -o -iname '*.n64' -o -iname '*.v64' -o -iname '*.sav' -o -iname 'saves' \) | grep -q .; then
	echo "package contains a ROM or a save" >&2
	exit 1
fi

rev="$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)"
dirty="$(git -C "$root" status --porcelain 2>/dev/null | grep -qv '^??' && echo dirty || echo clean)"
{
	echo "{"
	echo "  \"title\": \"DK64Recompiled\","
	echo "  \"source_revision\": \"$rev\","
	echo "  \"tree\": \"$dirty\","
	echo "  \"compiler\": \"$(${CC:-cc} --version | head -1)\","
	echo "  \"files\": {"
	(cd "$out" && find . -type f ! -name BUILD-METADATA.json | sort | while read -r f; do
		printf '    "%s": "%s",\n' "${f#./}" "$(sha256sum "$f" | cut -d' ' -f1)"
	done) | sed '$ s/,$//'
	echo "  }"
	echo "}"
} > "$out/BUILD-METADATA.json"
ls -l "$out" "$out/lib"
du -sh "$out"

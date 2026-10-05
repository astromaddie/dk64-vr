#!/usr/bin/env bash
# Copies the build to a Steam Frame and registers it as a devkit title, which then shows in the
# Frame's Steam library.
#
#   tools/steam_frame/deploy.sh [--package DIR] [--rom FILE] [--launch-args "A B"]
#                               [--no-register] [--logs] [--dry-run]
#
#   --package DIR    the staged package (default build-linux-aarch64/package/DK64Recompiled, made
#                    by `tools/docker/build-linux.sh package`)
#   --rom FILE       also copy the user's own US ROM (sha1 checked first) to
#                    ~/.local/share/DK64Recompiled/DK64.z64 on the Frame, where the game keeps its
#                    ROM. Never put it in git.
#   --launch-args A  extra arguments for the registered title
#   --no-register    only copy files
#   --logs           print the last run's log (run-stdout.txt) and exit
#   --dry-run        print what would run, change nothing
#
# FRAME_HOST (default steamos@frame.local; the USB link is steamos@10.86.200.233) and
# TITLE (default DK64VR, alphanumeric) come from the environment. The title is a native Linux
# ARM64 title: compat tool SteamLinuxRuntime_4-arm64, steam_play=0. Start it from the Steam
# library; a binary started over SSH sees no controllers.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
frame_host="${FRAME_HOST:-steamos@frame.local}"
title="${TITLE:-DK64VR}"
remote_dir="devkit-game/$title"
package="$root/build-linux-aarch64/package/DK64Recompiled"
want_sha1="cf806ff2603640a748fca5026ded28802f1f4a50"
rom=""
register=1
logs=0
dry=0
launch_args=""

[[ "$title" =~ ^[A-Za-z0-9]+$ ]] || { echo "TITLE must be alphanumeric" >&2; exit 2; }

while (($#)); do
	case "$1" in
		--package) package="$2"; shift 2 ;;
		--rom) rom="$2"; shift 2 ;;
		--launch-args) launch_args="$2"; shift 2 ;;
		--no-register) register=0; shift ;;
		--logs) logs=1; shift ;;
		--dry-run) dry=1; shift ;;
		*) echo "unknown option $1" >&2; exit 2 ;;
	esac
done

run() {
	if ((dry)); then
		printf '+'; printf ' %q' "$@"; printf '\n'
	else
		"$@"
	fi
}

ssh_opts=(-o ConnectTimeout=5 -o BatchMode=yes)

if ((logs)); then
	exec ssh "${ssh_opts[@]}" "$frame_host" "cat ~/$remote_dir/run-stdout.txt"
fi

[[ -x "$package/DK64Recompiled" && -x "$package/LAUNCH-STEAM-FRAME.sh" ]] || {
	echo "$package is not a package (run tools/docker/build-linux.sh package)" >&2
	exit 1
}
if [[ -n "$rom" ]]; then
	[[ -f "$rom" ]] || { echo "$rom is not a file" >&2; exit 1; }
	have="$(shasum -a 1 "$rom" | cut -d' ' -f1)"
	[[ "$want_sha1" == "$have" ]] || { echo "$rom is not the US ROM (sha1 $have, want $want_sha1)" >&2; exit 1; }
fi

if ! ((dry)); then
	ssh "${ssh_opts[@]}" "$frame_host" true 2>/dev/null || {
		echo "The Frame is not reachable at $frame_host (USB cable, charge, ssh key)." >&2
		exit 1
	}
fi

# macOS rsync is 2.6: no --info. -a keeps the executable bits.
run ssh "${ssh_opts[@]}" "$frame_host" "mkdir -p ~/$remote_dir"
run rsync -a --delete --partial --exclude 'run-*.txt' "$package/" "$frame_host:$remote_dir/"

if [[ -n "$rom" ]]; then
	run ssh "${ssh_opts[@]}" "$frame_host" 'mkdir -p ~/.local/share/DK64Recompiled'
	run rsync -a --partial "$rom" "$frame_host:.local/share/DK64Recompiled/DK64.z64"
fi

if ((register)); then
	# The devkit service's own shortcut tool, as the other native Frame ports use it. The title
	# name must be alphanumeric.
	argv='"LAUNCH-STEAM-FRAME.sh"'
	for a in $launch_args; do
		[[ "$a" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "bad launch argument $a" >&2; exit 2; }
		argv+=', "'$a'"'
	done
	parms='{"gameid": "'$title'", "directory": "/home/steamos/'$remote_dir'", "force_appid": "", "argv": ['$argv'], "env": {}, "settings": {"steam_play": "0", "compat_tool": "SteamLinuxRuntime_4-arm64"}, "lepton_args": ""}'
	run ssh "${ssh_opts[@]}" "$frame_host" "cd ~/devkit-utils && python3 steam-client-create-shortcut --parms '$parms'"
	echo "Registered $title. Start it from the Frame's Steam library; read the log with: $0 --logs"
fi

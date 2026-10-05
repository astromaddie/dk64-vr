#!/usr/bin/env bash
# The devkit title DK64Recompiled runs this (deploy.sh registers it). Steam starts it, which is
# also what gives the game its controllers; a binary started from SSH sees none. Logs go to
# run-stdout.txt beside this script.
#
# The ROM is not part of the package. The game keeps its own copy of the ROM, its saves
# (saves/), mods (mods/), settings and controller bindings in one folder, and this script points
# that folder at $XDG_DATA_HOME/DK64Recompiled (default ~/.local/share/DK64Recompiled) through
# APP_FOLDER_PATH. Without the variable the game would use ~/.config/DK64Recompiled.
#
# Put the user's own US ROM in that folder as DK64.z64 (any other single *.z64 there is linked
# to that name); it is checked against the game's hash. If it is missing the game opens its ROM
# screen, which needs a desktop file portal the Frame does not have.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# The game finds assets/ and recompcontrollerdb.txt relative to its working directory.
cd "$here"

# The Frame runs gamescope with Xwayland on :0.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
export DISPLAY="${DISPLAY:-:0}"

data="${XDG_DATA_HOME:-$HOME/.local/share}/DK64Recompiled"
export APP_FOLDER_PATH="${APP_FOLDER_PATH:-$data}"
mkdir -p "$APP_FOLDER_PATH"

if [[ ! -e "$APP_FOLDER_PATH/DK64.z64" ]]; then
	shopt -s nullglob
	roms=("$APP_FOLDER_PATH"/*.z64)
	shopt -u nullglob
	((${#roms[@]} == 1)) && ln -s "${roms[0]}" "$APP_FOLDER_PATH/DK64.z64"
fi

# One-shot diagnostics for an unattended run (standard.md section 8): a Steam launch has fixed
# arguments, so they come from $APP_FOLDER_PATH/vr-diag, which is read and deleted here, so a
# normal launch never inherits them. Lines are DK64_VR_<KEY>=<value> (exported) or ARGS=<extra
# game arguments>, e.g. ARGS=--game dk64 to skip the launcher.
diag="$APP_FOLDER_PATH/vr-diag"
diag_lines=()
extra_args=()
if [[ -f "$diag" ]]; then
	while IFS= read -r line || [[ -n "$line" ]]; do
		case "$line" in
			DK64_VR_*=*) export "${line?}"; diag_lines+=("$line") ;;
			ARGS=*) read -r -a extra_args <<< "${line#ARGS=}"; diag_lines+=("$line") ;;
		esac
	done < "$diag"
	rm -f "$diag"
fi

log="${DK64_LOG:-run-stdout.txt}"
{
	echo "[launch] $(date -Is) $here/DK64Recompiled $*"
	echo "[launch] data dir $APP_FOLDER_PATH"
	[[ -e "$APP_FOLDER_PATH/DK64.z64" ]] || echo "[launch] no DK64.z64 in $APP_FOLDER_PATH: the game will stop at its ROM screen"
	if ((${#diag_lines[@]})); then
		echo "[vr] diag: ${diag_lines[*]}"
	else
		echo "[vr] diag: no vr-diag file"
	fi
} > "$log"

exec ./DK64Recompiled "$@" ${extra_args[@]+"${extra_args[@]}"} >> "$log" 2>&1

#!/usr/bin/env bash
# One unattended run of the DK64VR Steam title on the Frame (standard.md section 8, lessons 37,
# 46, 60, 81): arms a one-shot vr-diag file, launches the title through Steam's pipe (so it gets
# controllers and the per-app refresh rate), waits, then copies the log and the dumps back.
#
#   tools/steam_frame/unattended.sh <seconds> <out-dir> [KEY=VALUE ...]
#
# KEY=VALUE become DK64_VR_<KEY>=<VALUE> lines (e.g. force_render=1 dump_frame=600); ARGS=... is
# passed through (default ARGS=--game dk64, which skips the launcher). Book the Frame first
# (frame-lock.sh claim). The run stops on its own only with diag_exit_seconds; otherwise the
# script stops it after <seconds>.
set -euo pipefail
host="${FRAME_HOST:-steamos@192.168.1.31}"
title="${TITLE:-DK64VR}"
seconds="$1"; out="$2"; shift 2
ssh_opts=(-o ConnectTimeout=5 -o BatchMode=yes)
data=".local/share/DK64Recompiled"

# Someone else's app, or a wearer: stop (lessons 37, 46).
busy="$(ssh "${ssh_opts[@]}" "$host" 'ps -eo args | grep -E "lepton-steamlaunch|lepton-dev|devkit-game/" | grep -v -E "grep|DK64" || true')"
if [[ -n "$busy" ]]; then echo "another app is running:"; echo "$busy"; exit 3; fi
if ssh "${ssh_opts[@]}" "$host" 'journalctl --user --since "-2 min" --no-pager 2>/dev/null | grep -q "session focused"'; then
	echo "a session was FOCUSED in the last 2 minutes: someone may be wearing the headset"; exit 3
fi

diag=""
have_args=0
for kv in "$@"; do
	case "$kv" in
		ARGS=*) diag+="$kv"$'\n'; have_args=1 ;;
		RT64_VR_*=*) diag+="$kv"$'\n' ;;
		*=*) k="${kv%%=*}"; diag+="DK64_VR_$(printf '%s' "$k" | tr '[:lower:]' '[:upper:]')=${kv#*=}"$'\n' ;;
	esac
done
((have_args)) || diag+="ARGS=--game dk64"$'\n'
printf '%s' "$diag" | ssh "${ssh_opts[@]}" "$host" "rm -rf ~/$data/vr-dumps; cat > ~/$data/vr-diag"

# The shortcut's appid from shortcuts.vdf (binary VDF), then steam://rungameid/<appid<<32|0x02000000>.
gameid="$(ssh "${ssh_opts[@]}" "$host" python3 - "$title" <<'PY'
import glob, re, struct, sys
title = sys.argv[1].encode()
for path in glob.glob('/home/steamos/.local/share/Steam/userdata/*/config/shortcuts.vdf'):
    data = open(path, 'rb').read()
    for m in re.finditer(rb'\x02appid\x00(.{4})', data, re.S):
        appid = struct.unpack('<I', m.group(1))[0]
        name = re.search(rb'\x01(?:AppName|appname)\x00([^\x00]*)\x00', data[m.end():m.end() + 400])
        if name and (name.group(1) == title or name.group(1) == b'Devkit Game: ' + title):
            print((appid << 32) | 0x02000000)
            sys.exit(0)
sys.exit(1)
PY
)"
echo "launching $title (gameid $gameid) for ${seconds}s"
ssh "${ssh_opts[@]}" "$host" "printf 'steam://rungameid/%s\n' $gameid > ~/.steam/steam.pipe"
sleep "$seconds"

# Stop it if it is still running (its own container), then fetch.
ssh "${ssh_opts[@]}" "$host" 'pid=$(pgrep -x DK64Recompiled | head -1); if [ -n "$pid" ]; then kill -TERM $pid; sleep 4; pgrep -x DK64Recompiled >/dev/null && echo "still running after TERM" || echo "stopped"; else echo "already exited"; fi'
mkdir -p "$out"
scp -q "${ssh_opts[@]}" "$host:devkit-game/$title/run-stdout.txt" "$out/run-stdout.txt" || true
scp -q -r "${ssh_opts[@]}" "$host:$data/vr-dumps" "$out/" 2>/dev/null || true
grep -E "^\[vr\] diag" "$out/run-stdout.txt" || echo "WARNING: no diag echo in the log: the build under test may not have run (lesson 81)"
ls "$out" "$out/vr-dumps" 2>/dev/null | head -50

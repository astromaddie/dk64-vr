#!/usr/bin/env bash
# Host-only launcher test: no Steam, headset, ROM or game launch.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
scratch_parent="${1:-$repo/build-vr-diag-test}"
mkdir -p "$scratch_parent"
scratch_parent="$(cd "$scratch_parent" && pwd)"
scratch="$(mktemp -d "$scratch_parent/vr-diag.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/data"
ln -s /dev/null "$scratch/data/DK64.z64" # only bypass the launcher's ROM lookup; no game data
cp "$repo/tools/steam_frame/LAUNCH-STEAM-FRAME.sh" "$scratch/launch.sh"
cat > "$scratch/DK64Recompiled" <<'SH'
#!/usr/bin/env bash
printf '[fixture] fov=%s fdm=%s layer=%s hud=%s args=%s\n' "${DK64_VR_FOVEATION-unset}" "${FDM_DEBUG-unset}" "${DISABLE_VULKAN_FDM_INJECTION_LAYER-unset}" "${RT64_VR_LOG_HUD-unset}" "$*"
printf '[camera] scale=%s lower=%s closer=%s\n' "${DK64_VR_WORLD_SCALE-unset}" "${DK64_VR_CAMERA_LOWER-unset}" "${DK64_VR_CAMERA_CLOSER-unset}"
printf '[drivers] tu=%s fd=%s mesa=%s vk=%s\n' "${TU_DEBUG-unset}" "${FD_DEBUG-unset}" "${MESA_DEBUG-unset}" "${VK_LOADER_DEBUG-unset}"
SH
chmod +x "$scratch/DK64Recompiled"
cat > "$scratch/data/vr-diag" <<'DIAG'
DK64_VR_FOVEATION=high
DK64_VR_WORLD_SCALE=8
DK64_VR_CAMERA_LOWER=0.5
DK64_VR_CAMERA_CLOSER=0.3
RT64_VR_LOG_HUD=1
FDM_DEBUG=1
DISABLE_VULKAN_FDM_INJECTION_LAYER=0
TU_DEBUG=perf
FD_DEBUG=msgs
MESA_DEBUG=context
VK_LOADER_DEBUG=error
ARGS=--game dk64
DIAG
run_fixture() {
    env -u DK64_VR_WORLD_SCALE -u DK64_VR_CAMERA_LOWER -u DK64_VR_CAMERA_CLOSER -u DK64_VR_FOVEATION -u RT64_VR_LOG_HUD -u FDM_DEBUG -u DISABLE_VULKAN_FDM_INJECTION_LAYER \
        -u TU_DEBUG -u FD_DEBUG -u MESA_DEBUG -u VK_LOADER_DEBUG \
        APP_FOLDER_PATH="$scratch/data" bash "$scratch/launch.sh"
}
run_fixture
test ! -e "$scratch/data/vr-diag"
test "$(tail -n 3 "$scratch/run-stdout.txt" | head -n 1)" = '[fixture] fov=high fdm=1 layer=0 hud=1 args=--game dk64'
test "$(tail -n 2 "$scratch/run-stdout.txt" | head -n 1)" = '[camera] scale=8 lower=0.5 closer=0.3'
test "$(tail -n 1 "$scratch/run-stdout.txt")" = '[drivers] tu=perf fd=msgs mesa=context vk=error'
run_fixture
test "$(tail -n 4 "$scratch/run-stdout.txt" | head -n 1)" = '[vr] diag: no vr-diag file'
test "$(tail -n 3 "$scratch/run-stdout.txt" | head -n 1)" = '[fixture] fov=unset fdm=unset layer=unset hud=unset args='
test "$(tail -n 2 "$scratch/run-stdout.txt" | head -n 1)" = '[camera] scale=unset lower=unset closer=unset'
test "$(tail -n 1 "$scratch/run-stdout.txt")" = '[drivers] tu=unset fd=unset mesa=unset vk=unset'
echo 'vr-diag launcher: camera, foveation and driver overrides passed and consumed once; PASS'

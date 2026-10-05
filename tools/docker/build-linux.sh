#!/usr/bin/env bash
# Builds DK64 Recompiled for Linux inside the pinned environment from linux.Dockerfile. The
# target architecture is the Docker host's: arm64 on Apple Silicon, which is the Steam Frame's.
# Build files go to build-linux-<arch>/ in the repository (gitignored), and so do the generated
# recompiler outputs (RecompiledFuncs/, RecompiledPatches/, rsp/n_aspMain.cpp) and the
# decompressed ROM in the repository root. None of them is ever committed.
#
#   DK64_ROM="/path/to/Donkey Kong 64 (USA).z64" tools/docker/build-linux.sh [step]
#
# Steps (default: all):
#   image    build the Docker image (tools/docker/linux.Dockerfile)
#   tools    build N64Recomp and RSPRecomp from lib/N64ModernRuntime/N64Recomp
#   rom      decompress the ROM (needs DK64_ROM; its sha1 is checked first)
#   recomp   run N64Recomp us.toml and RSPRecomp n_aspMain.toml
#   build    CMake + Ninja: the DK64Recompiled binary
#   package  stage build-linux-<arch>/package/DK64Recompiled (binary, assets, lib/, launcher)
#   abi      the ABI check on the staged package (fails above glibc 2.35 / GLIBCXX 3.4.30)
#   smoke    start the package under Xvfb + lavapipe for a few seconds (DK64_ROM optional)
#   all      image, tools, rom, recomp, build, package, abi
#
# JOBS sets the build parallelism (default: every CPU of the container).
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
image="${DK64_VR_IMAGE:-dk64-vr-linux}"
step="${1:-all}"
want_sha1="cf806ff2603640a748fca5026ded28802f1f4a50"

# Passed through only when set, so the container script's own defaults apply otherwise.
env_args=()
for var in BUILD_DIR JOBS BUILD_TYPE SMOKE_SECONDS; do
	[[ -z "${!var:-}" ]] || env_args+=(-e "$var=${!var}")
done

in_container() {
	local extra=("$@")
	docker run --rm -v "$repo":/src -w /src ${env_args[@]+"${env_args[@]}"} \
		${extra[@]+"${extra[@]}"} "$image" tools/docker/container-steps.sh "$STEP"
}

run_step() {
	STEP="$1"
	shift
	in_container "$@"
}

check_rom() {
	[[ -n "${DK64_ROM:-}" && -f "$DK64_ROM" ]] || { echo "set DK64_ROM to your US .z64" >&2; exit 1; }
	have="$(shasum -a 1 "$DK64_ROM" | cut -d' ' -f1)"
	[[ "$have" == "$want_sha1" ]] || { echo "$DK64_ROM is not the US ROM (sha1 $have, want $want_sha1)" >&2; exit 1; }
}

case "$step" in
	image) docker build -t "$image" -f "$repo/tools/docker/linux.Dockerfile" "$repo/tools/docker" ;;
	tools | recomp | build | package | abi) run_step "$step" ;;
	rom)
		check_rom
		# Read-only mount: the ROM is never copied anywhere on the host.
		run_step rom -v "$DK64_ROM":/rom/baserom.us.z64:ro -e ROM_FILE=/rom/baserom.us.z64
		;;
	smoke)
		if [[ -n "${DK64_ROM:-}" ]]; then
			check_rom
			run_step smoke -v "$DK64_ROM":/rom/baserom.us.z64:ro -e ROM_FILE=/rom/baserom.us.z64
		else
			run_step smoke
		fi
		;;
	all)
		check_rom
		"$0" image
		"$0" tools
		"$0" rom
		"$0" recomp
		"$0" build
		"$0" package
		"$0" abi
		;;
	*) echo "unknown step $step" >&2; exit 2 ;;
esac

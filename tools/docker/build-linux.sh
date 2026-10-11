#!/usr/bin/env bash
# Builds DK64 Recompiled for Linux inside the pinned environment from linux.Dockerfile. The
# target architecture is the Docker image's. Set DOCKER_PLATFORM=linux/arm64 to build for the
# Steam Frame from an x86-64 Docker host with ARM64 emulation enabled.
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
#   vrtest   build and run the VR bridge's host unit tests
#   all      image, tools, rom, recomp, build, package, abi
#
# JOBS sets the build parallelism (default: every CPU of the container). DK64_VR=1 makes the
# OpenXR VR build, in build-linux-<arch>-vr/.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
image="${DK64_VR_IMAGE:-dk64-vr-linux}"
docker_platform="${DOCKER_PLATFORM:-}"
step="${1:-all}"
want_sha1="cf806ff2603640a748fca5026ded28802f1f4a50"

# Passed through only when set, so the container script's own defaults apply otherwise.
env_args=()
for var in BUILD_DIR JOBS BUILD_TYPE SMOKE_SECONDS DK64_VR; do
	[[ -z "${!var:-}" ]] || env_args+=(-e "$var=${!var}")
done

in_container() {
	local extra=("$@")
	local platform_args=()
	[[ -z "$docker_platform" ]] || platform_args+=(--platform "$docker_platform")
	docker run ${platform_args[@]+"${platform_args[@]}"} --rm -v "$repo":/src -w /src ${env_args[@]+"${env_args[@]}"} \
		${extra[@]+"${extra[@]}"} "$image" tools/docker/container-steps.sh "$STEP"
}

build_image() {
	if [[ -n "$docker_platform" ]]; then
		docker buildx build --platform "$docker_platform" --load -t "$image" \
			-f "$repo/tools/docker/linux.Dockerfile" "$repo/tools/docker"
	else
		docker build -t "$image" -f "$repo/tools/docker/linux.Dockerfile" "$repo/tools/docker"
	fi
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

prepare_vr_dependencies() {
	bash "$repo/tools/vr/prepare-public-dependencies.sh"
}

case "$step" in
	image) build_image ;;
	tools | recomp | package | abi) run_step "$step" ;;
	build)
		if [[ "${DK64_VR:-0}" == 1 ]]; then prepare_vr_dependencies; fi
		run_step "$step"
		;;
	vrtest)
		prepare_vr_dependencies
		run_step "$step"
		;;
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
		if [[ "${DK64_VR:-0}" == 1 ]]; then prepare_vr_dependencies; fi
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

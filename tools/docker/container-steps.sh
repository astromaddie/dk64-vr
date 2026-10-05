#!/usr/bin/env bash
# The build steps themselves, run inside the container from tools/docker/linux.Dockerfile with
# the repository mounted at /src. tools/docker/build-linux.sh is the host-side entry point.
#
#   container-steps.sh tools|rom|recomp|build|package|abi|smoke
#
# Environment: BUILD_DIR (default build-linux-<arch>), JOBS, BUILD_TYPE (default Release),
# ROM_FILE (rom step: the original US .z64, mounted read-only by build-linux.sh).
set -euo pipefail

cd /src
arch="$(uname -m)"
build="${BUILD_DIR:-build-linux-$arch}"
jobs="${JOBS:-$(nproc)}"
type="${BUILD_TYPE:-Release}"
out="$build/package/DK64Recompiled"

# Only on aarch64: char is unsigned by default there, the MIPS-derived code expects signed;
# and clang fuses a*b+c into FMA by default on aarch64, which would change the recompiled
# floating point results. x86-64 already behaves this way, so the flags change nothing there.
flags="-fsigned-char -ffp-contract=off"

step_tools() {
	# N64Recomp and RSPRecomp from the revision the runtime submodule is pinned to, so the
	# generated code and the runtime that loads it agree. Out-of-tree; the two binaries are
	# copied to the repository root, where us.toml and the CMake patch step expect them.
	cmake -S lib/N64ModernRuntime/N64Recomp -B "$build/n64recomp" -GNinja \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-15 -DCMAKE_CXX_COMPILER=clang++-15
	cmake --build "$build/n64recomp" --target N64RecompCLI RSPRecomp -j "$jobs"
	cp "$build/n64recomp/N64Recomp" "$build/n64recomp/RSPRecomp" .
}

step_rom() {
	# Decompresses the overlays with the decomp's own script, which wants fixed file names in
	# its working directory. Done in a throwaway directory, so the only result is the
	# gitignored donkeykong64.decompressed.us.z64 in the repository root.
	: "${ROM_FILE:?ROM_FILE}"
	work="$(mktemp -d)"
	trap 'rm -rf "$work"' EXIT
	cp "$ROM_FILE" "$work/baserom.us.z64"
	(cd "$work" && python3 /src/lib/dk64_decomp/tools/generate_decompressed_rom.py > /dev/null)
	cp "$work/baserom.us.decompressed.z64" donkeykong64.decompressed.us.z64
	ls -l donkeykong64.decompressed.us.z64
}

step_recomp() {
	test -x ./N64Recomp -a -x ./RSPRecomp || { echo "run the tools step first" >&2; exit 1; }
	test -f donkeykong64.decompressed.us.z64 || { echo "run the rom step first" >&2; exit 1; }
	./N64Recomp us.toml
	./RSPRecomp n_aspMain.toml
}

step_build() {
	test -d RecompiledFuncs || { echo "run the recomp step first" >&2; exit 1; }
	# VCPKG_MANIFEST_INSTALL=OFF: the top-level CMakeLists includes vcpkg's toolchain only for
	# curl, which the image provides (static, see the Dockerfile) and OPENSSL_USE_STATIC_LIBS
	# keeps libssl/libcrypto out of the package too. NFD_PORTAL=ON: the file dialog talks to
	# xdg-desktop-portal over D-Bus instead of linking GTK3.
	cmake -S . -B "$build" -GNinja -DCMAKE_BUILD_TYPE="$type" \
		-DCMAKE_C_COMPILER=clang-15 -DCMAKE_CXX_COMPILER=clang++-15 \
		-DPATCHES_C_COMPILER=clang-15 -DPATCHES_LD=ld.lld-15 \
		-DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
		-DVCPKG_MANIFEST_INSTALL=OFF -DNFD_PORTAL=ON -DOPENSSL_USE_STATIC_LIBS=ON \
		-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON '-DCMAKE_INSTALL_RPATH=$ORIGIN/lib'
	cmake --build "$build" --target DK64Recompiled -j "$jobs"
	file "$build/DK64Recompiled"
	ls -l "$build/DK64Recompiled"
}

step_package() {
	tools/package_linux.sh "$build" "$out"
}

step_abi() {
	python3 tools/check_linux_abi.py --lib-dir "$out/lib" "$out/DK64Recompiled"
}

step_smoke() {
	# Starts the packaged build under a virtual X server with software Vulkan (lavapipe), with
	# a throwaway data directory. Proves the binary loads, finds its assets, and gets as far as
	# its UI; it says nothing about the Frame's GPU. With ROM_FILE set, the ROM is stored where
	# the game keeps it, so the run also gets past the ROM screen.
	data="$(mktemp -d)"
	trap 'rm -rf "$data"' EXIT
	[ -z "${ROM_FILE:-}" ] || cp "$ROM_FILE" "$data/DK64.z64"
	cd "$out"
	set +e
	APP_FOLDER_PATH="$data" SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 \
		timeout "${SMOKE_SECONDS:-25}" xvfb-run -a -s '-screen 0 1280x720x24' ./DK64Recompiled 2>&1 | tail -40
	status=${PIPESTATUS[0]}
	echo "exit status $status (124 = still running when the timeout stopped it)"
	ls -l "$data"
}

step="${1:?step}"
"step_$step"

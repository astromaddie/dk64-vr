# Steam Frame build and install

This guide builds the standalone Steam Frame target: a Linux ARM64 package. The build host can be Linux, macOS, or Windows running a Linux shell under WSL2; it does not need to be ARM64. A Windows or desktop VR executable is a different target and is not the Frame package.

## Build host

The build host needs Bash, Git, Python 3, Perl's `shasum` command, and Docker with Buildx. The wrapper checks the ROM and the public VR dependency preparation script checks patch hashes with `shasum`. On Windows, use Docker Desktop with WSL2 integration, clone and build inside the Linux distribution's filesystem, and run these commands from Bash. See Microsoft's [install WSL](https://learn.microsoft.com/windows/wsl/install) instructions and Docker's [WSL2 integration](https://docs.docker.com/desktop/features/wsl/use-wsl/) and [multi-platform builds with emulation](https://docs.docker.com/build/building/multi-platform/) guides. `docker buildx inspect --bootstrap` should list `linux/arm64` under supported platforms. ARM64 emulation can make this compiler and game-code build slow.

On an ARM64 Linux Docker host, the default Docker platform is already the target. On x86-64 Linux, macOS, or WSL2, set `DOCKER_PLATFORM=linux/arm64`; the wrapper builds and loads the target image with Buildx, then runs the build inside that ARM64 image.

## Build on Frame Desktop

The script also supports building natively on the Frame's ARM64 Linux host if Docker and an ARM64-capable Buildx builder are already available in your writable home-directory environment. From that checkout, leave `DOCKER_PLATFORM` unset and run:

```sh
export DK64_ROM="$HOME/game-data/owned-us-rom.z64"
DK64_VR=1 JOBS=4 tools/docker/build-linux.sh all
```

This repository does not set up Docker or add packages to SteamOS; if Docker is unavailable, build on an external Linux, macOS, or WSL2 host instead.

## Build the Frame package

Clone the Frame branch with its submodules and set `DK64_ROM` to your own supported US 1.0 `.z64` image. The script checks its SHA-1 before generating game code.

```sh
git clone --recurse-submodules --branch steam-frame https://github.com/astromaddie/dk64-vr.git
cd dk64-vr
export DK64_ROM="$HOME/game-data/owned-us-rom.z64"
DOCKER_PLATFORM=linux/arm64 DK64_VR=1 JOBS=4 tools/docker/build-linux.sh all
```

On an ARM64 Linux Docker host, omit the `DOCKER_PLATFORM` assignment. The package is written to `build-linux-aarch64-vr/package/DK64Recompiled/`; it contains the ARM64 game, assets, runtime libraries and `LAUNCH-STEAM-FRAME.sh`, but not your ROM. Do not use `BUILDING.md`'s generic desktop build output as a Frame package.

## First install and game data

For a first install from another computer, follow Valve's [Steam Frame development setup](https://partner.steamgames.com/doc/steamhardware/steamframe/setup) to enable Developer Mode and pair the host. In SteamOS Devkit Client, connect to the Frame by IP, then upload the package directory above. Set **Name** to `DK64VR`, **Local Folder** to the package directory, **Start Command** to `LAUNCH-STEAM-FRAME.sh`, and **Runtime** to **Steam Linux Runtime 3.0 ARM64 (Sniper)**. The [Valve upload guide](https://partner.steamgames.com/doc/steamhardware/steamframe/loadgames) describes those fields. Uploading creates the Steam library entry; launch it there so Steam supplies SteamVR and controller input.

Before the first launch, transfer your own US ROM to the Frame. Its expected SHA-1 is `cf806ff2603640a748fca5026ded28802f1f4a50`. In a Frame Desktop terminal, after placing the file in `~/game-data/`, check it and copy it to the exact runtime filename:

```sh
sha1sum "$HOME/game-data/owned-us-rom.z64"
mkdir -p "$HOME/.local/share/DK64Recompiled"
cp "$HOME/game-data/owned-us-rom.z64" "$HOME/.local/share/DK64Recompiled/DK64.z64"
```

The launcher uses that writable app-data directory; saves, settings and mods also stay there.

If the package is already on the Frame, keep the whole package in a writable folder under your home directory, add `LAUNCH-STEAM-FRAME.sh` through Steam's **Add a Non-Steam Game** flow, and start it from the Steam library. Copy the ROM into the app-data path first. Start it from Steam so Steam supplies SteamVR and controller input.

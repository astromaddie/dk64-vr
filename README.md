# Donkey Kong 64 VR for Steam Frame

This is my Steam Frame fork of [Donkey Kong 64: Recompiled](https://github.com/Rainchus/Donkey-Kong-64-Recompiled). It adds a native OpenXR stereo mode for Steam Frame, running as a Linux ARM64 app through SteamVR, Vulkan, RT64 and Plume. The original flat mode is still available.

**EXPERIMENTAL.** This is a personal-use project and is still a work in progress. You need the supported US 1.0 DK64 ROM from your own copy of the game. The repository contains source and build tools, not the ROM or generated game data.

**Want to play on Steam Frame?** Download the prebuilt standalone package from the [v0.1.0 prerelease](https://github.com/astromaddie/dk64-vr/releases/tag/v0.1.0); no compiler is needed. You still need the supported US 1.0 DK64 ROM from your own copy of the game.

## What the Frame fork adds

- Tracked stereo gameplay, with game menus, pause screens and HUD presented on readable panels.
- A 3D cinema for cutscenes, with options for an immersive or flat presentation.
- Snap or smooth turning, adjustable world size and camera placement, camera smoothing and foveated rendering.
- A VR settings menu, laser pointer, recentering, pause handling and Frame controller bindings. Hold Menu + View to open VR settings; hold View to recenter. The right stick turns in third person by default.
- A native Linux ARM64 build and Steam Frame installation path. Without an active OpenXR runtime, the game runs in flat mode.

See [VR architecture](docs/VR_ARCHITECTURE.md) for the renderer and game integration, controls, settings and detailed implementation notes.

## Build and install

The [Steam Frame build and install guide](docs/STEAM-FRAME-BUILD-INSTALL.md) covers Docker ARM64 builds from Linux, macOS, or Windows through WSL2, the supported ROM, package output, first installation and Steam launch. It distinguishes the Linux ARM64 Frame package from Windows or desktop builds. The upstream [BUILDING.md](BUILDING.md) is a desktop recompiler guide, not the Steam Frame packaging route.

## ROM-free VR checks

The [VR validation workflow](.github/workflows/public-vr-validation.yml) runs without a ROM or repository secrets. You can run the same production-source checks locally after cloning the dependencies above, with CMake, Ninja and a C++ compiler:

```sh
bash tools/vr/prepare-public-dependencies.sh
cmake -S src/vr/tests -B build-vrtest -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-vrtest
ctest --test-dir build-vrtest --output-on-failure
```

## Upstream and licenses

The fork retains Donkey Kong 64: Recompiled's upstream notices and license. The earlier [PR0 readiness audit](docs/PR0_READINESS.md), [source and CI metadata](docs/PR0_METADATA.json), and [README history](docs/README_PRE_PR0.md) remain available as historical records. The host application retains its upstream [GPLv3 notice](COPYING); RT64 and Plume retain their own MIT notices. Code licenses do not grant permission to redistribute the commercial game, ROMs or extracted media. Bring your own game data.

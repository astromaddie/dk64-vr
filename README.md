# Donkey Kong 64: Recompiled — Steam Frame VR fork

**Status: EXPERIMENTAL. No Steam Frame build or release has been published.** Upstream is [Rainchus/Donkey-Kong-64-Recompiled](https://github.com/Rainchus/Donkey-Kong-64-Recompiled). This fork adds an optional native OpenXR stereo path targeting standalone Steam Frame on Linux ARM64 with SteamVR/OpenXR and Vulkan through RT64/Plume; flat mode remains the default.

| Evidence | Verified status |
| --- | --- |
| Host/source | ROM-free production VR bridge and RT64 eye-slot tests compiled on macOS ARM64; CTest 1/1 passed on 2026-10-11 for the PR0 production-source changes. The public [ROM-free workflow](.github/workflows/public-vr-validation.yml) is configured without secrets; the Ubuntu 22.04 GitHub run passed at the immutable source commit in PR0_METADATA.json. |
| Unattended Frame | Earlier builds verified OpenXR startup, stereo rendering, scene routing, menus and diagnostics. Device profiles were accepted; no real controller button press was confirmed to reach the game. |
| Worn | Two predecessor builds were worn 2026-10-06. The current source has not been worn; focused wearer acceptance and current-source performance remain pending. |

**Known failures and limits:** game button input remains unconfirmed, recent camera changes are headset-unverified, and current-source performance is unmeasured. No complete application or package build is claimed. Immutable tested source and CI run are recorded in [PR0 metadata](docs/PR0_METADATA.json).

**External game data:** a personally owned US 1.0 DK64 ROM is required for the complete native build and gameplay. ROM and generated game data stay outside Git and CI artifacts.

Run the public, ROM-free production-source checks from a clean checkout:

```sh
bash tools/vr/prepare-public-dependencies.sh
cmake -S src/vr/tests -B build-vrtest -G Ninja
cmake --build build-vrtest --parallel 2
ctest --test-dir build-vrtest --output-on-failure
```

A complete local build requires your owned ROM and its existing hash validation:

```sh
DK64_ROM="/path/to/owned-us-rom.z64" DK64_VR=1 JOBS=4 tools/docker/build-linux.sh all
```

See [PR0 readiness and evidence](docs/PR0_READINESS.md), [VR architecture and wearer checklist](docs/VR_ARCHITECTURE.md), and the [README retained for history](docs/README_PRE_PR0.md). Upstream licenses and notices remain in their original files.

The host application retains its upstream [GPLv3 notice](COPYING); RT64 and Plume retain their own MIT notices. These code licenses do not grant permission to redistribute the commercial game, ROMs or extracted media. Immutable source/CI identity is in [PR0 metadata](docs/PR0_METADATA.json).

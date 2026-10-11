# PR0 release-readiness record — Donkey Kong 64 VR

**State: EXPERIMENTAL; no public Steam Frame build or release.** Source, device and wearer evidence are distinguished below.

## Identity and target

- Upstream: [Rainchus/Donkey-Kong-64-Recompiled](https://github.com/Rainchus/Donkey-Kong-64-Recompiled).
- Target: standalone Steam Frame, Linux ARM64, SteamVR/OpenXR, Vulkan via SDL2, RT64 and Plume.
- Source baseline before PR0 edits: `25848f4bdc8f26d5864721eadbc5e2c88ff782a1`.
- Tested source: baseline plus the PR0 working-tree change set on 2026-10-11; final integrated commit ID is pending coordinator integration.
- Intended runtime target: Steam Frame standalone, Linux ARM64, SteamVR/OpenXR and Vulkan; experimental, with no general platform-support claim.
- Host validation: macOS ARM64; the public workflow is configured for Ubuntu 22.04 but has not yet produced a remote GitHub run.
- RT64 public base: `43373749dac9bbc1b653e6a02aed40a9e1783bed`; VR overlay SHA-256: `b12913902ff8578b7b288ec306996f633510e2bd4554bd9b2a9a198564325bcb`.
- Plume public base: `d890ac899e505fb30040e037a4037cdeca68f033`; VR overlay SHA-256: `99fbdb9731416e35cbc5854bf977cf8fc3679655246462f6e6fed145d18294db`.

The bootstrap checks exact base commits and patch hashes, then applies tracked VR source overlays to clean public checkouts. It keeps RT64 and Plume license/notice files intact and removes the private submodule URLs.

## Evidence

- **Host/source:** ROM-free production VR bridge + RT64 eye-slot CMake/CTest: 1/1 passed on 2026-10-11 in a fresh macOS ARM64 checkout with an empty HOME and no Git credentials.
  ```sh
  bash tools/vr/prepare-public-dependencies.sh
  cmake -S src/vr/tests -B build-vrtest -G Ninja
  cmake --build build-vrtest --parallel 2
  ctest --test-dir build-vrtest --output-on-failure
  ```
- **Public CI:** Configured for GitHub push/PR with no repository secrets or ROM; its remote GitHub run has not yet been observed. The [workflow](../.github/workflows/public-vr-validation.yml) fetches public sources and runs CMake/CTest.
- **Unattended Frame:** Earlier builds verified OpenXR startup, stereo rendering, scene routing, menus and diagnostics; the headset accepted device profiles, but no real controller button press was confirmed to reach the game.
- **Worn:** Two earlier builds were worn on 2026-10-06. The current source changes have not been worn; focused wearer acceptance and current-source performance remain pending. See [VR camera check](VR_CAMERA_CHECK.md).
- **Performance:** earlier off-head measurements are in the architecture document. Current-source worn performance is unmeasured.

## Clean-checkout boundary

The clean host run used the source baseline plus the PR0 working-tree change set listed above; the final integrated commit ID is pending coordinator integration. Each checkout used a separate empty HOME, disabled system/global Git configuration, no SSH agent or GitHub token, and fetched only public sources.

The ROM-free test path initialized the public RT64/Plume bases, verified overlay SHA-256 values, applied the tracked overlays, compiled production VR bridge sources, and passed CTest 1/1. This is source/test evidence on macOS ARM64, not a full runtime support claim. The GitHub workflow is configured for Ubuntu 22.04 but has not yet produced a remote run.

The tests do not build the complete game executable or a package.

A complete build and playable run require the locally held US 1.0 DK64 ROM and existing hash validation. No ROM was copied here or used by CI; no complete application build is claimed.

## External data and full build

The US 1.0 ROM and generated recompiler/game data are local external inputs, excluded from source and CI artifacts. Use only data you own and keep it outside the public tree.

```sh
DK64_ROM="/path/to/owned-us-rom.z64" DK64_VR=1 JOBS=4 tools/docker/build-linux.sh all
```

See [VR architecture](VR_ARCHITECTURE.md) for implementation detail, historical evidence and remaining acceptance checks.

## Legacy upstream CI

The inherited full-build workflows fetch private data and are disabled by default in this fork. To opt in deliberately, set the repository Actions variable `RUN_LEGACY_VALIDATION` to the exact string `true` and configure the required repository secrets: `SECRET_NAME` (private extra-data repository), `SECRET_TOKEN`, and `SECRET_ENCRYPTION`. The automatic `public-vr-validation` workflow is independent and requires no secrets or ROM.

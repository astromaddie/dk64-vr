# DK64 VR (Steam Frame) architecture

Current state of the VR layer in this fork of Donkey Kong 64: Recompiled (N64Recomp + RT64 +
N64ModernRuntime). All VR code is behind the CMake option `DK64_VR`; with it off the flat build is
unchanged. Chronology, if any is needed, goes in a separate lab log, not here.

Nothing here has been worn. Every row of the status table is headset-unverified.

## 1. Target and probe facts

- Native Linux ARM64 title (SDL2 + Vulkan through RT64/plume), devkit title `DK64VR`, compat tool
  `SteamLinuxRuntime_4-arm64`. Persistent data in `$XDG_DATA_HOME`.
- Display target (platform standard): 120 Hz from SteamVR's per-app setting, 2160 x 2160 per eye
  (`resolution_scale` 1.25), sRGB swapchains written through a UNORM view or raw-copied.
- Device (2026-10-05, read over SSH): Turnip Adreno 750, Vulkan 1.4, `dualSrcBlend`, descriptor
  indexing, `depthClamp`, `shaderInt64` (no `shaderFloat64`). RT64 needs dual-source blending.
- ROM: the user's US 1.0 cartridge dump (sha1 cf806ff2...). Never committed.

## 2. Submodules carrying port commits

| Path | Port remote (`vr`) | Branch |
| --- | --- | --- |
| `lib/rt64` | `astromaddie/dk64-vr-rt64` (private) | `steam-frame` |
| `lib/rt64/src/contrib/plume` | `astromaddie/dk64-vr-plume` (private) | `steam-frame` |
| `lib/RecompFrontend` | `astromaddie/dk64-vr-recompfrontend` (private) | `steam-frame` |

Push submodule commits before the superproject commit that records them.

## 3. Game facts

- Simulation: fixed 30 Hz in play (`getFrameDelta()` 2, `patches/timing_fixes.c:178`), 20 Hz in some
  intro scenes (delta 3). The patch reports the rate to RT64 with `gEXSetRefreshRate(60/delta)`.
- Camera: `guPerspective` (fov 45, near 10 units) then `guLookAt` multiplied onto the projection stack
  (`lib/dk64_decomp/src/global_asm/code_450.c:657-675`, `patches/patches_ui.c:3003-3005`); RT64 splits
  it into float view and projection (`lib/rt64/src/hle/rt64_rsp.cpp:142-161`). The camera projection's
  matrix group is `MTXTAG_CAMERAPROJECTION` (5). First person narrows fov to 13-15 degrees.
- Input: `recompinput::profiles::get_n64_input` (`src/main/main.cpp:843-848`).

## 4. Frame protocol

- RT64's workload thread is the XR frame thread. `xrWaitFrame` paces it.
- Each XR frame: wait, begin, then take the newest workload without blocking. A new workload is consumed
  once (game frame advance, matching, interpolation target), never per eye.
- The eye passes replay that workload once per eye through `threadRenderFrame` with override targets.
  Interpolation weight w = clamp(1 - (T(N) - predictedDisplayTime) / tick, 0, 1) on an ideal tick grid
  (sfvr scheduler, as in sm64-vr). With no new workload, the held pair is re-rendered with the new head
  pose, so frames keep flowing through loads.
- RT64's idle GPU keep-alive thread and its own present pacing are off in VR.
- Standby (`shouldRender` false) freezes the game and keeps polling.

## 5. Eye camera

Injected in `ProjectionProcessor::processScene` (`lib/rt64/src/render/rt64_projection_processor.cpp`),
beside RT64's debugger camera override. The world camera is the perspective projection with group 5 in
the frame's main colour target. Its view becomes `level_pitch(lerped game view) * inverse(eye pose in
game-camera space, scaled by world_scale)`, and its projection is built from the runtime's per-eye FOV
tangents with the game's depth terms. Fog is computed from z/w, so a different eye near plane gets a
linear fog remap (lessons 45, 90). The game camera is the head origin; the head adds rotation and
position, never the game's own turns.

World scale: placeholder `world_scale` 25 units per metre. Settle it from the logged vertex extent of
the player model.

## 6. Draw inventory

| Item | VR rule |
| --- | --- |
| Gradient and textured skyboxes (screen-space, `patches_framebuffer.c:785-995`) | Sky dome around the eye drawn through the camera projection |
| Transitions, zipper, framebuffer snapshots | Panel mode plus a compositor fade |
| HUD texrects and sprites (`gEXSetRectAlign`) | HUD-only pass, alpha solved from black and white renders, near head-leashed quad, no roll |
| In-world billboards | Unchanged at first; yaw-only facing later |
| Lens flare | Dropped from the eye passes |
| Actor and frustum culling (`patches_main.c:410-458`, `:1197-1235`) | Off in VR |
| Chunk visibility | To be checked with yaw-180 dumps |
| Scissor, viewport, letterbox | Full eye target for camera projections |
| Aspect correction (`G_EX_ASPECT_AUTO`) | Original aspect in VR; the eye FOV supplies the frustum |
| First person and scope | Eye FOV fixed; picture-in-picture scope later |
| Cutscenes | 3D cinema on a projection layer (default) |
| Arcade, Jetpac, main menu, pause, split screen, recompui menus | World-locked flat panel over a black projection layer |

## 7. Status

| Feature | State |
| --- | --- |
| Linux ARM64 flat build and package | in progress |
| OpenXR host, plume Vulkan binding | not started |
| Frame driver, per-eye replay, eye camera | not started |
| Game patches (culling off, sky dome, scene reporting) | not started |
| HUD quad, flat panel, input, system layer | not started |
| Diagnostics (`force_render`, `diag_yaw`, `dump_frame`, `[vr-perf]`) | not started |

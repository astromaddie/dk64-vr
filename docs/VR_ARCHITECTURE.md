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

Code: `src/vr/xr/xr_host.cpp` (OpenXR host), `lib/rt64/src/hle/rt64_vr.h` (RT64's host interface),
`WorkloadQueue::renderThreadLoopVR` (`lib/rt64/src/hle/rt64_workload_queue.cpp`), the bridge in
`src/vr/bridge/` (pure, tested by `src/vr/tests/`).

- Start-up: `main()` calls `dk64vr::hostInstall` before the renderer. It reads `vr.toml`, creates the
  OpenXR instance (`XR_KHR_vulkan_enable2`, optional `XR_FB_display_refresh_rate`) and sets plume's
  Vulkan create hooks, so the runtime creates the instance and device and picks the GPU. Without a
  runtime or headset it logs `[openxr] VR mode off` and the game runs flat.
- The session binds the DIRECT queue family's queue 0, which RT64 also submits to; every OpenXR call
  that may use the queue (begin/end frame, acquire/release) holds plume's mutex for that queue.
  Swapchain waits never hold it.
- RT64's workload thread is the XR frame thread. Each display frame: `xrWaitFrame`, `xrBeginFrame`,
  locate the eyes at `predictedDisplayTime`, then take at most one new workload without blocking (it
  must be complete and its present event processed). A new workload is consumed once: configuration,
  game frame advance, matching, interpolation target, camera classification, and the scheduler's
  arrival. It is never consumed per eye.
- Stereo frames replay the held pair (previous, newest game frame) once per eye through
  `threadRenderFrame`: the left eye into the main colour image itself (so later frames that read it stay
  coherent), the right eye into its own target. Each eye's command list ends with a raw copy of the
  RGBA8 UNORM image into the eye's `R8G8B8A8_SRGB` swapchain image (no re-encode; lessons 8, 42).
- Interpolation weight: w = clamp(1 - (T(N) - predictedDisplayTime) / tick, 0, 1) on an ideal grid
  T(N) = base + N·tick, tick = 1 / the game's reported rate (`gEXSetRefreshRate`). The grid sits one tick
  after arrival and follows the earliest arrival phase: an arrival up to one display period late is
  seamless; earlier ones move the grid at once, later ones pull it slowly; more than two ticks off
  re-anchors (`dk64vr_sched.*`). A rate change re-anchors after it holds for three arrivals.
- With no new workload, the held pair is rendered again with the new head pose, so frames keep flowing
  through loads.
- The barrier keeps the previous game frame's workload slot (and the newest) from being overwritten
  while the pair is held.
- Flat frames (panel scenes, standby, the probe) render each new workload once, at the panel's
  height in the original aspect. RT64's present queue then draws its usual output (VI renderer plus the
  recompui draw hook) into the panel swapchain (1920x1080 `B8G8R8A8_SRGB` through a UNORM view)
  instead of the window. The panel is not drawn while the eyes are shown.
- Layers: always one projection layer (black under the panel; black is drawn only until every eye image
  is black), plus the panel quad when no stereo image was produced. The panel is world-locked 2 m ahead
  of the recentred origin, 2.2 m wide.
- Off in VR: RT64's idle GPU keep-alive thread, its present pacing and frame generation, MSAA and
  high-precision targets. Nothing is presented to the window.
- Standby (`shouldRender` false): workloads are still consumed and rendered flat, so the game keeps
  running; nothing is submitted. `force_render` renders anyway with a synthetic straight-ahead head.
- Session: READY begins it and calls `dk64vr_set_active(1)` (the game patches' VR paths); STOPPING ends
  it; EXITING or LOSS_PENDING quits the game through `ultramodern::quit`.
- Stereo or panel: stereo while the game state's scene is gameplay, first person or cutscene and the
  frame has a camera projection; a change must hold for 12 display frames. No game state yet means
  panel. `diag_force_stereo` ignores the scene.

## 5. Eye camera

Injected in `ProjectionProcessor::processScene` (`lib/rt64/src/render/rt64_projection_processor.cpp`),
after RT64's view interpolation, for camera projections only: perspective projections drawn into the
frame's main colour image with matrix group 5 (`MTXTAG_CAMERAPROJECTION`) or 4
(`MTXTAG_SKYBOXBLEND`, which tags the world in maps with a gradient sky); if none carries either, every
perspective projection drawn there. The host (`XrHost::eyeCamera`) replaces:

- view = level(lerped game view) · inverse(eye pose in the head-origin frame, translation × world_scale);
  `level_pitch` (default on) drops the game camera's pitch and roll and keeps its heading;
- projection = the runtime's per-eye asymmetric FOV tangents in the game's projection form, keeping the
  game's depth terms and scale, so z/w, the depth buffer and fog are unchanged
  (`dk64vr::eyeProjection`; with symmetric tangents it equals `guPerspective`, tested).

The eye near plane is the game's (10 units, 0.4 m at 25 units/m), so no fog remap is applied;
`dk64vr::fogRemap` (tested) is ready for a smaller near. Head pose comes only from the runtime: eye
poses relative to the head from `xrLocateViews`, head from the VIEW space, both in LOCAL space. The game
camera is the head origin; the origin recentres on the first tracked frame and on
`XrEventDataReferenceSpaceChangePending` (yaw and position). The eye image is a stretched render of the
whole 320x240 frame at eye size (resolution scale eyeW/320 x eyeH/240, aspect correction and hor+
alignment off), so the eye FOV alone sets the frustum.

World scale: placeholder `world_scale` 25 units per metre. Measured (2026-10-06, DK Isles, the
`[vr] player extent` log): Donkey Kong's model spans 25.2 world units vertically, so at 25 he is 1.0 m
tall. About 15 would make him 1.7 m. To be settled at first wear.

## 5a. Settings and diagnostics

`vr.toml` in the app folder (`$APP_FOLDER_PATH`, `~/.local/share/DK64Recompiled` on the Frame), a
`[vr]` table of the sfvr registry keys plus the port keys below; `DK64_VR_<KEY>` environment variables
override it for the run. Port defaults: `refresh_rate` 120 (requested once through the FB extension,
nearest offered rate), `resolution_scale` 1.25 (2160² from the recommended 1728²).

| Port key | Default | Meaning |
| --- | --- | --- |
| `world_scale` | 25 | Game units per metre (5-100) |
| `level_pitch` | true | Drop the game camera's pitch and roll |
| `panel_distance`, `panel_width` | 2.0, 2.2 m | The flat panel |
| `diag_force_stereo` | false | Stereo whenever a camera projection exists, whatever the scene |
| `diag_autostart` | 0 s | Press A (every fourth press Start) every 1.25 s, from 5 s after start, until in gameplay for 3 s or the time is up |
| `diag_exit_seconds` | 0 s | Quit cleanly this long after the session starts |
| `dump_count`, `dump_interval`, `diag_yaw_step` | 1, 120, 0 | `dump_frame` repeats: e.g. 3, 300, 90 dumps yaw 0, 90 and 180 in one run |
| `dump_panel_frame` | 0 | Dump the panel every Nth present (up to 40) |

Standard diagnostics: `force_render` (render and keep the game running in standby, synthetic head
with level eyes the runtime's distance apart), `diag_yaw`, `dump_frame` (Nth stereo frame:
`vr-dumps/vr-eye{0,1}-NNN-yawD.png` read back from the submitted swapchain images, written by a
background thread, with a `[vr] dump` state line), `probe_seconds` (each eye cleared red/blue first;
`vr-probe{0,1}` dumps when `dump_frame` is set), `timing` (`[vr-perf]` every 10 s with fps, missed
frames, CPU stages wait/frame/end, per-eye wall ms, GPU ms from RT64's timestamp queries, display rate,
eye size, the game's frame rate and grid rebases). Always logged once: runtime, Vulkan range, offered
refresh rates, `predictedDisplayPeriod`, swapchain formats and sizes, eye distance and FOV, camera
classification and game fov/near/far. `[vr-stall]` when no frame passes a checkpoint for 3 s.

Steam launches take diagnostics from a one-shot `vr-diag` file in the app folder, read and deleted by
`LAUNCH-STEAM-FRAME.sh`: lines `DK64_VR_<KEY>=<value>`, and `ARGS=--game dk64` to skip the launcher.
The log says `[vr] diag: ...` or `[vr] diag: no vr-diag file`.

## 5b. Measured (unattended, off-head, Steam launch, 2026-10-06)

Display ran at 72 Hz off-head (the FB extension listed only 120). DK Isles gameplay, both eyes:

| Eye size | GPU (both eyes) | fps at 72 Hz | Missed / 10 s |
| --- | --- | --- | --- |
| 2160² (scale 1.25) | 13.8-14.9 ms | 36 (half rate) | n/a |
| 1728² (scale 1.0) | 8.4-8.9 ms | 70-72 | 0-20 |
| 1296² (scale 0.75) | 5.3 ms | 72 | 0 |

The GPU sits at 903 MHz throughout at 2160²: GPU-bound (RT64's passes plus the compositor). Each eye
waits for its GPU work before the next is recorded, so the left eye's wall time also absorbs the
compositor's GPU time. 120 Hz is out of reach as built.

## 6. Draw inventory

| Item | VR rule |
| --- | --- |
| Gradient and textured skyboxes (screen-space, `patches_framebuffer.c:785-995`) | Sky dome around the eye drawn through the camera projection |
| Transitions, zipper, framebuffer snapshots | Panel mode plus a compositor fade |
| HUD texrects and sprites (`gEXSetRectAlign`) | HUD-only pass, alpha solved from black and white renders, near head-leashed quad, no roll |
| In-world billboards | Unchanged at first; yaw-only facing later |
| Lens flare | Dropped from the eye passes |
| Underwater tint (`func_global_asm_80701CA0`, screen-space fill rect) | In the eye at screen depth for now; check whether a full-eye tint is fine |
| Cutscene letterbox bars | Head-locked black bars in the eyes (seen in dumps); go away with the 3D cinema |
| Actor and frustum culling (`patches_main.c:410-458`, `:1197-1235`) | Off in VR |
| Chunk visibility | To be checked with yaw-180 dumps |
| Scissor, viewport, letterbox | Full eye target for camera projections (gameplay dumps show the full eye; no override built) |
| Aspect correction (`G_EX_ASPECT_AUTO`) | Original aspect in VR; the eye FOV supplies the frustum |
| First person and scope | Eye FOV fixed; picture-in-picture scope later |
| Cutscenes | 3D cinema on a projection layer (default) |
| Arcade, Jetpac, main menu, pause, split screen, recompui menus | World-locked flat panel over a black projection layer |

## 7. Status

| Feature | State |
| --- | --- |
| Linux ARM64 flat build and package | in progress |
| OpenXR host, plume Vulkan binding | verified unattended (probe colours per eye, session lifecycle, clean exit) |
| Frame driver, per-eye replay, eye camera | verified unattended (stereo eye dumps in DK Isles at yaw 0, 90, 180; 72 fps at 1728²) |
| Flat panel for menus and transitions | verified unattended (panel dumps of the boot logos and file select) |
| Game patches (culling off, sky dome, scene reporting) | not started |
| HUD quad, flat panel, input, system layer | not started |
| Diagnostics (`force_render`, `diag_yaw`, `dump_frame`, `probe_seconds`, `[vr-perf]`, autostart, self-exit) | verified unattended |

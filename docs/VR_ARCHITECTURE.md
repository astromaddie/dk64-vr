# DK64 VR (Steam Frame) architecture

Current state of the VR layer in this fork of Donkey Kong 64: Recompiled (N64Recomp + RT64 +
N64ModernRuntime). All VR code is behind the CMake option `DK64_VR`; with it off the flat build is
unchanged. Chronology, if any is needed, goes in a separate lab log, not here.

Nothing here has been worn. Every row of the status table is headset-unverified.

## 1. Target and probe facts

- Native Linux ARM64 title (SDL2 + Vulkan through RT64/plume), devkit title `DK64VR`, compat tool
  `SteamLinuxRuntime_4-arm64`. Persistent data in `$XDG_DATA_HOME`.
- Display: the platform standard asks for 120 Hz at about 2160 x 2160 an eye. This port renders
  2160 x 2160 an eye (`resolution_scale` 1.25) and holds 72 Hz there with no missed frames; the eye
  passes take about 5.8 ms of GPU a frame (section 5b). 90 and 120 Hz are not measured yet: unattended
  runs get the title's per-app rate, and DK64VR has no per-app entry (72 Hz off-head). Eye images
  are drawn into the swapchain images through a UNORM view (bits unchanged).
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
- Pause: whenever the session is not FOCUSED or the headset is in standby (`shouldRender` false), and
  `force_render` is off, the game is frozen: the game thread waits inside the once-per-frame game-state
  publish (`src/game/vr_api.cpp`, `dk64vr_control.h`), after its display lists are out, so no lock is
  held. The XR loop keeps polling at the display rate, and the audio queue runs dry (silence). A quit
  still goes through while frozen (the wait polls the runtime's `exited`).
- Layers, in submission order: the projection layer (eyes, black under the panel, or the cinema
  views), the panel quad, the HUD quad, the black blink quad (view space, mode changes), then the
  pointer's beam and dot, always last (standard.md section 3).
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

World scale: `world_scale` 15 units per metre. Measured (2026-10-06, DK Isles, the `[vr] player
extent` log): Donkey Kong's model spans 25.2 world units vertically, so he is about 1.7 m tall. Later
runs of the same log gave 32-42 units because its 40-unit radius picks up props near the hut; the 25.2
reading is the clean one. To be settled at first wear.

`head_translation` scales the head's displacement from the recentre origin, never the eye offsets.

## 5a. Settings and diagnostics

`vr.toml` in the app folder (`$APP_FOLDER_PATH`, `~/.local/share/DK64Recompiled` on the Frame), a
`[vr]` table of the sfvr registry keys plus the port keys below; `DK64_VR_<KEY>` environment variables
override it for the run. Port defaults: `refresh_rate` 72 (requested once through the FB extension,
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
| `port_schema` | 4 | Migration marker: a file below 4 that still holds an old default takes the new one once (world_scale 25, resolution_scale 1.25 then 1.0, refresh_rate 120, hud_distance 2, snap_blink 0.15) |
| `right_stick_x` | `turn` | `turn`: comfort turn (`turn_mode`, `snap_angle`, `smooth_turn_speed`); `camera`: the game's own C-left/C-right camera swing |
| `move_from_view` | true | Left stick relative to where you look (`move_relative`: head, left or right hand) instead of the game camera |
| `snap_blink` | 0 s | Optional black blink after a snap turn (the turn is instant without it) |
| `cutscenes` | `cinema` | `cinema` (3D screen), `immersive` (head in the game camera), `panel` (flat) |
| `cinema_distance`, `cinema_width` | 3.0, 3.6 m | The cinema screen |
| `cinema_depth` | 0.6 | On-screen separation of a point at infinity, as a fraction of the eye distance (0 = flat) |
| `diag_menu_at` | 0 s | Open the recomp menu (a View press) this long after the session starts |
| `diag_pointer` | false | A synthetic right hand aiming at the panel when no hand is tracked |
| `diag_snap_seconds` | 0 s | A snap turn right every N s of play, with the game camera's and the shown heading logged for 12 frames |
| `diag_hud_seconds` | 0 s | Show the game's HUD counters every N s of play (through its own `func_global_asm_806F8BC4`), with an eye and HUD dump half a second later |
| `diag_start_at` | 0 s | Press Start for 0.2 s (the game's pause menu) |

Standard keys this port uses: `turn_mode` (snap), `snap_angle` (30), `smooth_turn_speed` (120),
`move_relative` (head), `hud_distance` (1.5 m; port default), `hud_scale` (100: 40 degrees wide),
`hud_visible`, `haptics` (0.6), `dominant_hand` (right: the pointer hand), `head_translation`,
`refresh_rate` (72; port default), `resolution_scale` (1.25; port default). Not used yet: `vignette`,
`roomscale`, `lean_limit`, `duck_crouch`, `height_offset`, `seated`, `horizon_lock`,
`follow_vehicle_rotation`, `camera_shake`, `hud_mode` (only `near` exists).

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
`LAUNCH-STEAM-FRAME.sh`: lines `DK64_VR_<KEY>=<value>` or `RT64_VR_<NAME>=<value>`, and
`ARGS=--game dk64` to skip the launcher. The log says `[vr] diag: ...` or `[vr] diag: no vr-diag file`.
`tools/steam_frame/unattended.sh <seconds> <out> KEY=VALUE...` arms that file, launches the title
through `~/.steam/steam.pipe`, waits, stops it if needed and fetches the log and dumps (book the Frame
first).

More diagnostics: the `[vr-perf]` line also carries `mode=` (flat, stereo, cinema), the HUD's cost and
how often its signature changed. `dump_frame` also writes `vr-hud-NNN.png` (with alpha) when a HUD image
is drawn. With dumps or `dump_panel_frame` on, `[vr] layers:` lines give every layer's pose, size, FOV and
image rectangle; `tools/steam_frame/composite.py` rebuilds what SteamVR composites for one eye from them
and the dumps (numpy and Pillow). `RT64_VR_LOG_HUD=1` lists the HUD's draws every 300th game frame;
`RT64_VR_HUD_DEBUG=1` shows the black and white HUD renders side by side instead of the solved image.
`RT64_VR_LOG_HUD=<game frames>` lists the HUD's draws (with rects and scissors) that often.

Performance diagnostics (vr-diag lines; `TU_*`, `FD_*`, `MESA_*` and `VK_*` driver variables pass
through too, e.g. `TU_DEBUG=perf` for Turnip's LRZ reasons): with `timing`, a second `[vr-perf] eyes`
line gives per eye (left/right) the GPU time of RT64's passes and of the eye copy, the worst frame,
CPU record, submit and GPU-wait time, render passes begun and their area, draws, ubershader draws,
decal draws, depth framebuffer switches, vertex depth tests and clears. `RT64_VR_PASS_TIMES=1`
timestamps every render pass break twice a 1200 eye passes (`[vr-pass]`); `RT64_VR_LOG_DRAWS=1`
lists one right-eye pass's draws (`[vr-draw]`). Switches, all on by default: `RT64_VR_DEFER_CLEARS`,
`RT64_VR_RO_DEPTH_LOAD`, `RT64_VR_DEFER_WAIT`, `RT64_VR_HIDDEN_MASK` (=0 off),
`RT64_VR_EYE_COPY=transfer` (the old copy). Measurement only: `RT64_VR_EXP_POINT=1` (point
sampling), `RT64_VR_EYE_ORDER=swap`, `RT64_VR_RIGHT_TARGET=main`, `RT64_VR_SAME_EYE=1`.
`[vr] input:` lines log each profile's suggest result, the action set attach, each hand's interaction
profile and any change of held N64 buttons.

## 5b. Measured (unattended, off-head, Steam launch, 2026-10-06)

Current build (branch `vr-perf`), three runs a setting, DK Isles start position, gameplay windows of
10 s after the intro (the first one includes the transition), 72 Hz display:

| Build | Eye | GPU a frame (passes + copy) | fps (min-max) | Missed / min, 3 runs | Cinema missed / min |
| --- | --- | --- | --- | --- | --- |
| Before (`vr-play`, 7c3ea340) | 2160² | 14.6 ms + 2.3 ms copy | 36 (half rate) | 15, 0, 16 (at 36 Hz) | 80-96 |
| Before | 1728² | 8.4-8.5 ms + copy | 67.9-72.0 | 6.8, 15.8, 6.8 | 30-34 |
| After | 2160² | 5.3 + 0.5 = 5.8 ms | 70.5-72.0 | 1.5, 0, 0 | 0 |
| After | 1728² | 3.9 + 0.3 = 4.2 ms | 70.5-72.0 | 1.5, 0, 0 | 0 |

The 1.5 a minute is one window of 10 missed frames at the cutscene-to-gameplay switch.

Earlier measurements of the unchanged build:

Display ran at 72 Hz off-head (the FB extension listed only 120). DK Isles gameplay, both eyes:

| Eye size | GPU (both eyes) | fps at 72 Hz | Missed / 10 s |
| --- | --- | --- | --- |
| 2160² (scale 1.25) | 13.8-14.9 ms | 36 (half rate) | n/a |
| 1728² (scale 1.0) | 8.4-8.9 ms | 70-72 | 0-20 |
| 1296² (scale 0.75) | 5.3 ms | 72 | 0 |

The GPU sits at 903 MHz throughout at 2160²: GPU-bound (RT64's passes plus the compositor). Each eye
waits for its GPU work before the next is recorded, so the left eye's wall time also absorbs the
compositor's GPU time. 120 Hz is out of reach as built.

With the HUD split (2026-10-06, same conditions, 1728², several runs): gameplay with the HUD counters
forced on every 5 s ran 69-72 fps with 0-27 missed frames per 10 s and 8.4-9.2 ms GPU; cutscenes in the
cinema 70.8-71.8 fps, 1-10 missed. Every 10 s window is within the earlier range except one run at
27 missed. A first version that rendered the HUD in two extra full RT64 passes cost 2-4 ms of CPU per game
frame and doubled the missed frames; the HUD now rides along in the left eye's pass at half resolution.

### Performance work (branch `vr-perf`, running table)

Unattended, off-head, Steam launch, DK Isles start position, 2160² an eye, display 72 Hz. GPU is
RT64's passes an eye (timestamps), copy is the eye-to-swapchain copy an eye; fps and missed from
`[vr-perf]` in gameplay. One run unless noted.

| Step | GPU eye L / R | Copy | GPU a frame | fps | Missed / 10 s |
| --- | --- | --- | --- | --- | --- |
| Baseline (instrumented, unchanged behaviour) | 6.80 / 8.11 ms | 1.17 ms | 17.3 ms | 36 (half rate) | 0 |
| Clears as load ops, read-only depth loads | 6.79 / 8.14 | 1.19 | 17.3 | 36 | 0 |
| + far clip as a clip distance (no discard) | 6.05 / 7.17 | 1.17 | 15.6 | 36 | 0 |
| + eye copy as a draw | 5.95 / 7.25 (4 runs, ±0.1) | 0.29 | 13.8 | 36 | 0 |
| + uniform texture index for non-LOD draws | 4.96 / 5.15 | 0.30 | 10.7 | 69.5 | 18-26 |
| + wave-uniform texture index for LOD draws | 2.90 / 2.87 | 0.31 | 6.4 | 72.0 | 0 |
| + right eye not waited for (CPU only: right eye wall 4.1 to 0.6 ms) | 2.91 / 2.86 | 0.31 | 6.4 | 72.0 | 0 |
| + hidden-area mask (15.7% of each eye) | 2.69 / 2.62 | 0.27 | 5.86 | 72.0 | 0 |

Where the frame goes now (2160², per frame): the left eye's GPU wait is about 8.5 ms for 3 ms of its
own work and the deferred right eye still runs 3 ms into the next frame, at a GPU clock pinned at
903 MHz; at 1296² (2.9 ms of GPU) the left eye still waits 10 ms. That wait is SteamVR's pacing and
its compositor sharing the GPU, not the eye passes, so the 72 Hz runs say nothing yet about the
headroom at 90 or 120 Hz: that needs the title's per-app rate set to 90 or 120 and a Steam launch.

Findings: Turnip renders every pass in sysmem (TU_DEBUG=sysmem changes nothing; gmem is slower), so
pass breaks (16 an eye, 14 of them decal depth switches) cost 0.01-0.05 ms each and are not the cost.
The cost is fragment shading: forcing one point tap per texture (`RT64_VR_EXP_POINT`, a measurement
only) gives 2.85 ms an eye, and so does dropping the non-uniform texture access everywhere (which is
wrong for LOD draws). Adreno's non-uniform sample was the expensive part, not the filtering; LOD
draws now take the uniform path whenever their wave agrees on the tile. The right eye's extra
~1.2 ms is the compositor preempting whichever eye is rendered second (`RT64_VR_EYE_ORDER=swap`
moves it to the left eye; the same camera or the same target for both eyes doesn't remove it).

HUD text clipped in the speech bubble (fixed): the flat output of the same moment shows the whole
text, centred. In the VR passes RT64 widened every projection with a viewport narrower than the frame
by the eye's axis-scale ratio (4:3), so the text overran the game's own text-box scissor
(27.75,65.75-164.5,145.5); `DrawParams::vrStretched` fixes it for eye and HUD passes.

Parity: each lever's dumps were compared by eye with the previous build's at the same camera
(decal shadows, fog, three-point filtering, sky, the HUD image): unchanged apart from DK's idle
animation, the HUD fix above and the hidden-area corners (black, never shown by the lenses). Before
and after at the same camera: `vr-perf` scratch `r01-base2160` vs `r23-parity` (not committed).

## 6. Draw inventory

| Item | VR rule |
| --- | --- |
| Gradient and textured skyboxes (screen-space, `patches_framebuffer.c:785-995`) | Sky dome around the eye drawn through the camera projection |
| Transitions, zipper, framebuffer snapshots | Panel mode; a 0.2 s black cut on every flat/stereo/cinema change (no fade yet) |
| HUD: counters, icons, text, speech bubbles, the first-person crosshair (orthographic sprites from `func_global_asm_80715E94`, `printStyledText`, `displayImage`; a few texrects) | Out of the eyes; drawn in the left eye's pass into black and white images and solved into the HUD quad (section 9) |
| In-world billboards | Unchanged at first; yaw-only facing later |
| Lens flare | Dropped from the eye passes |
| Underwater tint (`func_global_asm_80701CA0`, an untextured full-viewport quad), fades and pause darkening (full-screen fill rects) | Stay in the eyes: a 2D draw that covers the frame is a full-eye tint, not HUD |
| Cutscene letterbox bars (full-width fill rects, `patches_ui.c:3039-3064`) | Stay in the eye images; the cinema shows only the rows between them |
| Actor and frustum culling (`patches_main.c:410-458`, `:1197-1235`) | Off in VR |
| Chunk visibility | To be checked with yaw-180 dumps |
| Scissor, viewport, letterbox | Full eye target for camera projections (gameplay dumps show the full eye; no override built) |
| Aspect correction (`G_EX_ASPECT_AUTO`) | Original aspect in VR; the eye FOV supplies the frustum |
| First person and scope | Eye FOV fixed; picture-in-picture scope later |
| Cutscenes and scripted cameras | 3D cinema on a projection layer (section 9) |
| Arcade, Jetpac, main menu, pause, split screen, recompui menus | World-locked flat panel over a black projection layer |

## 7. Status

Nothing has been worn. "Unattended" means a Steam launch on the Frame with `force_render`, checked in
logs and dumps (and composites rebuilt from them); it never pressed a button.

| Feature | State |
| --- | --- |
| Linux ARM64 flat build and package | builds (flat and VR); flat not run on the device in this round |
| OpenXR host, plume Vulkan binding | verified unattended (probe colours per eye, session lifecycle, clean exit) |
| Frame driver, per-eye replay, eye camera | verified unattended (stereo eye dumps in DK Isles; 70-72 fps at 1728²) |
| Controller bindings: Frame first, Index and Touch | host-tested (every path against each profile's components, unique names); all three profiles accepted on the device; no button press seen yet |
| N64 pad merge, View short/hold, stick relative to the view | host-tested; not exercised on the device (no wearer) |
| Snap turn (instant on screen, game camera follows) | verified unattended (`diag_snap_seconds`: shown heading moves 30 degrees in one frame, the game camera arrives in 4-6 s) |
| Recomp menus on the panel, laser pointer | verified unattended (menu opened by a synthetic View press, synthetic pointer composited last, dot on the panel); clicking not exercised |
| Game menus, pause menu, transitions on the panel | verified unattended (title, file select, pause menu dumps) |
| HUD split and HUD quad | verified unattended (text and counters absent from the eyes, present with correct alpha in the HUD image, composite) |
| 3D cinema with letterbox crop | verified unattended (layer FOV from the screen corners, picture rows between the bars, composites); depth not judged by eye |
| Pause when unfocused or in standby, quit while paused | verified unattended (game rate 0 Hz while the XR loop runs at 72; clean quit) |
| Rumble Pak to haptics | built; not exercised (no wearer) |
| Diagnostics (above) | verified unattended |
| Vignette, fade (only a 0.2 s black cut), roomscale, seated mode, camera shake off, in-headset VR settings page | not started |

## 8. Controls

The Frame is read as a split Xbox-layout gamepad, starting from DK64 Recompiled's own gamepad defaults
(`src/game/config.cpp` `set_control_defaults` over recompinput's defaults), so buttons match the game's
own control descriptions. Merged into player 1's N64 input with any real pad (`dk64vr::mergeN64`).
Bindings and the map: `src/vr/bridge/dk64vr_input.{h,cpp}`; OpenXR: `src/vr/xr/xr_input.cpp`.

| Frame (Xbox name) | Hand | DK64 | Index fallback | Touch fallback |
| --- | --- | --- | --- | --- |
| A | right | N64 A: jump, confirm | right A | right A |
| X | right | N64 B: attack | right B | left X |
| Y | right | C-left (camera left) | left A | left Y |
| B | right | C-right (camera right) | left B | right B |
| LB (bumper) | left | C-down: zoom; Z + C-down fairy camera | left grip | left grip |
| RB (bumper) | right | C-up: first person; Z + C-up instrument | right grip | right grip |
| LT | left | N64 Z: crouch | left trigger | left trigger |
| RT | right | N64 R: centre camera; menu pointer click | right trigger | right trigger |
| Menu | right | Start: pause, skip | left stick click | left Menu |
| View, short | left | the recomp menu (DK64 Recompiled's Back) | right stick click | left stick click |
| View, hold 1 s | left | recentre, 0.6/80 ms buzz on both hands | as above | as above |
| D-pad | left | N64 D-pad | none | none |
| Left stick | left | N64 stick, relative to where you look | left stick | left stick |
| Right stick X | right | snap turn 30 degrees (`right_stick_x = camera`: the game's C-left/C-right) | right stick | right stick |
| Right stick Y | right | nothing in play (lesson 80); scrolls menus | right stick | right stick |
| Right stick click | right | N64 L (unused by the game) | none | right stick click |
| Left stick click, grips | | nothing | | |

In a recomp menu: D-pad or left stick navigate, A accepts, B goes back, Menu applies, the bumpers
switch tabs, the trigger clicks where the pointer is, View closes. The game's own menus (title, file
select, pause) take the N64 pad as usual.

Turning: the game eases its camera to a new heading over a second or more. A snap writes the camera's
heading (`CameraPaad::unkB2`, through the `recomp_vr_take_turn` import in the camera patch,
`patches/sound_options_patches.c` `func_global_asm_8061D1FC`) and, until the camera arrives, the eye
view is orbited about the camera's look-at point by what it still has to turn (`dk64vr::orbitView`),
so the wearer sees one instant step and the stick stays aligned. The default is the comfort snap
because the game's own C-left/C-right is a 45 degree smooth swing of the whole view, which is forced
smooth rotation. Turning applies in third-person play only, not in first person or cutscenes.

Haptics: the game's Rumble Pak (ultramodern `set_rumble`) gives 40 ms pulses on both hands while the
motor runs, scaled by `haptics`; the recentre buzz and a click on menu presses follow standard.md
section 6.

## 9. HUD, menus and cinema

HUD: RT64 classifies every game call of a frame (`threadVrClassifyCalls`): in the main color image,
draws up to and including the game camera stay in the eyes, so do perspective draws after it; 2D draws
after it are the HUD, unless they cover the frame (tints, fades) or are letterbox bars. The eye passes
skip the HUD calls. The left eye's pass draws them again as two extra framebuffer entries into
half-size images cleared to black and to white, and `VrHudSolvePS` solves colour and coverage
(a = 1 - mean(white - black), c = black / a; host-tested as `dk64vr::hudSolve`) into the HUD swapchain,
unpremultiplied. The quad follows the head's yaw and pitch, never roll, 1.5 m away and 40 degrees wide
(4:3); in first person it follows the game camera's aim instead, so the crosshair marks where shots go.
An unchanged HUD (hash of its draws' state, textures, vertices and matrices) is not redrawn; in
practice DK64's HUD animates every frame while it is up.

Menus: a recomp menu (or a View press on its way) switches to the panel at once. The panel is 2.2 m
wide, 2 m ahead of the recentre origin. The pointer is the dominant hand's `aim/pose` -Z ray; recompui
gets SDL mouse motion, button and wheel events through `recompui::queue_event`, and controller buttons
the same way. The game's own pause menu stays on the panel too: it is a 2D menu over a blurred
snapshot, which reads better on a still panel than floating in front of a frozen 3D scene.

Cinema: the cutscene scene (and scripted cameras, which the game reports as cutscenes) latches the
cinema on after 6 frames and off after 24. The screen is placed 3 m ahead of the head (yaw only, eye
height) when it comes on and on recentre, 3.6 m wide. Each eye's projection view has the screen's
orientation and the eye's position, and its FOV is the screen as that eye sees it (`dk64vr::screenFov`).
The game camera is split into two eyes `cinema_depth` x eye distance apart at infinity, frusta
converging at the game's near plane, so nothing drawn comes in front of the screen
(`dk64vr::cinemaEye`, host-tested). The image rectangle is the camera's scissor rows; DK64 narrows the
camera's viewport and aspect to the band between the bars, so rows map to tangents through the
viewport. The HUD quad sits 2 cm in front of the screen and spans the whole frame, so dialogue drawn in
a bar shows above or below the picture.

## 10. Wear checklist (about 15 minutes)

Build: branch `vr-perf`, installed as the `DK64VR` devkit title (binary md5
`2ccff3825d0f9999a904c68a3a503204`, no diagnostics armed). Before starting: in SteamVR's per-app settings set DK64VR to 72 Hz. Launch from the Steam
library (not SSH). Claim the Frame as `user-wearing`. Read the log afterwards with
`tools/steam_frame/deploy.sh --logs` (FRAME_HOST=steamos@192.168.1.31).

1. Launcher on the panel (2 min): the DK64 Recompiled launcher appears on a panel 2 m ahead. Point at
   it with the right hand: is there a beam and a dot, flat on the panel, never hidden? Click Start Game
   with the trigger. Then try A and the D-pad instead of the pointer.
2. Title and file select (1 min): the intro and title play on the cinema screen (3D?), the file select
   on the panel. Pick a file with A.
3. Intro cutscene (2 min): cinema screen 3 m ahead. Does the depth sit behind the screen? Is the speech
   bubble text readable, in front of the picture, and do bars ever follow your head? Does the screen
   stay put when you turn your head?
4. DK Isles, first minutes (4 min): walk around. Is DK about 1.7 m tall (`world_scale`)? Does the left
   stick go where you look? Snap turn with the right stick: one clean 30 degree step, no swing
   afterwards? Try Y/B (the game's camera swing) once for comparison. Jump (A), attack (X), crouch
   (LT), first person (RB) and back.
5. HUD (2 min): collect a banana or get hit so a counter shows. Is the HUD 1.5 m ahead, legible, free of
   roll when you tilt your head, in front of the world everywhere? Any HUD left in the world image?
6. Pause and menus (2 min): Menu pauses: the pause menu on the panel. View (short) opens the recomp
   menu: navigate with the stick, click with the trigger, close with View.
7. Recentre and focus (1 min): turn your body 90 degrees and hold View 1 s: buzz, view recentred behind
   DK. Lift the headset off for a few seconds: on return the game should have waited.
8. Answer: scale, HUD distance and size, snap angle, cinema distance and depth, any button that does
   not do what its label says, any discomfort.

## 11. Known risks for the first wear

- No button press has ever reached the game from the controllers: the input path is host-tested and
  the profiles are accepted on the device, but an unattended run has no wearer. If nothing responds,
  the log's `[vr] input:` lines tell whether samples are live.
- Snap turn: the game camera takes 4-6 s to arrive; the view is held at the new heading meanwhile. If
  something else turns the camera during that time (C-left/C-right, a scripted camera), the
  compensation is dropped and the view may jump to wherever the game camera is.
- The HUD is a 4:3 frame squeezed into 40 degrees; small text may be hard to read, and counters at the
  frame's edges sit 20 degrees off centre.
- Cutscene depth is computed, not seen: `cinema_depth` 0.6 may be too strong or too weak.
- Steam's Stop (SIGTERM) does not end a started game: SDL turns it into the recomp quit prompt. Quit
  from SteamVR's own menu instead (that path is clean, also while paused).
- First person: the crosshair follows the game camera's aim, but the head still looks around freely;
  untested.
- Frame time at 2160² and 72 Hz is unproblematic in the measured scenes (DK Isles, the intro
  cutscene); busier levels are unmeasured. 90 and 120 Hz have not been run.
- The hidden-area mask blackens the eye image's periphery; a game effect that later reads the main
  colour image (a pause snapshot) sees those corners black.

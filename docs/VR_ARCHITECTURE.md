# DK64 VR (Steam Frame) architecture

Current state of the VR layer in this fork of Donkey Kong 64: Recompiled (N64Recomp + RT64 +
N64ModernRuntime). All VR code is behind the CMake option `DK64_VR`; with it off the flat build is
unchanged. Chronology, if any is needed, goes in a separate lab log, not here.

Worn twice by the user on 2026-10-06 (builds before this one). Everything changed since is
headset-unverified, and the status table says so row by row.

## 1. Target and probe facts

- Native Linux ARM64 title (SDL2 + Vulkan through RT64/plume), devkit title `DK64VR`, compat tool
  `SteamLinuxRuntime_4-arm64`. Persistent data in `$XDG_DATA_HOME`.
- Display: the platform standard asks for 120 Hz at about 2160 x 2160 an eye. This port renders
  2160 x 2160 an eye whatever SteamVR recommends (`resolution_scale` auto, section 5a): SteamVR's
  per-app resolution moves the recommendation (1728 by default, 2160 with the user's per-app
  setting), and the old fixed 1.25 then rendered 2700 x 2700 at about 9.4 ms of GPU a frame, so
  SteamVR ran the app at 60 Hz instead of 120. At 2160 and 72 Hz the eye passes take about 5.8 ms
  of GPU a frame (section 5b). Unattended runs get the title's per-app rate (the user set DK64VR to
  120 Hz). Eye images are drawn into the swapchain images through a UNORM view (bits unchanged).
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
  high-precision targets. Nothing is presented to the window, and in VR the window is created
  hidden (`SDL_WINDOW_HIDDEN`, `dk64vr::hostActive`): a mapped window that is never drawn is what
  Steam registers as the title's desktop-game overlay (`valve.steam.desktopgame.<appid>`, "DK64:
  Recompiled", opaque, 0x0), which SteamVR can show as a black panel in front of the launcher
  (lesson 56; the user's first wear). Real pads still reach SDL without focus
  (`SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS`).
- Standby (`shouldRender` false): workloads are still consumed and rendered flat, so the game keeps
  running; nothing is submitted. `force_render` renders anyway with a synthetic straight-ahead head.
- Session: READY begins it and calls `dk64vr_set_active(1)` (the game patches' VR paths); STOPPING ends
  it; EXITING or LOSS_PENDING quits the game through `ultramodern::quit`.
- Pause: whenever the session is not FOCUSED or the headset is in standby (`shouldRender` false), and
  `force_render` is off, the game is frozen: the game thread waits inside the once-per-frame game-state
  publish (`src/game/vr_api.cpp`, `dk64vr_control.h`), after its display lists are out, so no lock is
  held. The XR loop keeps polling at the display rate, and the audio queue runs dry (silence). A quit
  still goes through while frozen (the wait polls the runtime's `exited`).
- Layers, in submission order: the projection layer (eyes, black under the panel, the cinema views,
  or the frozen world), the fade/dim quad (view space), the panel quad, the HUD quad, the black blink
  quad (view space, mode changes), then the pointer's beam and dot, always last (standard.md
  section 3). The fade, dim and blink use the UI image's black column in 16 alpha bands of 4 rows.
- Stereo or panel: stereo while the game state's scene is gameplay, first person or cutscene and the
  frame has a camera projection; a change must hold for 12 display frames. No game state yet means
  panel (boot logos, title, file select stay on the panel). `diag_force_stereo` ignores the scene.
- Transitions and pause (`XrHost::updateFreeze`): once in stereo, a map transition, the game's pause
  menu or a recomp menu no longer flips to the panel. The world freezes at once on the last stereo
  images, submitted again with the views they were made for (world-locked; the eye passes stop and
  workloads render flat). A transition fades it to black (0.15 s) and holds; the pause menu dims it
  (alpha 0.6) and its panel comes in front after 6 frames of pause (latched both ways); a recomp menu
  dims it and shows its panel at once. The world comes back when the scene has wanted stereo for 6
  frames, fading in from black (0.3 s). A transition into a menu stays black until the 12-frame latch
  moves to the panel. No turning while frozen. Logged as `[vr] freeze: ...`, `pause panel`,
  `menu panel`.

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
nearest offered rate), `resolution_scale` `auto`: an absolute eye size of 2160 on the longer side
(`dk64vr::eyeSize`, host-tested). A recommendation of 2160 or more is used as it is (scale 1.0), a
smaller one is scaled up to 2160, both clamped to the runtime's maximum. A number (0.75-1.5) still
multiplies the recommendation. The log says once `[vr] eye size: resolution_scale auto, recommended
AxA -> BxB (xS, target 2160, max ...)`.

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
| `foveation` | `high` | App-owned KHR shading-rate attachment on gameplay eye raster passes: `off`, `low`, `medium`, `high` (default since schema 5: worn, fixed high looked good; at 120 Hz / 2160² it holds 120.0 fps, 3.3 ms of GPU, where off runs 97-105 fps) |
| `foveation_eye_tracked` | true | Move the sharp region using valid `XR_EXT_eye_gaze_interaction` poses; otherwise use the asymmetric projection centre (worn: "looked invisible"). Every gaze/fixed switch is logged with its session time and count, and `[vr-fov] window:` counts them per 10 s |
| `dump_foveation` | false | Write `vr-fov-eye{0,1}-N.png`, a visualisation of the submitted rate-map bytes, once and with each eye dump |
| `port_schema` | 5 | Migration marker: a file below 5 that still holds an old default takes the new one once (world_scale 25, resolution_scale 1.25 then 1.0 then 1.25, refresh_rate 120, hud_distance 2, snap_blink 0.15; schema 5: resolution_scale 1.25 to `auto`, foveation off to high, foveation_eye_tracked false to true, snap_angle 30 to 45) |
| `camera_smoothing` | `medium` | Smoothing of the game camera (the head origin), section 5c: `off` (RT64's linear interpolation), `low` (B-spline through three game frames), `medium`, `high` (plus an orientation follower, 0.06 / 0.12 s) |
| `diag_camera_seconds` | 0 s | Camera motion without a wearer in stereo play, phases of N s: C-left tap, C-right tap, stick held forward-right, idle |
| `diag_camera_log` | false | A `[vr-cam]` line every display frame: weight, linear and smoothed camera heading, pitch and position |
| `diag_turn_stick` | 0 | Right stick X held at this deflection for 4 s, released for 4 s, in stereo play (a snap a hold, or the smooth orbit) |
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
| `diag_vr_menu_at`, `diag_vr_menu_step`, `diag_vr_menu_tab` | 0 s, 2.5 s, -1 | A tour of the VR settings menu (section 9a) from this time, in stereo play if the run gets there within another minute: an eye dump of the world just before, then per step a `vr-menu-<step>-NNN.png` panel dump (with alpha) and a layers line. `diag_vr_menu_tab` 0-4 opens that tab and leaves it up instead. Changes no setting |

Standard keys this port uses: `turn_mode` (snap; smooth orbits about Donkey Kong, section 8),
`snap_angle` (45; port default, the user's choice), `smooth_turn_speed` (120),
`move_relative` (head), `hud_distance` (1.5 m; port default), `hud_scale` (100: 40 degrees wide),
`hud_visible`, `haptics` (0.6), `dominant_hand` (right: the pointer hand), `head_translation`,
`refresh_rate` (72; port default), `resolution_scale` (`auto`; port default). Not used yet: `vignette`,
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
`LAUNCH-STEAM-FRAME.sh`: lines `DK64_VR_<KEY>=<value>` or `RT64_VR_<NAME>=<value>`,
`FDM_DEBUG=<value>`, `DISABLE_VULKAN_FDM_INJECTION_LAYER=<value>`, and
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

## 5c. Camera smoothing

The game camera is the head origin, so its motion is felt directly: DK64 moves it at 30 Hz, RT64
interpolates linearly between the last two game frames, so its velocity changes in a step at every
game frame, and the game's own swings (C-left/C-right, auto-follow, the R re-centre) start and stop
at full speed. Code: `src/vr/bridge/dk64vr_camsmooth.*` (pure, host-tested), used by
`XrHost::updateCameraCorrection`.

- History: RT64 reports every new workload's own camera view (the first projection of matrix group
  5, else 4, else the first perspective one in the main image; `VrWorkloadInfo::cameraView`). The
  smoother keeps the last three.
- `low`: a uniform quadratic B-spline through those three (Star Fox VR, lesson 31), evaluated at
  the display frame's interpolation weight: position and velocity continuous across game frames,
  half a game frame (17 ms) behind the linear path. `medium` / `high` add a critically damped
  follower on the camera's orientation only (smoothing time 0.06 / 0.12 s; at a steady turn it
  lags by rate x time), so a swing's start and stop are eased; position stays on the B-spline so
  Donkey Kong never drifts in the view.
- One rigid correction a display frame: inverse(linear) x smoothed, where `linear` reproduces RT64's
  own interpolation (view rotation slerped, view translation lerped), applied to every camera
  projection of both eyes before the snap-turn orbit, level pitch and the eye offsets. With
  smoothing off the correction is the identity; the log's `match=` is the largest distance between
  RT64's view and the reproduction in a window.
- Cuts reset the history and the follower to the new camera (no swoop): a scene change, a
  flat/stereo or cinema change, a recomp menu, and any jump of more than 60 units (4 m) or 52
  degrees between two game frames (map loads, respawns, cutscene cuts, first-person entry/exit,
  warps). Never smoothed: first person (the crosshair is the game camera's aim) and the cinema (the
  authored picture on a screen). Snap turns and recentring are unchanged (the orbit applies after
  the correction).
- Default `medium`: the B-spline alone removes the 30 Hz velocity steps but a C-button swing still
  starts and stops at full speed in about one game frame; 0.06 s eases that while keeping the lag
  of a fast swing to a few degrees.
- Diagnostics: with `timing`, `[vr-cam]` every 10 s gives, per display frame, the heading, pitch and
  position steps (sd, max) and how much the step changes from one frame to the next (sd, max; the
  abruptness) for the linear camera (`raw`, what `off` shows) and the one shown (`out`);
  `diag_camera_log` prints every frame; `diag_camera_seconds` drives the camera without a wearer.
  `[vr-cam] shown:` gives the heading rate after the turn orbit next to the turn the stick requested.

Measured (unattended, 120 Hz, 2160², DK Isles, `diag_camera_seconds=3`: C-left and C-right swings,
Donkey Kong running in a curve, idle; four steady 10 s windows, `raw` is what `off` shows):

| Per display frame | raw (linear) | out (medium) |
| --- | --- | --- |
| Heading step: sd / max | 0.17-0.21 / 1.17 deg | 0.15-0.20 / 1.02-1.04 deg |
| Heading step change: sd / max | 0.021-0.029 / 0.32-0.51 deg | 0.008-0.010 / 0.043-0.045 deg |
| Pitch step change: max | 0.017-0.019 deg | 0.003 deg |
| Position step change: sd / max | 0.14-0.17 / 2.29 units | 0.025-0.042 / 0.30-0.87 units |

So the frame-to-frame change of the heading's speed (the jolt at every 30 Hz game frame and at the
start and stop of a swing) is about 3x smaller in sd and 7-11x smaller at its worst, the position's
5x; the overall motion (step sd and max) is unchanged. `match=` stayed at 1.4 units / 0.01 degrees.

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

### Foveation design (2026-10-06; device-unverified)

The coordinator's probe reports Adreno 750 / Turnip Mesa 26.3-devel, Vulkan 1.4:
`VK_EXT_fragment_density_map` with non-subsampled images, but no dynamic density maps or fragment
density invocation built-ins; `VK_KHR_fragment_shading_rate` with attachment, pipeline and primitive
rates. Valve's implicit OpenXR/Vulkan FDM layer is loaded by SteamVR. Its symbols and strings show
swapchain-size matching, render-pass/framebuffer injection, FB profiles, META eye centres, and
`already_has_fdm`. These are evidence of possible behaviour, not a verified app contract.

Two routes are viable:

* **Valve injection:** enable `XR_FB_foveation`, `XR_FB_foveation_configuration`,
  `XR_FB_swapchain_update_state`, `XR_FB_foveation_vulkan` (and optional META eye tracking); chain
  `XrSwapchainCreateInfoFoveationFB` with the Vulkan foveation flag to each eye swapchain; create a
  level/dynamic profile and apply `XrSwapchainStateFoveationFB` with `xrUpdateSwapchainFB`.
  Vulkan foveation images can then be queried with `XrSwapchainImageFoveationVulkanFB`. An FB profile
  alone does not prove that RT64's separate eye target receives it. Exact size matching or rendering
  into the swapchain could make it eligible. The binary strings do not establish when it injects,
  how it associates two independent eye passes with the profiles, or whether normal RT64 targets
  remain sampleable. Size matching also cannot distinguish cinema renders of the same eye size.
  Direct swapchain rendering would change RT64's authoritative left-eye framebuffer and acquisition
  lifetime. This route needs the coordinator's `FDM_DEBUG` evidence before implementation decisions
  can rely on it; it is not the selected backend.
* **App-owned attachment:** EXT FDM on ordinary images would preserve RT64's sampling and copy paths
  because the device supports `fragmentDensityMapNonSubsampledImages`. Subsampled images would need
  special samplers and cannot retain the raw image-copy path. Non-dynamic FDM must be ready before
  render-pass recording; moving it with gaze would require a completed upload ahead of the eye pass.
  It also needs compatible FDM variants of graphics pipelines. The selected variant of this route is
  **KHR fragment shading rate**, an R8_UINT rate attachment on explicitly selected eye passes.
  It uses normal colour/depth images, existing pipelines with dynamic rate combiners, and uploads
  the map in the same command list before rasterisation. Only the optional attachment needs a
  render-pass-2 conversion in plume. This avoids an extra gaze-upload GPU wait and the unknown Valve
  injection contract. It reduces fragment invocations; it does not reduce target memory, geometry,
  framebuffer-copy bandwidth or compositor cost.

The device A/B and the user's wear made `high`, eye-tracked, the default at schema 5 (section 5a).
`foveation` is `off/low/medium/high`. Pure map generation includes
RT64 target padding, the asymmetric projection centre and tile coverage of the sharp disc.
Missing optional foveation keys take their off defaults without migrating or resetting
user-selected resolutions (the eye size default is `auto` since schema 5, section 5a).

Implementation: `src/vr/xr/xr_foveation.*` owns feature negotiation, per-eye R8_UINT images, rate
uploads and diagnostic CPU mirrors. `VrHost::foveatedFramebuffer` selects a cached companion only
while recording the main eye's raster framebuffer; read-only-depth companions carry the same rate
image. Plume converts that optional pass to render-pass-2, preserving the original colour/depth
load/store descriptions. The same rate-aware factory creates its deferred-clear variants, carrying
the R8_UINT attachment and subpass pNext into `getClearRenderPass`; read-only-depth companions also
keep the performance work's LOAD/STORE_NONE choice. Ordinary passes retain their original
construction. Pipelines enable a dynamic KHR combiner only when the feature was enabled at device creation: 1x1 pipeline rate,
KEEP primitive rate, REPLACE attachment rate. Unattached passes use KEEP/KEEP, hence 1x1. Shared
ownership keeps each rate image alive until its last companion framebuffer is destroyed.
The hidden-area mesh is drawn at full rate into the same colour/depth targets before scene raster
foveation. Both optimisations apply to the eye; mask, eye-output draw and HUD solve passes remain
unfoveated. The deferred final-eye submission is finished by `threadVrFinishSubmission` before
the next scene records or uploads a map, preserving upload-buffer and image reuse safety.

The Vulkan device contract forbids FDM and KHR attachment-rate features together. VR start-up
defaults `DISABLE_VULKAN_FDM_INJECTION_LAYER` to 1 before layer enumeration, including `foveation=off`
so the baseline is explicit. A one-shot value 0 unsets the variable before loading and selects a
Valve-only diagnostic run; the app refuses its KHR backend (`reason=Valve-layer-selected`). This
lets the coordinator compare the previous layer-allowed baseline without enabling conflicting
features. Loaded/injected Valve state cannot be introspected by the app: it logs
`Valve_injected=unknown-see-FDM_DEBUG` when allowed. Confirm injection from layer diagnostics,
not from the presence of an XR extension or a matching target size.

Hazards handled:

* Colour/depth images are ordinary, non-subsampled allocations. RT64's authoritative left-eye
  target, framebuffer copies/readbacks/effects, colour/depth sampling for blending/decals, and the
  draw into the sRGB swapchain (and optional transfer-copy fallback) retain their image layouts and
  samplers. There is no compressed pixel coordinate space and no new gamma conversion. Eye shading is coarser only in the selected
  regions; copied peripheral shading naturally carries that approximation into subsequent effects.
* HUD black/white renders, HUD solve, panel, cinema, pointer, probe and offscreen targets never
  select a rate companion. HUD/crosshair/subtitle text remains full density. Even in a main eye
  pass, low-res 2D UV snapping, no-near-clip geometry, decals, combiner noise and alpha dither use a
  KEEP/KEEP combiner; post-blend noise also stays 1x1. This is dynamic state, without new decal/pass
  splits or shader edits, to keep the parallel RT64/plume work easy to merge.
* `RasterPS` still receives full-framebuffer `SV_Position`/FragCoord values: no density-map UV
  rescaling is applied. Native texture UVs/3-point filtering and depth values retain their
  coordinate meaning. Pixel-addressed depth comparisons and noise use the full-rate exceptions
  above. Specialized VR shaders retain the performance work's vertex far-clip distance; fallback
  shaders retain their fragment depth clip. Alpha thresholds and fallback clipping are evaluated
  at the coarse fragment's representative sample, so clip contours, texture LOD transitions and
  thin alpha geometry need visual A/B acceptance; pixel-exact equivalence there is not claimed.
* The map's white disc uses distance to the nearest point of each tile, protecting every tile
  that touches the sharp region. Low: radius 0.38 of the shorter eye dimension, then up to 2x2;
  medium: 0.28, 2x2 until 0.58, then up to 4x4; high: 0.20, 2x2 until 0.42, then up to 4x4.
  Device-supported single-sample rates cap these requests. The map covers the allocated target,
  including padding, while the centre is measured in the visible eye rectangle.
* Valid tracked gaze is sampled once at predicted display time through the existing input sync.
  It is transformed into each runtime eye's coordinates (before recenter/diagnostic rotations).
  Inactive, invalid, synthetic or unsupported gaze, or invalid runtime eye poses, immediately uses
  the projection centre. The EXT action supplies one gaze ray, not independent eye vergence. KHR
  permits its map copy before rasterisation in the current eye command list; no extra synchronous
  upload submission is needed.

Diagnostics: `[vr-fov]` once for capabilities/backend/profile/layer decision and once per new eye
target extent for map sizes and selected/excluded passes; gaze source on changes; map dump metadata
when requested. `[vr-perf] foveated_passes=N` counts actual attached render-pass starts in each
window; it is not an eye count or a GPU invocation measurement. `dump_foveation` writes the actual
CPU upload bytes as PNG (white 1x1, green up to 2x2, orange coarser); off/cinema produces an
unattached white reference. The app cannot dump Valve's map. [Device A/B plan](FOVEATION_TEST.md)
records exact one-shot files, three repeats, interpretation and the separate gaze/worn gate.

Primary contracts: [FB foveation](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#XR_FB_foveation),
[EXT density maps](https://docs.vulkan.org/samples/latest/samples/extensions/fragment_density_map/README.html),
[KHR rate attachment](https://docs.vulkan.org/refpages/latest/refpages/source/VkFragmentShadingRateAttachmentInfoKHR.html),
[dynamic combiners](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdSetFragmentShadingRateKHR.html).

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

Worn twice (2026-10-06) on earlier builds; the rows changed by the polish work below are
headset-unverified. "Unattended" means a Steam launch on the Frame with `force_render`, checked in
logs and dumps (and composites rebuilt from them); it never pressed a button.

| Feature | State |
| --- | --- |
| Linux ARM64 flat build and package | builds (flat and VR); flat not run on the device in this round |
| App-owned eye foveation | rebased onto performance work; 1,643 host checks and VR/flat ARM64 builds pass; not run on device |
| OpenXR host, plume Vulkan binding | verified unattended (probe colours per eye, session lifecycle, clean exit) |
| Frame driver, per-eye replay, eye camera | verified unattended (stereo eye dumps in DK Isles; 70-72 fps at 1728²) |
| Controller bindings: Frame first, Index and Touch | host-tested (every path against each profile's components, unique names); all three profiles accepted on the device; no button press seen yet |
| N64 pad merge, View short/hold, stick relative to the view | host-tested; not exercised on the device (no wearer) |
| Snap turn (instant on screen, game camera follows) | verified unattended (`diag_snap_seconds`: shown heading moves 30 degrees in one frame, the game camera arrives in 4-6 s); 45 degree default and unwrapped orbit headset-unverified |
| Smooth orbit about DK (`turn_mode` smooth) | verified unattended (`diag_turn_stick=1`: the shown heading turns at exactly the requested rate, 120 deg/s at full deflection, and stops on release); headset-unverified |
| Camera smoothing (`camera_smoothing` medium) | host-tested; verified unattended (`[vr-cam]` before/after, section 5c); headset-unverified |
| Transitions fade in stereo, pause/recomp menu over the frozen dimmed world | verified unattended (intro transitions freeze and fade, pause panel and recomp menu over the frozen world in the layer log); headset-unverified |
| Hidden desktop window (no black SteamVR box at the launcher) | verified unattended (the `valve.steam.desktopgame.<appid>` overlay is no longer created; launcher and game run); headset-unverified |
| Eye size `auto` (2160 whatever SteamVR recommends) | verified unattended (recommended 2160 -> 2160 at the user's per-app setting); headset-unverified |
| Recomp menus on the panel, laser pointer | verified unattended (menu opened by a synthetic View press, synthetic pointer composited last, dot on the panel); clicking not exercised |
| Game menus on the panel (boot, title, file select) | verified unattended (title, file select dumps) |
| HUD split and HUD quad | verified unattended (text and counters absent from the eyes, present with correct alpha in the HUD image, composite) |
| 3D cinema with letterbox crop | verified unattended (layer FOV from the screen corners, picture rows between the bars, composites); depth not judged by eye |
| Pause when unfocused or in standby, quit while paused | verified unattended (game rate 0 Hz while the XR loop runs at 72; clean quit) |
| Rumble Pak to haptics | built; not exercised (no wearer) |
| Diagnostics (above) | verified unattended |
| VR settings menu (Menu + View: quick menu, full panel), chord resolution, live settings and `vr.toml` writes | host-tested (chord timing, rows and keys, Defaults, world size, save round trip); unattended: see section 9a's dumps; headset-unverified |
| Vignette (also during the smooth orbit), fade on flat/stereo/cinema switches (still a 0.2 s black cut), roomscale, seated mode, camera shake off | not started (the vignette is deferred: it needs its own radial image and an alpha ramp) |

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
| Menu + View, hold 0.5 s | both | the VR settings menu (section 9a), from anywhere; again to close | as above | as above |
| D-pad | left | N64 D-pad | none | none |
| Left stick | left | N64 stick, relative to where you look | left stick | left stick |
| Right stick X | right | snap turn 45 degrees, or the smooth orbit with `turn_mode = smooth` (`right_stick_x = camera`: the game's C-left/C-right) | right stick | right stick |
| Right stick Y | right | nothing in play (lesson 80); scrolls menus | right stick | right stick |
| Right stick click | right | N64 L (unused by the game) | none | right stick click |
| Left stick click, grips | | nothing | | |

In a recomp menu: D-pad or left stick navigate, A accepts, B goes back, Menu applies, the bumpers
switch tabs, the trigger clicks where the pointer is, View closes. The game's own menus (title, file
select, pause) take the N64 pad as usual.

The Menu + View chord (`dk64vr::SystemChord`, host-tested) is resolved before either button acts
alone (standard.md section 1). Menu down alone holds Start (or a recomp menu's Apply) back for
0.15 s: View joining in that time makes the chord, otherwise Start passes from then on, and a tap
shorter than that is sent as a 0.12 s Start after the release. View down alone goes to its own
state machine, which acts only on release (the recomp menu) or at 1 s (recentre); Menu joining
within 0.9 s makes the chord and cancels the View press without an event. From the moment both
are down nothing passes, whether the chord completes (0.5 s, a click on both hands) or one
button is let go early, until both are up. View held past 0.9 s and then Menu are two separate
actions (Start at once). Pausing is 0.15 s later than before; nothing else changed.

Turning: the game eases its camera to a new heading over a second or more. A snap writes the camera's
heading (`CameraPaad::unkB2`, through the `recomp_vr_take_turn` import in the camera patch,
`patches/sound_options_patches.c` `func_global_asm_8061D1FC`) and, until the camera arrives, the eye
view is orbited about the camera's look-at point by what it still has to turn
(`dk64vr::orbitViewBy`), so the wearer sees one instant step and the stick stays aligned. Headings are
unwrapped, so the view may run any distance ahead of the game's ease. The default is a 45 degree snap
(the user: 30 felt like the N64's small camera steps). `turn_mode = smooth` orbits the camera about
Donkey Kong while right stick X is held: 20% deadzone, the rest of the travel to the power 1.5
(`dk64vr::smoothTurnRate`), up to `smooth_turn_speed` (120 deg/s) at full deflection; each frame's
step goes to the game's heading the same way, and the orbit shows it at once, so the game's 4-6 s ease
never shows and the camera smoothing (applied before the orbit) adds no lag to it. Release stops it
at once. Turning applies in third-person play only, never in first person, the cinema, the panel or a
frozen world. When the game takes the camera over (C-left/C-right) or the turn times out (8 s after
the last input), what is left of the orbit unwinds over 0.4 s instead of jumping; a camera cut or a
mode change ends it.

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


## 9a. VR settings menu

The approved design (mockup layouts C then B): Menu + View held 0.5 s opens a quick menu of big
tiles, and its "All VR settings..." opens the full panel. Code: `src/vr/menu/dk64vr_menu.*` (the
model: rows, focus, pointer, Defaults, scrolling, and the RML it writes; pure, host-tested),
`src/vr/menu/dk64vr_menu_ui.*` (one recompui context on `assets/vr_menu.rml`, styled by
`assets/vr_menu.rcss`), `XrHost::openVrMenu`, `feedVrMenu`, `closeVrMenu`.

- **Quick menu (C)**, 1600 x 868 px centred on the panel: tiles Turning (Snap / Smooth; Off is in
  the full panel), Snap angle, World size, Camera smoothing, HUD distance, Foveation, each with its
  value and a line under the control (the reason when it is dimmed); Recentre now (the one blue
  button) in the title bar; All VR settings... and the hint bar at the bottom.
- **Full panel (B)**, 1808 x 1000 px: a rail with the standard's tabs in order (Comfort, Controls,
  HUD & menus, Display & performance, Room; no Accessibility & audio keys exist), per-tab Defaults
  (first press turns it red and names the tab, a second within 3 s resets the tab's keys), a "Game
  paused" chip, rows of label, description and control. Comfort has World size, Lower camera and
  Camera closer (the last two only when `camera_lower` and `camera_closer` are registered: rows
  whose key is missing hide themselves). Display & performance shows the measured display rate,
  read only, and says SteamVR's per-app setting decides it. Rows for registry keys that do nothing
  yet (vignette, roomscale, lean limit, height offset, seated) are shown dimmed with a "Coming"
  badge, as in the mockup. No diagnostics anywhere (host-tested).
- **World size** is Donkey Kong's height (25.2 game units, section 5): `world_scale` = 25.2 /
  height. The stepper walks 0.5, 0.8, 1.0, 1.2, 1.4, 1.6, 1.68 (default, "DK 1.7 m"), 1.8, 2.0,
  2.2, 2.5, 3, 3.5, 4, 5 and 6 m, limited to what the key's range allows (5 m with this build's
  floor of 5 units a metre), with a comparison ("About as tall as you" ... "Giant").
- **Input.** The pad: up/down move the focus (an orange pulsing ring, recompui's Secondary to
  SecondaryL), left/right change the value (held: repeats after 0.4 s every 0.09 s), A selects
  (segments cycle, toggles flip, buttons press), B or View goes back (B to C, C closes), the bumpers
  switch tabs, right stick Y scrolls. Dimmed rows, read-only rows and the controller map are skipped.
  The pointer: the dominant hand's aim ray, hit-tested by the menu against the laid-out document
  (`Rml::Context::GetElementAtPoint`, `data-act` attributes); hover lights the control (ui_hover
  haptic), the trigger clicks on press (ui_click), a slider drags anywhere on its 76 px strip, and
  the dot shrinks while the trigger is held. The pointer stays the last layer, its dot flat on the
  panel (lessons 41, 80, 83). Neither reaches recompui's other menus while this one is open.
- **Settings** change at once: the menu works on a copy and hands every change to the host, which
  sets it in its own settings (read every frame) and writes `vr.toml` atomically on a worker thread
  0.5 s after the last change and on close. Foveation levels and eye tracking change live when the
  shading-rate backend came up at launch; refresh rate and resolution apply at the next launch (the
  rows say so).
- **Pause.** While open the game is paused (the standby path: the game thread waits in its frame
  publish) and, in stereo, the recomp-menu path freezes the world, dimmed, behind the panel. The
  panel turns to face the head when the menu opens (yaw only; back on close and on recentre) and
  shows only the menu: RT64 draws the UI hook over transparent black (`VrHost::panelUiOnly`) and the
  panel layer is blended with premultiplied alpha (logged as `panelpm`). Over the launcher or a
  recomp menu the menu covers the panel with a dark backdrop instead. Closing resumes; the button
  that closed it is ignored until released (`InputMapper::ignoreHeld`).
- Sizes follow the mockup: 1 degree is about 30 px at 2 m, text 26 px or more, targets 76 px or more.

## 10. Wear checklist (about 15 minutes)

Build: branch `vr-polish`, installed as the `DK64VR` devkit title (binary md5
`37767855d220d91ccbd351bfb64b8cba`, no diagnostics armed; the device's `vr.toml` has
`resolution_scale = "auto"`, `snap_angle = 45`, foveation high eye-tracked, camera smoothing medium).
SteamVR per-app settings for DK64VR: 120 Hz (as the user set it). Launch from the Steam
library (not SSH). Claim the Frame as `user-wearing`. Read the log afterwards with
`tools/steam_frame/deploy.sh --logs` (FRAME_HOST=steamos@192.168.1.31).

1. Launcher on the panel (2 min): the DK64 Recompiled launcher appears on a panel 2 m ahead, with no
   black box in front of it (launch from the Steam library in the headset). Point at
   it with the right hand: is there a beam and a dot, flat on the panel, never hidden? Click Start Game
   with the trigger. Then try A and the D-pad instead of the pointer.
2. Title and file select (1 min): the intro and title play on the cinema screen (3D?), the file select
   on the panel. Pick a file with A.
3. Intro cutscene (2 min): cinema screen 3 m ahead. Does the depth sit behind the screen? Is the speech
   bubble text readable, in front of the picture, and do bars ever follow your head? Does the screen
   stay put when you turn your head?
4. DK Isles, first minutes (4 min): walk around. Is DK about 1.7 m tall (`world_scale`)? Does the left
   stick go where you look? Snap turn with the right stick: one clean 45 degree step, no swing
   afterwards? Try Y/B (the game's camera swing): does it start and stop gently now (camera
   smoothing)? Run in circles: does the world still jerk? Look at the palm trees near the start with
   your eyes, not your head: any pink rings? Jump (A), attack (X), crouch (LT), first person (RB) and
   back (the crosshair must not lag).
5. HUD (2 min): collect a banana or get hit so a counter shows. Is the HUD 1.5 m ahead, legible, free of
   roll when you tilt your head, in front of the world everywhere? Any HUD left in the world image?
6. Pause, menus and transitions (3 min): Menu pauses: the world stays, dimmed, with the pause menu on
   the panel in front, and no flicker to a black room. View (short) opens the recomp menu the same
   way: navigate with the stick, click with the trigger, close with View. Walk through a door to a
   new area: a fade to black and back, no flat panel in between.
6b. VR settings menu (3 min): in DK Isles, hold Menu and View together for half a second. The world
   freezes, dimmed, and the quick menu floats in front of you (no pause menu, no recentre, no
   recomp menu should appear). Change Turning to Smooth with the pointer and World size with the
   D-pad (right makes DK taller; the frozen world shows it after closing). Open All VR settings..., switch tabs with
   the bumpers, scroll Comfort with the right stick, try Defaults (it asks once). Close with B (or
   Menu + View again): the game carries on and the B press does not swing the camera. Is all text
   readable, and are the targets easy to hit?
7. Recentre and focus (1 min): turn your body 90 degrees and hold View 1 s: buzz, view recentred behind
   DK. Lift the headset off for a few seconds: on return the game should have waited.
8. Answer: scale, HUD distance and size, snap angle, cinema distance and depth, any button that does
   not do what its label says, any discomfort.

## 11. Known risks for the first wear

- No button press has ever reached the game from the controllers: the input path is host-tested and
  the profiles are accepted on the device, but an unattended run has no wearer. If nothing responds,
  the log's `[vr] input:` lines tell whether samples are live.
- Snap and smooth turns: the game camera takes 4-6 s to arrive; the view is held at the new heading
  meanwhile. If something else turns the camera during that time (C-left/C-right, the game's own
  heading changes), the rest unwinds over 0.4 s, a small swing.
- Pink rings around the palm trees at the start (second wear, foveation high fixed): not reproduced
  unattended. Gameplay dumps at the start position (foveation off: left eye; high: both eyes at two
  moments, and the HUD image) have no pink pixel at all (none of 4.7 million an eye), and 1:1 crops of the near and far palms in the
  coarse periphery look alike. The dark alpha-edge outlines around the palm crowns against the
  bright sky are the likeliest source of coloured fringes from the lenses' colour correction; to be
  checked worn with foveation off and high.
- The pause panel's picture is the game's snapshot of the left eye image, so it shows the
  hidden-area mask's black corners.
- The HUD is a 4:3 frame squeezed into 40 degrees; small text may be hard to read, and counters at the
  frame's edges sit 20 degrees off centre.
- Cutscene depth is computed, not seen: `cinema_depth` 0.6 may be too strong or too weak.
- Steam's Stop (SIGTERM) does not end a started game: SDL turns it into the recomp quit prompt. Quit
  from SteamVR's own menu instead (that path is clean, also while paused).
- First person: the crosshair follows the game camera's aim, but the head still looks around freely;
  untested.
- Frame time: 120.0 fps at 120 Hz and 2160² with foveation high in DK Isles (0-2 missed a 10 s
  window); busier levels are unmeasured.
- The hidden-area mask blackens the eye image's periphery; a game effect that later reads the main
  colour image (a pause snapshot) sees those corners black.

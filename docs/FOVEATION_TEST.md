# Foveation device A/B (coordinator handoff)

Device-unverified. Backend: app-owned `VK_KHR_fragment_shading_rate`, ordinary RT64 eye images;
no subsampled images or FB foveation profiles. Both settings default off. The worker did not contact
the Frame. Install the locally built package, verify its hashes, and book the Frame before running
`tools/steam_frame/unattended.sh`. Keep the same save, stationary DK Isles camera and per-app rate
for every comparison. Do not compare a 72 Hz run with a 90/120 Hz run.

Based on superproject `15f4830`, RT64 `aab09dc`, plume `716076c`. The coordinator measured about
5.8 ms GPU per frame (scene passes plus eye-output draw) at 2160² and 72 Hz, with no missed frames
in steady DK Isles gameplay; intro-to-gameplay transitions still need to be excluded. This A/B
now measures 90/120 Hz headroom and peripheral quality. Re-establish the off baseline in this
combined build: the earlier measurements did not establish the Valve layer's injection state.
Schema 5 makes the eye size an absolute 2160² (`resolution_scale = auto`): with SteamVR's per-app resolution at 2160 the old 1.25 rendered 2700². Schema 5 also makes foveation high, eye-tracked, the default (the A/B below sets every key explicitly).

The hidden-area mask and scene foveation both apply to the eye. The mask draws first at 1x1 into
the same normal colour/depth targets, and the subsequent scene selects the rate attachment.
Clear/load-op and read-only-depth pass variants carry that attachment too. Keep the performance
switches below identical for every comparison; leave the experimental point-sampling, same-eye,
target and eye-order diagnostics unset.

## Exact one-shot files

Every run's `vr-diag` is this common block followed by **one row's four lines** from the table.
All lines are env assignments, without TOML quotes. The launcher consumes the file once. Overrides
are not saved in `vr.toml`.

`DISABLE_VULKAN_FDM_INJECTION_LAYER=1` disables injection. Explicit `=0` is translated by the
host to an **unset** variable before OpenXR/Vulkan layer loading, so presence-based loaders also
allow the layer. Omitting the line defaults to disabled in this VR build.

```text
ARGS=--game dk64
DK64_VR_FORCE_RENDER=1
DK64_VR_DIAG_AUTOSTART=90
DK64_VR_DIAG_EXIT_SECONDS=150
DK64_VR_REFRESH_RATE=90
DK64_VR_TIMING=1
DK64_VR_DUMP_FRAME=600
DK64_VR_DUMP_COUNT=3
DK64_VR_DUMP_INTERVAL=1800
DK64_VR_DIAG_YAW=0
DK64_VR_DIAG_YAW_STEP=0
DK64_VR_DIAG_HUD_SECONDS=5
DK64_VR_DUMP_FOVEATION=1
DK64_VR_DUMP_PANEL_FRAME=0
RT64_VR_HIDDEN_MASK=1
RT64_VR_DEFER_CLEARS=1
RT64_VR_RO_DEPTH_LOAD=1
RT64_VR_DEFER_WAIT=1
RT64_VR_EYE_COPY=draw
FDM_DEBUG=1
```

| Run | Exact appended lines |
| --- | --- |
| Off, 1728 (optional size control) | `DK64_VR_RESOLUTION_SCALE=1.0`<br>`DK64_VR_FOVEATION=off`<br>`DK64_VR_FOVEATION_EYE_TRACKED=0`<br>`DISABLE_VULKAN_FDM_INJECTION_LAYER=1` |
| Off, 2160 | `DK64_VR_RESOLUTION_SCALE=auto`<br>`DK64_VR_FOVEATION=off`<br>`DK64_VR_FOVEATION_EYE_TRACKED=0`<br>`DISABLE_VULKAN_FDM_INJECTION_LAYER=1` |
| Low, 2160 fixed | `DK64_VR_RESOLUTION_SCALE=auto`<br>`DK64_VR_FOVEATION=low`<br>`DK64_VR_FOVEATION_EYE_TRACKED=0`<br>`DISABLE_VULKAN_FDM_INJECTION_LAYER=1` |
| Medium, 2160 fixed | `DK64_VR_RESOLUTION_SCALE=auto`<br>`DK64_VR_FOVEATION=medium`<br>`DK64_VR_FOVEATION_EYE_TRACKED=0`<br>`DISABLE_VULKAN_FDM_INJECTION_LAYER=1` |
| High, 2160 fixed | `DK64_VR_RESOLUTION_SCALE=auto`<br>`DK64_VR_FOVEATION=high`<br>`DK64_VR_FOVEATION_EYE_TRACKED=0`<br>`DISABLE_VULKAN_FDM_INJECTION_LAYER=1` |
| Low, 2160 gaze | `DK64_VR_RESOLUTION_SCALE=auto`<br>`DK64_VR_FOVEATION=low`<br>`DK64_VR_FOVEATION_EYE_TRACKED=1`<br>`DISABLE_VULKAN_FDM_INJECTION_LAYER=1` |
| Medium, 2160 gaze | `DK64_VR_RESOLUTION_SCALE=auto`<br>`DK64_VR_FOVEATION=medium`<br>`DK64_VR_FOVEATION_EYE_TRACKED=1`<br>`DISABLE_VULKAN_FDM_INJECTION_LAYER=1` |
| High, 2160 gaze | `DK64_VR_RESOLUTION_SCALE=auto`<br>`DK64_VR_FOVEATION=high`<br>`DK64_VR_FOVEATION_EYE_TRACKED=1`<br>`DISABLE_VULKAN_FDM_INJECTION_LAYER=1` |
| Valve allowed, 2160 | `DK64_VR_RESOLUTION_SCALE=auto`<br>`DK64_VR_FOVEATION=off`<br>`DK64_VR_FOVEATION_EYE_TRACKED=0`<br>`DISABLE_VULKAN_FDM_INJECTION_LAYER=0` |
| Layer conflict guard, 2160 | `DK64_VR_RESOLUTION_SCALE=auto`<br>`DK64_VR_FOVEATION=high`<br>`DK64_VR_FOVEATION_EYE_TRACKED=1`<br>`DISABLE_VULKAN_FDM_INJECTION_LAYER=0` |

The last run must say `backend=off reason=Valve-layer-selected`: Vulkan forbids enabling FDM and
KHR attachment shading rate together. It tests the guard, not our high preset's performance. The
Valve-allowed baseline investigates the layer's default behaviour without asking for FB profiles.
The app cannot introspect injected attachments: `Valve_injected=unknown-see-FDM_DEBUG` is deliberate.
Only layer logs can establish injection. `already_has_fdm`, a detected matching size, or a loaded
library alone is not proof that an eye pass was foveated.

Equivalent executable command (the script writes precisely those assignments):

```bash
tools/steam_frame/unattended.sh 160 results/medium-2160-fixed-r1 \
  force_render=1 diag_autostart=90 diag_exit_seconds=150 refresh_rate=90 timing=1 \
  dump_frame=600 dump_count=3 dump_interval=1800 diag_yaw=0 diag_yaw_step=0 \
  diag_hud_seconds=5 dump_foveation=1 dump_panel_frame=0 \
  RT64_VR_HIDDEN_MASK=1 RT64_VR_DEFER_CLEARS=1 RT64_VR_RO_DEPTH_LOAD=1 \
  RT64_VR_DEFER_WAIT=1 RT64_VR_EYE_COPY=draw \
  resolution_scale=1.25 foveation=medium foveation_eye_tracked=0 \
  FDM_DEBUG=1 DISABLE_VULKAN_FDM_INJECTION_LAYER=1
```

Repeat each performance row three times, alternating off-2160 between levels. Use distinct output
directories. Discard the first two timing windows after gameplay starts; compare complete windows
in the same `mode=stereo` scene. Report the spread of GPU ms, fps and `missed`, not a single run.
Use the measured `predictedDisplayPeriod` / `[vr-perf] rate`, not the offered FB refresh list. Set
SteamVR's DK64VR per-app rate to 90 for the target trial; then repeat off-2160 and the accepted
quality presets at 120 if they hold 90. Replace `DK64_VR_REFRESH_RATE=90` with `=120` in the common
block too. Changing only the env request does not prove SteamVR changed its actual per-app rate.

## Evidence and interpretation

Collect `run-stdout.txt`, every `vr-dumps/*.png`, package `BUILD-METADATA.json` and the installed
binary hash. `unattended.sh` fetches the log and dumps; keep any layer/runtime journal output too
if `FDM_DEBUG` is not present in stdout. Expected markers:

* `[vr] diag`: exact overrides; `[openxr] ... using 2160x2160`, frame period, clean exit.
* `[vr] hidden-area mesh, eye 0/1`: both meshes present with the mask enabled; black lens-hidden
  corners should match the off baseline. `[vr-perf] eyes`: scene/copy GPU ms, max, record, submit,
  wait, pass count and depth switches alongside `foveated_passes`. Deferred right-eye wait stays
  zero; its previous submission is completed before map/image reuse in the next scene.
* `[vr-fov] backend=khr_shading_rate`, enabled extension/features, supported rate mask/tile,
  `profile=app-owned fdm=none Valve_layer=disabled Valve_injected=no`.
* One `[vr-fov] eye=0/1 pass=main-raster` line per new target extent: allocated target (may be
  padded), visible eye extent, R8_UINT map size and asymmetric centre. No main-raster attachment
  creation for cinema or panels. `foveated_passes` is actual render-pass starts with our attachment,
  per ten-second window, not a count of eye replays. A single eye can start several passes.
* `centre_source=eye-gaze` when live tracking is valid; `fixed-projection` otherwise. Unattended
  synthetic/standby runs should fall back to fixed. Those runs cannot validate live eye tracking.
* `vr-fov-eye{0,1}-N.png`: actual uploaded rate-map bytes visualised, one pixel per rate texel;
  white 1x1, green up to 2x2, orange coarser. Off/cinema shows an all-white, unattached reference.
  These are CPU mirrors of the submitted map, not GPU readbacks or Valve's injected map.
* `vr-eye*`, `vr-hud*` and `[vr] layers`: rebuild the composite with `tools/steam_frame/composite.py`
  (read its `--help`). Check both eye edges, texture filtering, fog/far-clip contours, decals and
  pixel noise. HUD text/crosshair must stay as sharp as baseline; compare the HUD dump separately.

A map with colours and nonzero passes proves attachment selection/recording, not reduced GPU cost.
Lower GPU ms at the same scene/rate/eye size, sustained fps and fewer misses across repeats indicate
a benefit. Zero passes means off, unsupported, layer-selected, cinema/panel or a failure: inspect
`reason=` before treating it as a performance result. Exactly half-rate fps can be compositor
reprojection; repeat and compare GPU time/misses. Little saving with nonzero passes can mean geometry,
bandwidth, compositor cost, or pixel-exact exceptions dominate. Never raise the default on this basis
without device visuals and a worn check.

Before accepting a preset, check startup and every decal/depth switch for Vulkan errors with
deferred clears enabled. In one high-fixed diagnostic run, add `RT64_VR_PASS_TIMES=1` and collect
`[vr-pass]` along with the ordinary logs. If a Vulkan validation layer is installed, a separate
validation run can check clear-variant/framebuffer compatibility; keep it out of timing comparisons.
A clear/load-op variant must retain the rate attachment. Compare the hidden-area contour, far clip,
three-point filtering and speech-bubble scissor with foveation off; HUD/panel/cinema remain sharp.

After the performance runs, use the high-fixed row with `DK64_VR_DIAG_START_AT=45`,
`DK64_VR_DIAG_MENU_AT=75`, `DK64_VR_DUMP_PANEL_FRAME=120`; do a cutscene/cinema run from the same
save route. In windows entirely in panel/cinema, `foveated_passes=0`; inspect sharp menus, cinema
subtitles and HUD, and ensure no stale map is applied after a mode change. A worn gaze check should
look left/right/up/down, then lose tracking: the white region follows gaze and immediately returns
to the projection centre when invalid. This is a separate acceptance step; the unattended A/B does
not establish gaze latency or comfort.

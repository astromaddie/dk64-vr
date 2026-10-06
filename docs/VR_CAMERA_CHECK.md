# Larger world / lower, closer camera: coordinator device check

Status: host-tested and headset-unverified. The camera worker did not contact the device.
After the coordinator integrates/builds this commit and the settings-menu worker's commit,
use the normal booking/install/Steam-launch workflow. Keep the user's saves/config; env overrides
below last only for this one run. Use a save that resumes in DK Isles and confirm the dump's
state line says `scene gameplay`; autostart alone does not guarantee a particular map.

Put these lines in the app folder's one-shot `vr-diag`:

```text
ARGS=--game dk64
DK64_VR_FORCE_RENDER=1
DK64_VR_WORLD_SCALE=8
DK64_VR_CAMERA_LOWER=0.5
DK64_VR_CAMERA_CLOSER=0.3
DK64_VR_EYE_NEAR_M=0.1
DK64_VR_CAMERA_SMOOTHING=medium
DK64_VR_LEVEL_PITCH=1
DK64_VR_DIAG_AUTOSTART=90
DK64_VR_DIAG_EXIT_SECONDS=120
DK64_VR_DIAG_CAMERA_LOG=1
DK64_VR_TIMING=1
DK64_VR_DUMP_FRAME=600
DK64_VR_DUMP_COUNT=3
DK64_VR_DUMP_INTERVAL=300
DK64_VR_DIAG_YAW=0
DK64_VR_DIAG_YAW_STEP=90
```

Retrieve `run-stdout.txt` and both eyes for all three dumps:
`vr-eye{0,1}-000-yaw0.png`, `-001-yaw90.png`, `-002-yaw180.png` in `vr-dumps/`.
Take the associated HUD dumps and layer logs too. Dump frames count stereo, including cinema;
if a dump precedes gameplay, rerun with a later `DUMP_FRAME` rather than treating it as DK Isles
camera evidence. Scale 8 now has a 0.8-unit / 0.10 m eye near; `eye_near_m=0` restores 10 units / 1.25 m.

Look for:

- One settings line: `[vr-cam] world_scale 8.0 (DK height ≈ 3.15 m) lower 0.50 closer 0.30 eye_near_m 0.100`.
  The derived menu label must match, and reopening settings must retain the saved choices;
  these env overrides must not replace the values in `vr.toml`.
- Upright, distinct eyes with correct parallax at all three yaws; DK/world larger than scale 15.
  With the head level, the origin's height gap is 35% of the smoothed original (0.7*0.5), its
  horizontal distance 70%. `[vr-cam] origin` logs give focus/adjusted positions; no origin lines
  should appear for first person, cinema or panels.
- `[vr-cam] depth` reports near 0.8 units / 0.100 m, the original far, positive `sky_margin`, and
  non-identity fog coefficients. Cinema keeps its game's near and should not emit this immersive line.
- No unexpected fog wall, changed HUD size/distance, lens-mask border, missing sky, or new
  clipping holes. Check near geometry and DK for the remaining origin/collision limitations.

Repeat those captures with **world scale 4, camera_closer 0.8, camera_lower 0.5**. The eye near must
be 0.4 units / 0.100 m. DK and scenery more than 10 cm from the eye should retain complete surfaces;
the old 2.5 m slice must be gone. Inspect both eyes at all yaws for sky holes/flicker, distant terrain
loss, decal shadows bleeding or disappearing, and hidden-mask borders. Also test first-person
weapon/hand/body/billboards and pause/cinema transitions: only immersive eyes use the physical near.

For a fog/depth A/B, repeat each scale at the same saved location, origin settings and synthetic yaw,
changing only `DK64_VR_EYE_NEAR_M=0`. Fog opacity/colour on geometry visible in both runs must match
at near/mid/far distances; sky and distant geometry should remain intact. The fallback log reports
the game's near (10 units), identity fog `(1,0)` and the same source far. The closer default run
intentionally reveals geometry hidden by that old near slice. Compare decal attachment and overlap
ordering, not just scene colour. Repeat with foveation off if an artifact depends on shading rate.
Static dumps cannot accept temporal sky flicker: retain camera logging during the motion/orbit runs
below and watch a live run for far-plane crossings, disappearing sky bands and unstable shadows.

For a second unattended camera-motion pass, retain scale/lower/closer and timing, set
`DIAG_YAW_STEP=0`, `DUMP_COUNT=8`, `DUMP_INTERVAL=120`, and add:

```text
DK64_VR_DIAG_CAMERA_SECONDS=3
```

Confirm focus/adjusted positions progress smoothly during C-button swings and movement.
For orbit checks, disable `DIAG_CAMERA_SECONDS` (avoid conflicting C-button inputs) and add:

```text
DK64_VR_DIAG_TURN_STICK=1
DK64_VR_TURN_MODE=snap
```

Each snap should orbit DK rather than the former longer-radius pivot. Repeat with `TURN_MODE=smooth`:
`[vr-cam] shown:` should track the requested turn rate without a new 30 Hz radius step. Cuts and
mode transitions must reset immediately, with no swoop. A wearer must separately check recentre,
first-person entry/exit, pause/cinema return, perceived size, clipping and comfort; unattended
captures do not accept those behaviours.

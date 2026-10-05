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
camera evidence. At the game's 10-unit near, scale 8 has a 1.25 m near plane.

Look for:

- One settings line: `[vr-cam] world_scale 8.0 (DK height ≈ 3.15 m) lower 0.50 closer 0.30`.
  The derived menu label must match, and reopening settings must retain the saved choices;
  these env overrides must not replace the values in `vr.toml`.
- Upright, distinct eyes with correct parallax at all three yaws; DK/world larger than scale 15.
  With the head level, the origin's height gap is 35% of the smoothed original (0.7*0.5), its
  horizontal distance 70%. `[vr-cam] origin` logs give focus/adjusted positions; no origin lines
  should appear for first person, cinema or panels.
- No unexpected fog wall, changed HUD size/distance, lens-mask border, missing sky, or new
  clipping holes. Check near geometry and DK for the known clipping/collision limitations.

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

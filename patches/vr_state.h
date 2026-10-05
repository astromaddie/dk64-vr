// Game-side half of the VR game-state interface (see src/vr/include/dk64vr_game_state.h, which
// is frozen and owns the contract). The patches fill a VrGameState once per game frame and hand
// it to the host through recomp_vr_publish_game_state; the host copies it into a seqlock that
// the renderer reads. recomp_vr_is_active mirrors dk64vr_is_active().
//
// With the renderer inactive every gate in the patches reads 0 and the game runs as before.
#ifndef __VR_STATE_H__
#define __VR_STATE_H__

#include "patch_helpers.h"

// Same values as enum dk64vr_scene in the frozen header. src/game/vr_api.cpp static_asserts them.
#define VR_SCENE_UNKNOWN      0
#define VR_SCENE_GAMEPLAY     1
#define VR_SCENE_FIRST_PERSON 2
#define VR_SCENE_CUTSCENE     3
#define VR_SCENE_TRANSITION   4
#define VR_SCENE_MENU         5
#define VR_SCENE_PAUSE        6
#define VR_SCENE_ARCADE       7
#define VR_SCENE_MULTIPLAYER  8
#define VR_SCENE_COUNT        9

#ifdef MIPS
// Layout of dk64vr_game_state: nineteen 32-bit words, no padding. The host reads it straight out
// of RDRAM (words are host-endian there), so keep it in step with the frozen header.
typedef struct VrGameState {
    u32 frame;
    s32 scene;
    f32 cam_eye[3];
    f32 cam_at[3];
    f32 cam_up[3];
    f32 fov_y_deg;
    f32 near_z;
    f32 far_z;
    f32 player_pos[3];
    f32 player_yaw_rad;
    s32 sky_dome;
} VrGameState;
#endif

DECLARE_FUNC(s32, recomp_vr_is_active);
DECLARE_FUNC(void, recomp_vr_publish_game_state, const struct VrGameState*);

#ifdef MIPS
// vr_scene.c
void vr_frame_hook(void);
extern u8 vr_sky_dome_drawn;

// vr_sky.c. colors are 0xRRGGBBAA, stops are the Y of each colour stop in the flat sky's own
// vertex units. model is the double-buffered scratch matrix the flat sky would have used.
Gfx *vr_sky_dome(Gfx *dl, Mtx *model, const u32 *colors, const s16 *stops, f32 horizon_shift);
Gfx *vr_sky_dome_solid(Gfx *dl, Mtx *model, u32 color);
Gfx *vr_sky_dome_textured(Gfx *dl, Mtx *model, const u16 *texture, u8 tint_r, u8 tint_g, u8 tint_b);
#endif

#endif

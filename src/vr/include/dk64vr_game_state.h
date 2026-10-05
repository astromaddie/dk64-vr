// Interface between the DK64 game patches and the VR renderer (frozen; change only by agreement).
//
// The game side (patches/vr_*.c, through host imports in src/game/vr_api.cpp) publishes one
// snapshot per game frame. The renderer side (RT64 frame driver, compositor, input) reads the
// latest snapshot from any thread. Scene changes can reach the renderer a frame early or late
// relative to the workload being drawn, so consumers latch scene modes with hysteresis.
#ifndef DK64VR_GAME_STATE_H
#define DK64VR_GAME_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dk64vr_scene {
    DK64VR_SCENE_UNKNOWN = 0,
    DK64VR_SCENE_GAMEPLAY,      // 3D play with the chase camera
    DK64VR_SCENE_FIRST_PERSON,  // C-up look, coconut/peanut aim, sniper scope
    DK64VR_SCENE_CUTSCENE,      // scripted camera (is_cutscene_active)
    DK64VR_SCENE_TRANSITION,    // map transitions, zipper, framebuffer snapshot effects
    DK64VR_SCENE_MENU,          // boot logos, title, main menu, file select, options
    DK64VR_SCENE_PAUSE,         // in-game pause menu
    DK64VR_SCENE_ARCADE,        // Jetpac and the arcade cabinet (2D)
    DK64VR_SCENE_MULTIPLAYER,   // split screen
    DK64VR_SCENE_COUNT
} dk64vr_scene;

typedef struct dk64vr_game_state {
    uint32_t frame;           // game frame counter, increments once per published snapshot
    int32_t scene;            // dk64vr_scene
    float cam_eye[3];         // game camera position, game units, world space
    float cam_at[3];          // game camera look-at target
    float cam_up[3];          // game camera up vector
    float fov_y_deg;          // guPerspective fov
    float near_z;             // guPerspective near, game units
    float far_z;              // guPerspective far, game units
    float player_pos[3];      // player actor position, game units
    float player_yaw_rad;     // player facing, radians, game convention
    int32_t sky_dome;         // nonzero when the patches drew the VR sky dome this frame
} dk64vr_game_state;

// Latest complete snapshot (copied out under a seqlock, never torn). Valid from any thread.
void dk64vr_get_game_state(dk64vr_game_state* out);

// Written by the game side once per game frame.
void dk64vr_publish_game_state(const dk64vr_game_state* state);

// Nonzero while the VR renderer is active (stereo path on). The patches query it to switch
// camera culling off and draw the sky dome. Always 0 in a build without DK64_VR.
int dk64vr_is_active(void);
void dk64vr_set_active(int active);

#ifdef __cplusplus
}
#endif

#endif

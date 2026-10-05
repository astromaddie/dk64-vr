#include "common_structs.h"
#include "vr_state.h"

// Scene reporting for the VR renderer. vr_frame_hook runs once per game frame (it is called from
// the calculateLagBoost patch in patches_main.c, which the main loop reaches exactly once per
// iteration, after the frame's updates and display lists are done). It classifies what the game
// is showing, snapshots the camera and the player, publishes the lot, and, while the VR renderer
// is active, logs scene changes.

extern Maps current_map;
extern u8 game_mode;
extern u8 game_mode_copy;
extern u8 is_cutscene_active;
extern u8 cc_number_of_players;
extern u32 global_properties_bitfield;
extern f32 loading_zone_transition_speed;
extern f32 D_global_asm_807FD888;
extern s8 D_global_asm_807F5D84;
extern s8 D_global_asm_807F5D85;

// Set by vr_sky_dome (and friends) while the display list is built; published and cleared by the hook.
u8 vr_sky_dome_drawn = 0;

static VrGameState vr_state;
static s32 vr_scene_logged = VR_SCENE_UNKNOWN;
static u32 vr_frame_count = 0;

static const char *vr_scene_names[VR_SCENE_COUNT] = {
    "unknown", "gameplay", "first-person", "cutscene", "transition",
    "menu", "pause", "arcade", "multiplayer",
};

// Player control_state values (Actor.control_state, offset 0x154) that put the camera in a
// first-person view. Derived from the decomp: the action table at 0x80752C70 sends actions
// 5/7 to states 2/3 (C-up look and weapon aim, land and water), actions 6/8 to states 4/5 (the
// fairy camera), and the camera's shutter (func_global_asm_806E9BA8) uses 0x64/0x65.
static u8 vr_is_first_person_state(u8 state) {
    switch (state) {
        case 2:    // first person, land
        case 3:    // first person, underwater
        case 4:    // fairy camera, land
        case 5:    // fairy camera, underwater
        case 0x64: // fairy camera shutter, land
        case 0x65: // fairy camera shutter, underwater
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

// Maps that are a scripted show rather than play: the story intro, the DK rap, the endings, the
// level intros and game over screen.
static u8 vr_map_is_cinematic(void) {
    switch (current_map) {
        case MAP_DK_RAP:
        case MAP_HELM_INTRO_STORY:
        case MAP_ROCK_INTRO_STORY:
        case MAP_HELM_LEVEL_INTROS_GAME_OVER:
        case MAP_TRAINING_GROUNDS_END_SEQUENCE:
        case MAP_KLUMSY_ENDING:
        case MAP_BLOOPERS_ENDING:
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

static s32 vr_classify_scene(void) {
    Actor *player;

    // A fade, a zipper or a framebuffer snapshot effect is on the screen, or the map is loading
    // (the fade progress sits at 31 while the screen is black). The pause menu's blurred
    // backdrop also uses the snapshot machinery (effect 7) but counts as pause below.
    if ((loading_zone_transition_speed != 0.0f)
        || (D_global_asm_807FD888 != 0.0f)
        || (is_cutscene_active == 6)
        || (is_cutscene_active == 5)
        || ((D_global_asm_807F5D84 > 0) && (D_global_asm_807F5D85 != 7))) {
        return VR_SCENE_TRANSITION;
    }
    // Arcade cabinet (is_cutscene_active 3) and Jetpac (4): flat 2D games.
    if ((is_cutscene_active == 3) || (is_cutscene_active == 4)
        || (current_map == MAP_DK_ARCADE) || (current_map == MAP_JETPAC)) {
        return VR_SCENE_ARCADE;
    }
    // Boot logos, title screen, main menu with file select and options. The DK Theatre's
    // selection screen is a menu until a cutscene starts playing.
    if ((current_map == MAP_NINTENDO_LOGO)
        || (current_map == MAP_MAIN_MENU)
        || (current_map == MAP_TITLE_SCREEN_NOT_FOR_RESALE_VERSION)
        || (game_mode == GAME_MODE_NINTENDO_LOGO)
        || (game_mode == GAME_MODE_MAIN_MENU)
        || ((game_mode == GAME_MODE_DK_THEATRE) && (is_cutscene_active == 0))) {
        return VR_SCENE_MENU;
    }
    // In-game pause menu (global_properties_bitfield bit 1 is the game's pause flag).
    if (global_properties_bitfield & 2) {
        return VR_SCENE_PAUSE;
    }
    if (cc_number_of_players > 1) {
        return VR_SCENE_MULTIPLAYER;
    }
    // Scripted camera: is_cutscene_active 1 is the cutscene system, 2 the story intro (see
    // the isIntroStoryPlaying check in func_global_asm_805FBC5C's callers), 7 is only ever
    // compared against and treated like a cutscene. DKTV is the attract mode demo.
    if ((is_cutscene_active == 1) || (is_cutscene_active == 2) || (is_cutscene_active == 7)
        || vr_map_is_cinematic()
        || (game_mode == GAME_MODE_DK_TV)
        || (game_mode == GAME_MODE_OPENING_CUTSCENE)
        || (game_mode == GAME_MODE_END_SEQUENCE)
        || (game_mode == GAME_MODE_END_SEQUENCE_DK_THEATRE)
        || (game_mode == GAME_MODE_DK_THEATRE)) {
        return VR_SCENE_CUTSCENE;
    }
    if (character_change_array != NULL) {
        player = character_change_array[0].playerPointer;
        if ((player != NULL) && vr_is_first_person_state(player->control_state)) {
            return VR_SCENE_FIRST_PERSON;
        }
    }
    return VR_SCENE_GAMEPLAY;
}

static void vr_fill_camera(VrGameState *s) {
    CharacterChange *cc;
    Actor *player;
    f32 offset;
    f32 dx;
    f32 dz;

    cc = &character_change_array[0];
    // The game nudges the eye by one unit in x and z when it sits right above or below its
    // target, so that guLookAt has a horizontal direction (func_global_asm_805FCA94).
    offset = 0.0f;
    dx = cc->look_at_eye_x - cc->look_at_at_x;
    dz = cc->look_at_eye_z - cc->look_at_at_z;
    if (dx < 0.0f) {
        dx = -dx;
    }
    if (dz < 0.0f) {
        dz = -dz;
    }
    if ((dx < 0.5f) && (dz < 0.5f)) {
        offset = 1.0f;
    }
    s->cam_eye[0] = cc->look_at_eye_x + offset;
    s->cam_eye[1] = cc->look_at_eye_y;
    s->cam_eye[2] = cc->look_at_eye_z + offset;
    s->cam_at[0] = cc->look_at_at_x;
    s->cam_at[1] = cc->look_at_at_y;
    s->cam_at[2] = cc->look_at_at_z;
    s->cam_up[0] = cc->look_at_up_x;
    s->cam_up[1] = cc->look_at_up_y;
    s->cam_up[2] = cc->look_at_up_z;
    s->fov_y_deg = cc->fov_y;
    s->near_z = cc->near;
    s->far_z = cc->far;

    player = cc->playerPointer;
    if (player != NULL) {
        s->player_pos[0] = player->x_position;
        s->player_pos[1] = player->y_position;
        s->player_pos[2] = player->z_position;
        // y_rotation is 0..4095 for a full turn; 2*pi / 4096.
        s->player_yaw_rad = (f32)((u16)player->y_rotation & 0xFFF) * 0.0015339808f;
    } else {
        s->player_pos[0] = 0.0f;
        s->player_pos[1] = 0.0f;
        s->player_pos[2] = 0.0f;
        s->player_yaw_rad = 0.0f;
    }
}

// The game's "show this HUD counter" (hud.c): item, freeze, delay.
void func_global_asm_806F8BC4(s32 HUDItemIndex, u8 arg1, s32 arg2);

void vr_frame_hook(void) {
    s32 scene;
    s32 active;
    s32 item;

    scene = vr_classify_scene();
    // Diagnostic (diag_hud_seconds): the counters on screen for an unattended HUD dump.
    if (recomp_vr_take_hud_diag() && (scene == VR_SCENE_GAMEPLAY)) {
        for (item = 0; item < 8; item++) {
            func_global_asm_806F8BC4(item, 0, 0);
        }
        recomp_printf("[vr] diag: HUD counters shown\n");
    }
    vr_state.frame = ++vr_frame_count;
    vr_state.scene = scene;
    if (character_change_array != NULL) {
        vr_fill_camera(&vr_state);
    }
    vr_state.sky_dome = vr_sky_dome_drawn;
    vr_sky_dome_drawn = 0;
    recomp_vr_publish_game_state(&vr_state);

    active = recomp_vr_is_active();
    if (active && (scene != vr_scene_logged)) {
        recomp_printf("[vr-scene] %s -> %s\n", vr_scene_names[vr_scene_logged], vr_scene_names[scene]);
        vr_scene_logged = scene;
    } else if (!active) {
        vr_scene_logged = VR_SCENE_UNKNOWN;
    }
}

#include "common_structs.h"
#include "vr_state.h"

// VR sky dome. The game's gradient sky is a screen-space ortho strip that slides with the
// camera pitch (func_global_asm_80704B20), so in a headset it would turn with the head. While
// the VR renderer is active the sky functions call vr_sky_dome instead: an elevation-banded
// sphere centred on the game camera, drawn through the same camera projection as the world,
// with no depth test or write, first in the frame. The renderer keeps head rotation out of
// the game camera, so the dome stays put in the world.
//
// How the flat sky maps to elevation. The strip's vertices are y = 0..960 in its own units; the
// flat code puts vertex y on screen row y - 336 at level pitch (temp_f2 in 80704B20), and the
// screen centre is row 120 with 240 rows spanning a 45 degree vertical fov, 290 rows per unit
// of tan. So a vertex at y sits at tan(elevation) = (456 - y) / 290 for a level camera, and the
// colour stops keep their order top to bottom. The dome reproduces exactly that: rings at the
// stop heights (and three in the long upper band), colour interpolated in y like the RDP did.

extern u8 D_global_asm_807444FC;

#define VR_SKY_ROWS_PER_TAN 290.0f
#define VR_SKY_LEVEL_ROW 456.0f
// Added to every vertex y before it becomes an elevation; tune the horizon height here.
#define VR_SKY_HORIZON_BIAS 0.0f
// The dome sits this far out, as a fraction of the camera's far plane.
#define VR_SKY_RADIUS_FRACTION 0.9f
#define VR_SKY_UNIT 1000.0f // vertex coordinates are s16; the matrix scales them up

#define VR_DOME_SEGS 16
#define VR_DOME_RINGS 7
// Layout: [top pole][ring 0][ring 1] ... [ring 6][bottom pole]
#define VR_DOME_VERTS (1 + VR_DOME_SEGS * VR_DOME_RINGS + 1)

static Vtx vr_dome_vtx[2][VR_DOME_VERTS];

static const f32 vr_seg_cos[VR_DOME_SEGS] = {
    1.0f, 0.92387953f, 0.70710678f, 0.38268343f,
    0.0f, -0.38268343f, -0.70710678f, -0.92387953f,
    -1.0f, -0.92387953f, -0.70710678f, -0.38268343f,
    0.0f, 0.38268343f, 0.70710678f, 0.92387953f,
};
static const f32 vr_seg_sin[VR_DOME_SEGS] = {
    0.0f, 0.38268343f, 0.70710678f, 0.92387953f,
    1.0f, 0.92387953f, 0.70710678f, 0.38268343f,
    0.0f, -0.38268343f, -0.70710678f, -0.92387953f,
    -1.0f, -0.92387953f, -0.70710678f, -0.38268343f,
};

extern u8 skip_interpolation;
extern u8 skip_persp_interp;

// Colour of the stop gradient at height y, written as r, g, b.
static void vr_stop_color(const u32 *colors, const s16 *stops, f32 y, f32 *out) {
    s32 i;
    f32 span;
    f32 t;
    u32 a;
    u32 b;

    i = 0;
    while ((i < 2) && (y > (f32)stops[i + 1])) {
        i++;
    }
    span = (f32)(stops[i + 1] - stops[i]);
    t = 0.0f;
    if (span > 0.0f) {
        t = (y - (f32)stops[i]) / span;
    }
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    a = colors[i];
    b = colors[i + 1];
    out[0] = (f32)((a >> 24) & 0xFF) + t * ((f32)((b >> 24) & 0xFF) - (f32)((a >> 24) & 0xFF));
    out[1] = (f32)((a >> 16) & 0xFF) + t * ((f32)((b >> 16) & 0xFF) - (f32)((a >> 16) & 0xFF));
    out[2] = (f32)((a >> 8) & 0xFF) + t * ((f32)((b >> 8) & 0xFF) - (f32)((a >> 8) & 0xFF));
}

static void vr_set_vtx(Vtx *v, f32 x, f32 y, f32 z, const f32 *rgb) {
    v->v.ob[0] = (s16)x;
    v->v.ob[1] = (s16)y;
    v->v.ob[2] = (s16)z;
    v->v.flag = 0;
    v->v.tc[0] = 0;
    v->v.tc[1] = 0;
    v->v.cn[0] = (u8)(rgb[0] + 0.5f);
    v->v.cn[1] = (u8)(rgb[1] + 0.5f);
    v->v.cn[2] = (u8)(rgb[2] + 0.5f);
    v->v.cn[3] = 0xFF;
}

// Same camera-projection matrix group the world uses in VR, so the dome's projection gets
// the eye camera the renderer injects (the flat sky left its own SKYBOXBLEND group behind,
// which the world projection then inherited).
static Gfx *vr_camera_group(Gfx *dl) {
    if (skip_interpolation || skip_persp_interp) {
        gEXMatrixGroupSkipAllAspect(dl++, MTXTAG_CAMERAPROJECTION, G_EX_NOPUSH, G_MTX_PROJECTION, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO);
    } else {
        gEXMatrixGroupSimpleNormal(dl++, MTXTAG_CAMERAPROJECTION, G_EX_NOPUSH, G_MTX_PROJECTION, G_EX_EDIT_NONE);
    }
    return dl;
}

static f32 vr_clamp_coord(f32 v) {
    if (v > 30000.0f) {
        return 30000.0f;
    }
    if (v < -30000.0f) {
        return -30000.0f;
    }
    return v;
}

Gfx *vr_sky_dome(Gfx *dl, Mtx *model, const u32 *colors, const s16 *stops, f32 horizon_shift) {
    CharacterChange *cc;
    Vtx *vtx;
    f32 ys[VR_DOME_RINGS];
    f32 rgb[3];
    f32 t;
    f32 c;
    f32 radius;
    f32 scale;
    f32 sc[4][4];
    f32 tr[4][4];
    s32 i;
    s32 k;
    s32 idx;

    cc = &character_change_array[0];
    vtx = vr_dome_vtx[D_global_asm_807444FC];

    // Rings: the four stops, with the long top band cut in three and the bottom band in two.
    ys[0] = (f32)stops[0];
    ys[1] = (f32)stops[0] + (f32)(stops[1] - stops[0]) * (1.0f / 3.0f);
    ys[2] = (f32)stops[0] + (f32)(stops[1] - stops[0]) * (2.0f / 3.0f);
    ys[3] = (f32)stops[1];
    ys[4] = (f32)stops[2];
    ys[5] = (f32)(stops[2] + stops[3]) * 0.5f;
    ys[6] = (f32)stops[3];

    vr_stop_color(colors, stops, ys[0], rgb);
    vr_set_vtx(&vtx[0], 0.0f, VR_SKY_UNIT, 0.0f, rgb);
    for (k = 0; k < VR_DOME_RINGS; k++) {
        t = (VR_SKY_LEVEL_ROW - (ys[k] + horizon_shift + VR_SKY_HORIZON_BIAS)) / VR_SKY_ROWS_PER_TAN;
        c = 1.0f / _sqrtf(1.0f + t * t);
        vr_stop_color(colors, stops, ys[k], rgb);
        for (i = 0; i < VR_DOME_SEGS; i++) {
            idx = 1 + (k * VR_DOME_SEGS) + i;
            vr_set_vtx(&vtx[idx],
                VR_SKY_UNIT * c * vr_seg_cos[i],
                VR_SKY_UNIT * c * t,
                VR_SKY_UNIT * c * vr_seg_sin[i],
                rgb);
        }
    }
    vr_stop_color(colors, stops, ys[VR_DOME_RINGS - 1], rgb);
    vr_set_vtx(&vtx[VR_DOME_VERTS - 1], 0.0f, -VR_SKY_UNIT, 0.0f, rgb);

    // Model matrix: unit sphere scaled out to the radius, centred on the game camera.
    radius = cc->far * VR_SKY_RADIUS_FRACTION;
    if (radius < 200.0f) {
        radius = 200.0f;
    } else if (radius > 30000.0f) {
        radius = 30000.0f;
    }
    scale = radius / VR_SKY_UNIT;
    guScaleF(sc, scale, scale, scale);
    guTranslateF(tr, vr_clamp_coord(cc->look_at_eye_x), vr_clamp_coord(cc->look_at_eye_y), vr_clamp_coord(cc->look_at_eye_z));
    guMtxCatF(sc, tr, sc);
    guMtxF2L(sc, model);

    gDPPipeSync(dl++);
    gDPSetRenderMode(dl++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);
    gDPSetCombineMode(dl++, G_CC_SHADE, G_CC_SHADE);
    gDPSetCycleType(dl++, G_CYC_1CYCLE);
    gSPLoadGeometryMode(dl++, 0);
    gSPSetGeometryMode(dl++, G_SHADE | G_SHADING_SMOOTH);
    dl = vr_camera_group(dl);
    // The world's own camera matrices (func_global_asm_8062C29C), then the dome's model matrix.
    gSPMatrix(dl++, osVirtualToPhysical(&cc->unk88[D_global_asm_807444FC]), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPMatrix(dl++, osVirtualToPhysical(&cc->unk8[D_global_asm_807444FC]), G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
    gSPMatrix(dl++, osVirtualToPhysical(model), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPViewport(dl++, osVirtualToPhysical(&cc->unk250[D_global_asm_807444FC]));
    gDPSetScissor(dl++, G_SC_NON_INTERLACE, cc->unk270[0], cc->unk270[1], cc->unk270[2], cc->unk270[3]);

    // Top cap: pole plus ring 0.
    gSPVertex(dl++, osVirtualToPhysical(&vtx[0]), 1 + VR_DOME_SEGS, 0);
    for (i = 0; i < VR_DOME_SEGS; i += 2) {
        gSP2Triangles(dl++,
            0, 1 + i, 1 + ((i + 1) % VR_DOME_SEGS), 0,
            0, 1 + ((i + 1) % VR_DOME_SEGS), 1 + ((i + 2) % VR_DOME_SEGS), 0);
    }
    // Bands: ring k and ring k + 1 in one 32 vertex load.
    for (k = 0; k < VR_DOME_RINGS - 1; k++) {
        gSPVertex(dl++, osVirtualToPhysical(&vtx[1 + (k * VR_DOME_SEGS)]), 2 * VR_DOME_SEGS, 0);
        for (i = 0; i < VR_DOME_SEGS; i++) {
            s32 a = i;
            s32 b = (i + 1) % VR_DOME_SEGS;
            s32 cc0 = VR_DOME_SEGS + i;
            s32 d = VR_DOME_SEGS + b;
            gSP2Triangles(dl++, a, cc0, d, 0, a, d, b, 0);
        }
    }
    // Bottom cap: last ring plus the pole.
    gSPVertex(dl++, osVirtualToPhysical(&vtx[1 + ((VR_DOME_RINGS - 1) * VR_DOME_SEGS)]), VR_DOME_SEGS + 1, 0);
    for (i = 0; i < VR_DOME_SEGS; i += 2) {
        gSP2Triangles(dl++,
            i, (i + 1) % VR_DOME_SEGS, VR_DOME_SEGS, 0,
            (i + 1) % VR_DOME_SEGS, (i + 2) % VR_DOME_SEGS, VR_DOME_SEGS, 0);
    }
    gDPPipeSync(dl++);
    vr_sky_dome_drawn = 1;
    return dl;
}

// One colour all the way round, for the maps whose "sky" is a flat fill.
Gfx *vr_sky_dome_solid(Gfx *dl, Mtx *model, u32 color) {
    u32 colors[4];
    s16 stops[4];

    colors[0] = color;
    colors[1] = color;
    colors[2] = color;
    colors[3] = color;
    stops[0] = 0;
    stops[1] = 492;
    stops[2] = 552;
    stops[3] = 960;
    return vr_sky_dome(dl, model, colors, stops, 0.0f);
}

extern Maps current_map;

// The textured skies are a 320x240 RGBA5551 picture that the flat code pans and wraps across the
// screen. A dome cannot reuse that mapping, so it takes the picture's average colour (sampled
// every other pixel, once per map and texture) times the same primitive colour the flat sky
// modulates with.
Gfx *vr_sky_dome_textured(Gfx *dl, Mtx *model, const u16 *texture, u8 tint_r, u8 tint_g, u8 tint_b) {
    static const u16 *cached_texture = NULL;
    static s32 cached_map = -1;
    static u32 cached_rgb[3];
    u32 color;
    u32 r;
    u32 g;
    u32 b;
    s32 x;
    s32 y;
    u32 samples;
    u16 p;

    if (texture == NULL) {
        return vr_sky_dome_solid(dl, model, 0x000000FF);
    }
    if ((texture != cached_texture) || (cached_map != (s32)current_map)) {
        r = 0;
        g = 0;
        b = 0;
        samples = 0;
        for (y = 0; y < 240; y += 2) {
            for (x = 0; x < 320; x += 2) {
                p = texture[(y * 320) + x];
                r += (p >> 11) & 0x1F;
                g += (p >> 6) & 0x1F;
                b += (p >> 1) & 0x1F;
                samples++;
            }
        }
        cached_rgb[0] = ((r * 255) / 31) / samples;
        cached_rgb[1] = ((g * 255) / 31) / samples;
        cached_rgb[2] = ((b * 255) / 31) / samples;
        cached_texture = texture;
        cached_map = (s32)current_map;
    }
    color = ((cached_rgb[0] * tint_r / 255) << 24)
        | ((cached_rgb[1] * tint_g / 255) << 16)
        | ((cached_rgb[2] * tint_b / 255) << 8)
        | 0xFF;
    return vr_sky_dome_solid(dl, model, color);
}

// Game-side half of the VR interface: the state snapshot the renderer reads (a seqlock, so a
// reader never sees a torn copy), the "VR is active" flag the patches gate on, and the host
// imports through which the game patches reach both. The contract is
// src/vr/include/dk64vr_game_state.h; the patch side is patches/vr_scene.c and friends.
//
// These are compiled into every build. dk64vr_is_active() stays 0 until the renderer calls
// dk64vr_set_active(1), so with VR off the patches behave exactly as they always did.
//
// Testing without the renderer: set DK64_VR_FORCE_PATCHES=1 in the environment and
// dk64vr_is_active() reports 1, which switches the patches' VR paths on (culling off, sky dome,
// no lens flare, scene log lines) in an ordinary flat run. Unset, or "0", it does nothing.

#include <atomic>
#include <chrono>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "../src/vr/include/dk64vr_game_state.h"
#include "../src/vr/include/dk64vr_control.h"
#include "../patches/vr_state.h"

// The patches carry their own copy of the scene numbers; keep it in step with the contract.
static_assert(VR_SCENE_UNKNOWN == DK64VR_SCENE_UNKNOWN, "scene numbering drifted");
static_assert(VR_SCENE_GAMEPLAY == DK64VR_SCENE_GAMEPLAY, "scene numbering drifted");
static_assert(VR_SCENE_FIRST_PERSON == DK64VR_SCENE_FIRST_PERSON, "scene numbering drifted");
static_assert(VR_SCENE_CUTSCENE == DK64VR_SCENE_CUTSCENE, "scene numbering drifted");
static_assert(VR_SCENE_TRANSITION == DK64VR_SCENE_TRANSITION, "scene numbering drifted");
static_assert(VR_SCENE_MENU == DK64VR_SCENE_MENU, "scene numbering drifted");
static_assert(VR_SCENE_PAUSE == DK64VR_SCENE_PAUSE, "scene numbering drifted");
static_assert(VR_SCENE_ARCADE == DK64VR_SCENE_ARCADE, "scene numbering drifted");
static_assert(VR_SCENE_MULTIPLAYER == DK64VR_SCENE_MULTIPLAYER, "scene numbering drifted");
static_assert(VR_SCENE_COUNT == DK64VR_SCENE_COUNT, "scene numbering drifted");

namespace {

// The snapshot is nineteen 32-bit words (ints and floats). Held as atomic words so the seqlock
// copies are well defined; the sequence counter is odd while a write is in progress.
constexpr size_t kStateWords = sizeof(dk64vr_game_state) / sizeof(uint32_t);
static_assert(sizeof(dk64vr_game_state) % sizeof(uint32_t) == 0, "state must be whole words");
static_assert(kStateWords == 19, "state layout changed; update patches/vr_state.h");

std::atomic<uint32_t> g_state_seq{0};
std::atomic<uint32_t> g_state_words[kStateWords];
std::mutex g_publish_mutex; // one game thread publishes; this just keeps a stray second writer honest

std::atomic<int> g_vr_active{0};
std::atomic<int> g_vr_paused{0};

bool force_patches_from_env() {
    static const bool forced = [] {
        const char* value = std::getenv("DK64_VR_FORCE_PATCHES");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return forced;
}

} // namespace

extern "C" void dk64vr_publish_game_state(const dk64vr_game_state* state) {
    if (state == nullptr) {
        return;
    }
    uint32_t words[kStateWords];
    std::memcpy(words, state, sizeof(words));

    std::lock_guard<std::mutex> lock(g_publish_mutex);
    const uint32_t seq = g_state_seq.load(std::memory_order_relaxed);
    g_state_seq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i < kStateWords; i++) {
        g_state_words[i].store(words[i], std::memory_order_relaxed);
    }
    g_state_seq.store(seq + 2, std::memory_order_release);
}

extern "C" void dk64vr_get_game_state(dk64vr_game_state* out) {
    if (out == nullptr) {
        return;
    }
    uint32_t words[kStateWords];
    for (;;) {
        const uint32_t before = g_state_seq.load(std::memory_order_acquire);
        if (before & 1) {
            continue; // a write is in flight
        }
        for (size_t i = 0; i < kStateWords; i++) {
            words[i] = g_state_words[i].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_state_seq.load(std::memory_order_relaxed) == before) {
            break;
        }
    }
    std::memcpy(out, words, sizeof(words));
}

extern "C" int dk64vr_is_active(void) {
    return (g_vr_active.load(std::memory_order_relaxed) != 0 || force_patches_from_env()) ? 1 : 0;
}

extern "C" void dk64vr_set_active(int active) {
    g_vr_active.store(active != 0 ? 1 : 0, std::memory_order_relaxed);
}

extern "C" void dk64vr_set_paused(int paused) {
    g_vr_paused.store(paused != 0 ? 1 : 0, std::memory_order_relaxed);
}

extern "C" int dk64vr_is_paused(void) {
    return g_vr_paused.load(std::memory_order_relaxed);
}

namespace {
std::mutex g_turn_mutex;
int g_turn_degrees = 0;
std::chrono::steady_clock::time_point g_turn_time;
}

extern "C" void dk64vr_request_turn(int degrees) {
    std::lock_guard<std::mutex> lock(g_turn_mutex);
    g_turn_degrees += degrees;
    g_turn_time = std::chrono::steady_clock::now();
}

extern "C" int dk64vr_take_turn(void) {
    std::lock_guard<std::mutex> lock(g_turn_mutex);
    const int degrees = g_turn_degrees;
    g_turn_degrees = 0;
    if (degrees == 0) {
        return 0;
    }
    if ((std::chrono::steady_clock::now() - g_turn_time) > std::chrono::milliseconds(300)) {
        std::printf("[vr] turn: %d deg dropped (not taken within 0.3 s)\n", degrees);
        return 0;
    }
    return degrees;
}

namespace {
std::atomic<int> g_hud_diag{0};
}

extern "C" void dk64vr_request_hud_diag(void) {
    g_hud_diag.store(1);
}

extern "C" int dk64vr_take_hud_diag(void) {
    return g_hud_diag.exchange(0);
}

// The runtime's quit flag (librecomp/src/recomp.cpp), so a paused game never holds up a quit.
extern std::atomic_bool exited;

// Host imports for the game patches (addresses in patches/syms.ld).

extern "C" void recomp_vr_is_active(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return(ctx, dk64vr_is_active());
}

extern "C" void recomp_vr_publish_game_state(uint8_t* rdram, recomp_context* ctx) {
    // The patch side lays the snapshot out exactly as dk64vr_game_state; words in RDRAM are
    // host-endian, so it can be read in place.
    const dk64vr_game_state* state = _arg<0, const dk64vr_game_state*>(rdram, ctx);
    dk64vr_publish_game_state(state);

    // While the VR host says so, the game thread waits here (dk64vr_control.h). Never set in a
    // flat build.
    while (g_vr_paused.load(std::memory_order_relaxed) && !exited.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

extern "C" void recomp_vr_take_turn(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return(ctx, dk64vr_take_turn());
}

extern "C" void recomp_vr_take_hud_diag(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return(ctx, dk64vr_take_hud_diag());
}

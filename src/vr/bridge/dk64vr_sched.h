// Display-time scheduler for a game that runs on its own clock (lessons 3 and 31).
//
// DK64 runs its own fixed tick (30 Hz in play, 20 Hz in some intros) and hands RT64 one
// workload per tick. The XR frame loop consumes each new workload once, at the first XR frame
// after it arrives, and replays the held pair (previous, newest) at every display frame. This
// scheduler places the newest tick N on an ideal grid T(N) = base + N * tick and gives the
// interpolation weight for a display time r (predictedDisplayTime):
//
//   w = clamp(1 - (T(N) - r) / tick, 0, 1)    (0 shows tick N-1, 1 shows tick N)
//
// The grid is anchored so a workload is shown fully one tick after the display frame it was
// consumed in (w = 0 on arrival, which continues the previous pair's w = 1 image). The image
// is continuous for any arrival up to one display frame later than T(N) - tick, so the grid
// follows the *earliest* arrival phase: an arrival earlier than the grid moves the grid to it
// at once, one more than a display period later pulls it later only by a small gain (which absorbs drift between the
// game's clock and the display's). An arrival more than `max_offset_ticks` off the grid (a
// load, a stall) re-anchors it. A change of the game's rate re-anchors too, once the new rate
// has held for `rate_hold` arrivals (DK64 reports a one-off long frame as a lower rate).
// Pure: no clock reads, times are seconds from the caller.
#pragma once

#include <cstdint>

namespace dk64vr {

struct TickScheduler {
    double tick_s = 1.0 / 30.0;
    double base_s = 0.0;  // display time of tick 0
    int64_t newest = -1;  // index of the newest tick
    bool anchored = false;
    double late_gain = 0.05;
    double period_s = 0.0;  // display frame period: arrivals up to this late are seamless
    double max_offset_ticks = 2.0;
    int rate_hold = 3;
    double pending_tick_s = 0.0;
    int pending_count = 0;
    uint64_t arrivals = 0;
    uint64_t rebases = 0;
    double last_offset_s = 0.0;  // grid minus ideal at the last arrival, before correction

    void reset();

    // A new tick was consumed by the display frame shown at display_s. tick_s is the game's
    // current tick length (1 / its frame rate). Returns true when the grid was (re)anchored.
    bool arrive(double display_s, double tick_s_now);

    // Interpolation weight for a frame shown at display_s.
    float weight(double display_s) const;

    // Display time at which the newest tick is shown fully.
    double newestTime() const { return base_s + double(newest) * tick_s; }
};

}

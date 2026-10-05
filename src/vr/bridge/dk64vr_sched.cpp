#include "dk64vr_sched.h"

#include <cmath>

namespace dk64vr {

void TickScheduler::reset() {
    anchored = false;
    newest = -1;
}

bool TickScheduler::arrive(double display_s, double tick_s_now) {
    arrivals++;
    if (!(tick_s_now > 0.0)) {
        tick_s_now = tick_s;
    }

    bool rateChanged = false;
    if (std::fabs(tick_s_now - tick_s) > 1e-9) {
        if (std::fabs(tick_s_now - pending_tick_s) > 1e-9) {
            pending_tick_s = tick_s_now;
            pending_count = 0;
        }

        rateChanged = (++pending_count >= rate_hold) || !anchored;
    }
    else {
        pending_count = 0;
    }

    if (!anchored || rateChanged) {
        tick_s = tick_s_now;
        pending_count = 0;
        anchored = true;
        newest = 0;
        base_s = display_s + tick_s;
        last_offset_s = 0.0;
        rebases++;
        return true;
    }

    newest++;
    const double ideal = display_s + tick_s;
    const double offset = newestTime() - ideal;
    last_offset_s = offset;
    if (std::fabs(offset) > max_offset_ticks * tick_s) {
        base_s = ideal - double(newest) * tick_s;
        rebases++;
        return true;
    }

    // offset > 0: this arrival came earlier than the grid allows for; < 0: later.
    if (offset > 0.0) {
        base_s -= offset;
    }
    else if (-offset > period_s) {
        base_s += late_gain * (-offset - period_s);
    }
    return false;
}

float TickScheduler::weight(double display_s) const {
    if (!anchored || newest < 0) {
        return 1.0f;
    }

    const double w = 1.0 - (newestTime() - display_s) / tick_s;
    if (!(w > 0.0)) {
        return 0.0f;
    }

    if (w > 1.0) {
        return 1.0f;
    }

    return float(w);
}

}

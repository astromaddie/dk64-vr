// Diagnostics for unattended runs (standard.md section 8): image dumps written off the frame
// thread, the autostart input schedule, the self-exit timer and the stall watchdog.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dk64vr {

// Writes an RGBA8 (or BGRA8, swapped to RGBA) image as PNG on a detached thread. The pixels are
// copied before this returns. Alpha is written as 255 unless keepAlpha (the HUD image).
void writePngAsync(const std::string &path, const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t rowBytes, bool bgra, bool keepAlpha = false);

// Autostart: for `seconds` after start(), presses A (and every fourth time Start) on a schedule,
// merged into player 1's N64 buttons, until stop().
struct Autostart {
    void start(double nowS, double seconds);
    void stop(const char *reason);
    // Buttons to OR into the N64 pad for the time now.
    uint16_t buttons(double nowS);
    bool active() const { return running; }

    bool running = false;
    double startS = 0.0;
    double endS = 0.0;
    uint32_t presses = 0;
    int64_t lastPress = -1;
};

// Stall watchdog: a thread that logs `[vr-stall]` once per stall when the frame loop has not
// passed a checkpoint for `seconds` while the session runs.
void watchdogStart(double seconds);
void watchdogBeat(const char *stage);
void watchdogArm(bool armed);  // only while frames are expected
void watchdogStop();

// Requests a clean quit through the runtime (ultramodern::quit), as the quit prompt does.
void requestQuit(const char *reason);

double monotonicSeconds();

}

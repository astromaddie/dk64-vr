#include "dk64vr_diag.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "ultramodern/ultramodern.hpp"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include "dk64vr_log.h"

namespace dk64vr {

double monotonicSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void writePngAsync(const std::string &path, const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t rowBytes, bool bgra, bool keepAlpha) {
    std::vector<uint8_t> rgba(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src = pixels + size_t(y) * rowBytes;
        uint8_t *dst = rgba.data() + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; x++) {
            dst[x * 4 + 0] = src[x * 4 + (bgra ? 2 : 0)];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + (bgra ? 0 : 2)];
            dst[x * 4 + 3] = keepAlpha ? src[x * 4 + 3] : 255;
        }
    }

    std::thread([path, width, height, data = std::move(rgba)]() {
        if (stbi_write_png(path.c_str(), int(width), int(height), 4, data.data(), int(width * 4))) {
            dk64vr_logf("dump: wrote %s (%ux%u)", path.c_str(), width, height);
        }
        else {
            dk64vr_logf("dump: could not write %s", path.c_str());
        }
    }).detach();
}

// ---------------------------------------------------------------------------------------------
// Autostart

// N64 button bits (libultra cont.h).
static const uint16_t BUTTON_A = 0x8000;
static const uint16_t BUTTON_START = 0x1000;

void Autostart::start(double nowS, double seconds) {
    running = true;
    startS = nowS;
    endS = nowS + seconds;
    presses = 0;
    lastPress = -1;
    dk64vr_logf("diag_autostart: pressing A and Start for %.0f s", seconds);
}

void Autostart::stop(const char *reason) {
    if (running) {
        running = false;
        dk64vr_logf("diag_autostart: stopped after %u presses (%s)", presses, reason);
    }
}

uint16_t Autostart::buttons(double nowS) {
    if (!running) {
        return 0;
    }

    if (nowS >= endS) {
        stop("time");
        return 0;
    }

    // A press of 150 ms every 1.25 s, the first ones 5 s in (past the boot logos); every
    // fourth one is Start, which the title screen and the story intro take.
    const double t = nowS - startS - 5.0;
    if (t < 0.0) {
        return 0;
    }

    const double period = 1.25;
    const int64_t index = int64_t(t / period);
    const double phase = t - double(index) * period;
    if (phase > 0.15) {
        return 0;
    }

    if (index != lastPress) {
        lastPress = index;
        presses++;
    }

    return ((index % 4) == 3) ? BUTTON_START : BUTTON_A;
}

// ---------------------------------------------------------------------------------------------
// Watchdog

namespace {
    std::mutex watchdogMutex;
    std::condition_variable watchdogCondition;
    std::thread watchdogThread;
    bool watchdogRunning = false;
    bool watchdogArmed = false;
    double watchdogLastBeat = 0.0;
    const char *watchdogStage = "none";
}

void watchdogStart(double seconds) {
    std::scoped_lock lock(watchdogMutex);
    if (watchdogRunning) {
        return;
    }

    watchdogRunning = true;
    watchdogLastBeat = monotonicSeconds();
    watchdogThread = std::thread([seconds]() {
        bool reported = false;
        std::unique_lock lock(watchdogMutex);
        while (watchdogRunning) {
            watchdogCondition.wait_for(lock, std::chrono::milliseconds(500));
            const double now = monotonicSeconds();
            const double since = now - watchdogLastBeat;
            if (watchdogArmed && (since > seconds)) {
                if (!reported) {
                    dk64vr_logpf("[vr-stall]", "no frame for %.1f s; last checkpoint: %s", since, watchdogStage);
                    reported = true;
                }
            }
            else if (reported) {
                dk64vr_logpf("[vr-stall]", "frames resumed");
                reported = false;
            }
        }
    });
}

void watchdogBeat(const char *stage) {
    std::scoped_lock lock(watchdogMutex);
    watchdogLastBeat = monotonicSeconds();
    watchdogStage = stage;
}

void watchdogArm(bool armed) {
    std::scoped_lock lock(watchdogMutex);
    if (armed && !watchdogArmed) {
        watchdogLastBeat = monotonicSeconds();
    }

    watchdogArmed = armed;
}

void watchdogStop() {
    {
        std::scoped_lock lock(watchdogMutex);
        if (!watchdogRunning) {
            return;
        }

        watchdogRunning = false;
    }

    watchdogCondition.notify_all();
    if (watchdogThread.joinable()) {
        watchdogThread.join();
    }
}

void requestQuit(const char *reason) {
    // The same path as the quit prompt's "quit" (an SDL_QUIT would only open the prompt once the
    // game runs).
    dk64vr_logf("quit requested: %s", reason);
    ultramodern::quit();
}

}

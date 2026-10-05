#include "dk64vr_foveation.h"

#include <algorithm>
#include <cmath>

namespace dk64vr {

const char *foveationName(FoveationLevel level) {
    static const char *names[] = { "off", "low", "medium", "high" };
    const unsigned i = unsigned(level);
    return i < 4 ? names[i] : "off";
}

static bool validFov(const FovTangents &f) {
    return std::isfinite(f.left) && std::isfinite(f.right) && std::isfinite(f.up) && std::isfinite(f.down)
        && f.right > f.left && f.up > f.down;
}

FoveationCentre projectionCentre(const FovTangents &f) {
    if (!validFov(f)) return {};
    return { std::clamp(-f.left / (f.right - f.left), 0.0f, 1.0f),
        std::clamp(f.up / (f.up - f.down), 0.0f, 1.0f) };
}

FoveationCentre gazeCentre(const FovTangents &f, Vec3 d) {
    if (!validFov(f) || !std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z) || d.z >= -1e-5f) {
        return projectionCentre(f);
    }
    return { std::clamp((d.x / -d.z - f.left) / (f.right - f.left), 0.0f, 1.0f),
        std::clamp((f.up - d.y / -d.z) / (f.up - f.down), 0.0f, 1.0f) };
}

static uint8_t supportedRate(uint32_t desiredArea, uint16_t supported) {
    uint8_t best = 0;
    uint32_t area = 1;
    for (uint8_t x = 0; x <= 2; x++) {
        for (uint8_t y = 0; y <= 2; y++) {
            if (desiredArea <= 4 && (x > 1 || y > 1)) continue;
            const uint8_t code = (x << 2) | y;
            const uint32_t a = 1u << (x + y);
            if ((supported & (1u << code)) && a <= desiredArea && (a > area || (a == area && x == y))) {
                best = code;
                area = a;
            }
        }
    }
    return best;
}

ShadingRateMap makeShadingRateMap(uint32_t tw, uint32_t th, uint32_t ew, uint32_t eh,
    uint32_t bx, uint32_t by, FoveationCentre centre, FoveationLevel level, uint16_t supported) {
    ShadingRateMap map;
    if (!tw || !th || !ew || !eh || !bx || !by) return map;
    map.width = (tw - 1) / bx + 1;
    map.height = (th - 1) / by + 1;
    map.rates.resize(size_t(map.width) * map.height, 0);
    if (level == FoveationLevel::Off) return map;
    if (!std::isfinite(centre.x) || !std::isfinite(centre.y)) centre = {};
    const float cx = std::clamp(centre.x, 0.0f, 1.0f) * ew;
    const float cy = std::clamp(centre.y, 0.0f, 1.0f) * eh;
    const float scale = float(std::min(ew, eh));
    const float inner = (level == FoveationLevel::Low ? 0.38f : level == FoveationLevel::Medium ? 0.28f : 0.20f) * scale;
    const float outer = (level == FoveationLevel::High ? 0.42f : 0.58f) * scale;
    for (uint32_t y = 0; y < map.height; y++) {
        for (uint32_t x = 0; x < map.width; x++) {
            // Distance to the closest point of this tile, not its centre: a coarse
            // tile cannot straddle the full-resolution fovea.
            const float dx = std::max({ float(x * bx) - cx, cx - float(std::min((x + 1) * bx, tw)), 0.0f });
            const float dy = std::max({ float(y * by) - cy, cy - float(std::min((y + 1) * by, th)), 0.0f });
            const float radius = std::sqrt(dx * dx + dy * dy);
            const uint32_t area = radius <= inner ? 1 : (radius <= outer || level == FoveationLevel::Low) ? 4 : 16;
            map.rates[size_t(y) * map.width + x] = supportedRate(area, supported);
        }
    }
    return map;
}

std::vector<uint8_t> shadingRateVisualization(const ShadingRateMap &map) {
    std::vector<uint8_t> pixels(map.rates.size() * 4);
    for (size_t i = 0; i < map.rates.size(); i++) {
        const uint8_t code = map.rates[i];
        const uint32_t area = 1u << ((code >> 2) + (code & 3));
        pixels[i * 4 + 0] = area == 1 ? 255 : area <= 4 ? 40 : 255;
        pixels[i * 4 + 1] = area == 1 ? 255 : area <= 4 ? 220 : 130;
        pixels[i * 4 + 2] = area == 1 ? 255 : area <= 4 ? 80 : 40;
        pixels[i * 4 + 3] = 255;
    }
    return pixels;
}

}

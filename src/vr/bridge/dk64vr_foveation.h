// Pure projection and shading-rate map math. No OpenXR/Vulkan dependency.
#pragma once

#include <cstdint>
#include <vector>

#include "dk64vr_math.h"

namespace dk64vr {

enum class FoveationLevel { Off, Low, Medium, High };
const char *foveationName(FoveationLevel level);

struct FoveationCentre { float x = 0.5f, y = 0.5f; }; // top-left image UV
FoveationCentre projectionCentre(const FovTangents &fov);
// Direction in the eye's coordinates, looking along -Z. Invalid/behind-eye gaze
// falls back to the projection centre; out-of-frustum gaze clamps to the edge.
FoveationCentre gazeCentre(const FovTangents &fov, Vec3 direction);

struct ShadingRateMap {
    uint32_t width = 0, height = 0;
    // VK_KHR_fragment_shading_rate R8_UINT encoding: log2(x) << 2 | log2(y).
    std::vector<uint8_t> rates;
};

// supportedRates has bit <encoded rate> set for each supported 1-sample rate.
// The visible eye occupies the top-left of the allocated render target, which
// may be padded by RT64. Tiles touching the sharp disc always use 1x1.
ShadingRateMap makeShadingRateMap(uint32_t targetWidth, uint32_t targetHeight,
    uint32_t eyeWidth, uint32_t eyeHeight, uint32_t tileWidth, uint32_t tileHeight,
    FoveationCentre centre, FoveationLevel level, uint16_t supportedRates);
// White = 1x1; green = up to 2x2; orange = coarser. One RGBA pixel per map texel.
std::vector<uint8_t> shadingRateVisualization(const ShadingRateMap &map);

}

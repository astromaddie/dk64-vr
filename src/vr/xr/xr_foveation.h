// App-owned KHR shading-rate attachments. Workload thread, after device creation.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "plume_vulkan.h"
#include "dk64vr_foveation.h"

namespace dk64vr {

class XrFoveation {
public:
    // Preserves plume's extension/feature chain. prepared is used immediately by
    // xrCreateVulkanDeviceKHR; its extra storage lives in this object.
    void prepareDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo &base,
        VkDeviceCreateInfo &prepared, FoveationLevel level, bool valveDisabled);
    void deviceCreated(plume::VulkanDevice *device);
    bool enabled() const { return active && !failed; }
    // The level of an attachment that came up at launch can change at any time (the map is
    // rebuilt every frame); Off is the host not asking for a companion at all.
    void setLevel(FoveationLevel next) { if (active && next != FoveationLevel::Off) level = next; }
    // Called only for the main gameplay eye raster. Map upload completes in this
    // command list before the companion framebuffer is bound. No readback/wait.
    plume::RenderFramebuffer *framebuffer(uint32_t eye, plume::RenderCommandList *commands,
        plume::RenderFramebuffer *base, uint32_t eyeWidth, uint32_t eyeHeight,
        FoveationCentre centre, uint64_t frame);
    void dump(uint32_t eye, uint32_t eyeWidth, uint32_t eyeHeight, const std::string &path, bool attached);
    void reset(); // after the last eye submission completes

private:
    struct Image;
    std::map<uint64_t, std::shared_ptr<Image>> images[2];
    std::shared_ptr<Image> lastImage[2];
    plume::VulkanDevice *device = nullptr;
    FoveationLevel level = FoveationLevel::Off;
    bool active = false;
    bool failed = false;
    VkExtent2D texelSize = { 32, 32 };
    uint16_t supportedRates = 1;
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
    std::vector<const char *> extensions;
};

}

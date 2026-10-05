#include "xr_foveation.h"

#include <algorithm>
#include <cstring>

#include "dk64vr_log.h"
#include "../diag/dk64vr_diag.h"

namespace dk64vr {

void XrFoveation::prepareDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo &base,
    VkDeviceCreateInfo &prepared, FoveationLevel requested, bool valveDisabled) {
    level = requested;
    prepared = base;
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> offered(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, offered.data());
    const auto has = [&](const char *name) {
        return std::any_of(offered.begin(), offered.end(), [&](const auto &e) { return strcmp(e.extensionName, name) == 0; });
    };
    const bool khr = has(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    const bool fdm = has(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
    features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
    VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    if (khr) {
        query.pNext = &features;
        vkGetPhysicalDeviceFeatures2(physical, &query);
    }
    // An FDM layer can enable fragmentDensityMap while creating the VkDevice.
    // Vulkan forbids it alongside attachmentFragmentShadingRate. Never mix them.
    bool chainHasFdm = false;
    for (auto p = static_cast<const VkBaseInStructure *>(base.pNext); p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT) {
            chainHasFdm = reinterpret_cast<const VkPhysicalDeviceFragmentDensityMapFeaturesEXT *>(p)->fragmentDensityMap != 0;
        }
    }
    active = requested != FoveationLevel::Off && valveDisabled && !chainHasFdm && features.attachmentFragmentShadingRate;
    const char *reason = requested == FoveationLevel::Off ? "setting-off" : !valveDisabled ? "Valve-layer-selected"
        : chainHasFdm ? "FDM-already-enabled" : !features.attachmentFragmentShadingRate ? "KHR-attachment-unsupported" : "enabled";
    dk64vr_logpf("[vr-fov]", "requested=%s backend=%s VK_KHR_fragment_shading_rate=%d attachment=%d pipeline=%d VK_EXT_fragment_density_map=%d profile=app-owned fdm=none Valve_layer=%s Valve_injected=%s reason=%s",
        foveationName(requested), active ? "khr_shading_rate" : "off", khr, features.attachmentFragmentShadingRate,
        features.pipelineFragmentShadingRate, fdm, valveDisabled ? "disabled" : "allowed",
        valveDisabled ? "no" : "unknown-see-FDM_DEBUG", reason);
    if (!active) return;

    VkPhysicalDeviceFragmentShadingRatePropertiesKHR props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 properties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties.pNext = &props;
    vkGetPhysicalDeviceProperties2(physical, &properties);
    texelSize = props.minFragmentShadingRateAttachmentTexelSize;
    // A square tile avoids aspect-ratio ambiguity; use a fine map when the device permits.
    const uint32_t square = std::max({ texelSize.width, texelSize.height, 16u });
    if (square <= props.maxFragmentShadingRateAttachmentTexelSize.width && square <= props.maxFragmentShadingRateAttachmentTexelSize.height) {
        texelSize = { square, square };
    }
    const auto getRates = vkGetPhysicalDeviceFragmentShadingRatesKHR; // volk's instance-loaded command
    uint32_t rateCount = 0;
    if (getRates && getRates(physical, &rateCount, nullptr) == VK_SUCCESS) {
        std::vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(rateCount, { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR });
        if (getRates(physical, &rateCount, rates.data()) == VK_SUCCESS) {
            for (const auto &rate : rates) {
                const uint32_t w = rate.fragmentSize.width, h = rate.fragmentSize.height;
                if (!(rate.sampleCounts & VK_SAMPLE_COUNT_1_BIT) || w > 4 || h > 4 || !w || !h) continue;
                const uint8_t code = ((w == 4 ? 2 : w == 2 ? 1 : 0) << 2) | (h == 4 ? 2 : h == 2 ? 1 : 0);
                supportedRates |= 1u << code;
            }
        }
    }
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8_UINT, &format);
    if (!getRates || supportedRates == 1 || !texelSize.width || !texelSize.height
        || !(format.optimalTilingFeatures & VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR)) {
        active = false;
        dk64vr_logpf("[vr-fov]", "backend=off reason=no-supported-coarse-rate-or-format");
        return;
    }
    dk64vr_logpf("[vr-fov]", "tile=%ux%u supported_rate_mask=0x%04x depthStencilWrites=%d sampleMask=%d nontrivialCombiners=%d",
        texelSize.width, texelSize.height, supportedRates, props.fragmentShadingRateWithShaderDepthStencilWrites,
        props.fragmentShadingRateWithShaderSampleMask, props.fragmentShadingRateNonTrivialCombinerOps);
    extensions.assign(base.ppEnabledExtensionNames, base.ppEnabledExtensionNames + base.enabledExtensionCount);
    if (std::none_of(extensions.begin(), extensions.end(), [](const char *e) { return strcmp(e, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0; })) {
        extensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    }
    features.primitiveFragmentShadingRate = VK_FALSE;
    features.pipelineFragmentShadingRate = VK_FALSE; // pipeline rate stays 1x1; only the attachment supplies rates
    features.attachmentFragmentShadingRate = VK_TRUE;
    features.pNext = const_cast<void *>(base.pNext);
    prepared.pNext = &features;
    prepared.enabledExtensionCount = uint32_t(extensions.size());
    prepared.ppEnabledExtensionNames = extensions.data();
}

void XrFoveation::deviceCreated(plume::VulkanDevice *created) {
    device = created;
    if (active) {
        device->cmdSetFragmentShadingRate = reinterpret_cast<PFN_vkCmdSetFragmentShadingRateKHR>(vkGetDeviceProcAddr(device->vk, "vkCmdSetFragmentShadingRateKHR"));
        if (!device->cmdSetFragmentShadingRate) {
            active = false;
            dk64vr_logpf("[vr-fov]", "backend=off reason=missing-vkCmdSetFragmentShadingRateKHR");
        }
        else dk64vr_logpf("[vr-fov]", "enabled_extensions=VK_KHR_fragment_shading_rate attachment=1 pipeline=0 primitive=0 normal-images=1");
    }
}

struct XrFoveation::Image {
    plume::VulkanDevice *device = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::unique_ptr<plume::RenderBuffer> upload;
    ShadingRateMap map;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t updatedFrame = 0;
    FoveationCentre centre;

    ~Image() {
        if (view) vkDestroyImageView(device->vk, view, nullptr);
        if (image) vmaDestroyImage(device->allocator, image, allocation);
    }

    bool create(plume::VulkanDevice *owner, const ShadingRateMap &initial) {
        device = owner;
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8_UINT;
        info.extent = { initial.width, initial.height, 1 };
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo memory = {};
        memory.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        VkResult res = vmaCreateImage(device->allocator, &info, &memory, &image, &allocation, nullptr);
        if (res != VK_SUCCESS) return false;
        VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = info.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = viewInfo.subresourceRange.layerCount = 1;
        res = vkCreateImageView(device->vk, &viewInfo, nullptr, &view);
        if (res != VK_SUCCESS) return false;
        upload = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(initial.rates.size()));
        return upload != nullptr;
    }

    bool record(plume::RenderCommandList *commands, ShadingRateMap next) {
        if (map.rates == next.rates) return true;
        void *bytes = upload->map();
        if (!bytes) return false;
        memcpy(bytes, next.rates.data(), next.rates.size());
        // Plume's unmap does not flush non-coherent host memory. Keep this narrow
        // upload independent of the backend's assumptions about coherent heaps.
        const auto buffer = static_cast<plume::VulkanBuffer *>(upload.get());
        const VkResult flushed = vmaFlushAllocation(device->allocator, buffer->allocation, 0, next.rates.size());
        upload->unmap();
        if (flushed != VK_SUCCESS) return false;
        commands->setFramebuffer(nullptr); // raw copy/barriers must be outside a render pass
        const VkCommandBuffer cmd = static_cast<plume::VulkanCommandList *>(commands)->vk;
        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy = {};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = { next.width, next.height, 1 };
        vkCmdCopyBufferToImage(cmd, buffer->vk, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
        barrier.oldLayout = barrier.newLayout;
        barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        layout = barrier.newLayout;
        map = std::move(next);
        return true;
    }
};

plume::RenderFramebuffer *XrFoveation::framebuffer(uint32_t eye, plume::RenderCommandList *commands,
    plume::RenderFramebuffer *base, uint32_t ew, uint32_t eh, FoveationCentre centre, uint64_t frame) {
    if (!active || failed || eye >= 2) return base;
    auto fb = static_cast<plume::VulkanFramebuffer *>(base);
    // The eye render is single-sample; do not change offscreen/MSAA or depth-only passes.
    if (fb->colorAttachments.size() != 1 || fb->colorAttachments[0]->desc.multisampling.sampleCount != 1) return base;
    const uint64_t key = (uint64_t(fb->width) << 32) | fb->height;
    auto &image = images[eye][key];
    if (!image) {
        image = std::make_shared<Image>();
        const auto map = makeShadingRateMap(fb->width, fb->height, ew, eh, texelSize.width, texelSize.height, centre, level, supportedRates);
        if (map.rates.empty() || !image->create(device, map)) {
            failed = true;
            dk64vr_logpf("[vr-fov]", "backend=off reason=rate-image-create-failed eye=%u", eye);
            return base;
        }
        dk64vr_logpf("[vr-fov]", "eye=%u pass=main-raster map=%ux%u R8_UINT target=%ux%u visible=%ux%u centre=%.4f,%.4f HUD=full cinema=full panel=full offscreen=full Valve_injected=no",
            eye, map.width, map.height, fb->width, fb->height, ew, eh, centre.x, centre.y);
    }
    if (image->updatedFrame != frame) {
        image->centre = centre;
        if (!image->record(commands, makeShadingRateMap(fb->width, fb->height, ew, eh, texelSize.width, texelSize.height, centre, level, supportedRates))) {
            failed = true;
            dk64vr_logpf("[vr-fov]", "backend=off reason=rate-upload-failed eye=%u", eye);
            return base;
        }
        image->updatedFrame = frame;
    }
    plume::VulkanShadingRateAttachment attachment;
    attachment.imageView = image->view;
    attachment.texelSize = texelSize;
    attachment.lifetime = image;
    plume::VulkanFramebuffer *companion = fb->withShadingRate(attachment);
    if (!companion) {
        failed = true;
        dk64vr_logpf("[vr-fov]", "backend=off reason=companion-framebuffer-create-failed eye=%u", eye);
        return base;
    }
    lastImage[eye] = image;
    return companion;
}

void XrFoveation::dump(uint32_t eye, uint32_t ew, uint32_t eh, const std::string &path, bool attached) {
    if (eye >= 2) return;
    const auto image = lastImage[eye];
    const auto map = attached && image ? image->map
        : makeShadingRateMap(ew, eh, ew, eh, texelSize.width, texelSize.height, {}, FoveationLevel::Off, 1);
    if (map.rates.empty()) return;
    const auto pixels = shadingRateVisualization(map);
    writePngAsync(path, pixels.data(), map.width, map.height, map.width * 4, false);
    dk64vr_logpf("[vr-fov]", "dump=%s attached=%d map=%ux%u centre=%.4f,%.4f white=1x1 green=up-to-2x2 orange=coarser",
        path.c_str(), attached && image != nullptr, map.width, map.height, attached && image ? image->centre.x : 0.5f, attached && image ? image->centre.y : 0.5f);
}

void XrFoveation::reset() {
    images[0].clear();
    images[1].clear();
    lastImage[0].reset();
    lastImage[1].reset();
}

}

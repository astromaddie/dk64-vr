// The OpenXR host for DK64 VR: instance and Vulkan binding (through plume's create hooks), the
// session and its frame loop (driven from RT64's workload thread through RT64::VrHost), the eye
// and panel swapchains, the composition layers, and the diagnostics of standard.md section 8.
//
// Threads: install and shutdown run on the main thread; the plume hooks on the thread that sets
// up RT64; beginFrame, eyeCamera, recordEye, eyeDone and endFrame on RT64's workload thread;
// panelAcquire and panelRelease on RT64's present thread. Every OpenXR call that may touch the
// Vulkan queue holds the queue's plume mutex.
#include "dk64vr_host.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "plume_vulkan.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "hle/rt64_vr.h"
#include "render/rt64_render_worker.h"

#include "dk64vr_game_state.h"
#include "dk64vr_input.h"
#include "dk64vr_control.h"
#include "dk64vr_log.h"
#include "dk64vr_math.h"
#include "dk64vr_sched.h"
#include "dk64vr_settings.h"
#include "sfvr/sfvr_perf.h"
#include "../diag/dk64vr_diag.h"
#include "xr_input.h"

#include <SDL.h>
#include "recompui/recompui.h"
#include "recompinput/recompinput.h"

namespace dk64vr {

namespace {

// The camera projection's matrix groups (patches/common_structs.h): MTXTAG_CAMERAPROJECTION, and
// MTXTAG_SKYBOXBLEND, which tags the world projection in maps with a gradient sky.
const uint32_t CameraGroup = 5;
const uint32_t CameraGroupSky = 4;

const char *xrResultName(XrInstance instance, XrResult result) {
    static thread_local char buffer[XR_MAX_RESULT_STRING_SIZE];
    if ((instance == XR_NULL_HANDLE) || XR_FAILED(xrResultToString(instance, result, buffer))) {
        snprintf(buffer, sizeof(buffer), "%d", int(result));
    }

    return buffer;
}

const char *sceneName(int32_t scene) {
    static const char *names[] = { "unknown", "gameplay", "first-person", "cutscene", "transition", "menu", "pause", "arcade", "multiplayer" };
    return ((scene >= 0) && (scene < DK64VR_SCENE_COUNT)) ? names[scene] : "invalid";
}

bool sceneIsStereo(int32_t scene) {
    return (scene == DK64VR_SCENE_GAMEPLAY) || (scene == DK64VR_SCENE_FIRST_PERSON) || (scene == DK64VR_SCENE_CUTSCENE);
}

Mat4 toMat4(const float m[16]) {
    Mat4 r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r.m[i][j] = m[i * 4 + j];
        }
    }

    return r;
}

void fromMat4(const Mat4 &m, float out[16]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            out[i * 4 + j] = m.m[i][j];
        }
    }
}

Pose fromXr(const XrPosef &p) {
    Pose r;
    r.q = { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w };
    r.p = { p.position.x, p.position.y, p.position.z };
    return r;
}

XrPosef toXr(const Pose &p) {
    XrPosef r;
    r.orientation = { p.q.x, p.q.y, p.q.z, p.q.w };
    r.position = { p.p.x, p.p.y, p.p.z };
    return r;
}

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t format = 0;
    bool bgra = false;
    std::vector<std::unique_ptr<plume::VulkanTexture>> textures;
    std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
    uint32_t index = 0;
    bool acquired = false;
    bool releasedOnce = false;
};

struct Readback {
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint32_t width = 0;
    uint32_t height = 0;
    bool bgra = false;
    bool pending = false;
    std::string path;
};

class XrHost final : public RT64::VrHost {
public:
    // ----------------------------------------------------------------------------------------
    // Install (main thread)

    bool install(const std::filesystem::path &appFolder) {
        dataDir = appFolder;
        std::error_code ec;
        std::filesystem::create_directories(dataDir, ec);
        const std::string settingsPath = (dataDir / "vr.toml").string();
        dk64vr_settings_load(&settings, settingsPath.c_str());
        logSettings();

        uint32_t extensionCount = 0;
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr))) {
            dk64vr_logpf("[openxr]", "no OpenXR runtime; VR mode off");
            return false;
        }

        std::vector<XrExtensionProperties> extensions(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
        xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());
        bool haveVulkan2 = false;
        for (const XrExtensionProperties &e : extensions) {
            haveVulkan2 = haveVulkan2 || (strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0);
            haveRefreshRate = haveRefreshRate || (strcmp(e.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0);
            haveFrameProfile = haveFrameProfile || (strcmp(e.extensionName, "XR_VALVE_frame_controller_interaction") == 0);
        }

        if (!haveVulkan2) {
            dk64vr_logpf("[openxr]", "the runtime lacks %s; VR mode off", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
            return false;
        }

        std::vector<const char *> enabled = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
        if (haveRefreshRate) {
            enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        }

        // The Steam Frame's controller profile, bound first when offered (standard.md section 2).
        if (haveFrameProfile) {
            enabled.push_back("XR_VALVE_frame_controller_interaction");
        }

        XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
        strncpy(createInfo.applicationInfo.applicationName, "DK64VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
        createInfo.applicationInfo.applicationVersion = 1;
        strncpy(createInfo.applicationInfo.engineName, "RT64", XR_MAX_ENGINE_NAME_SIZE - 1);
        createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        createInfo.enabledExtensionCount = uint32_t(enabled.size());
        createInfo.enabledExtensionNames = enabled.data();
        XrResult res = xrCreateInstance(&createInfo, &instance);
        if (XR_FAILED(res)) {
            dk64vr_logpf("[openxr]", "xrCreateInstance failed (%d); VR mode off", int(res));
            return false;
        }

        XrInstanceProperties instanceProps = { XR_TYPE_INSTANCE_PROPERTIES };
        xrGetInstanceProperties(instance, &instanceProps);
        XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        res = xrGetSystem(instance, &systemInfo, &systemId);
        if (XR_FAILED(res)) {
            dk64vr_logpf("[openxr]", "no headset (%s); VR mode off", xrResultName(instance, res));
            xrDestroyInstance(instance);
            instance = XR_NULL_HANDLE;
            return false;
        }

        XrSystemProperties systemProps = { XR_TYPE_SYSTEM_PROPERTIES };
        xrGetSystemProperties(instance, systemId, &systemProps);
        dk64vr_logpf("[openxr]", "runtime %s %u.%u.%u, system %s, refresh-rate extension %s, Frame controller profile %s", instanceProps.runtimeName,
            XR_VERSION_MAJOR(instanceProps.runtimeVersion), XR_VERSION_MINOR(instanceProps.runtimeVersion), XR_VERSION_PATCH(instanceProps.runtimeVersion),
            systemProps.systemName, haveRefreshRate ? "yes" : "no", haveFrameProfile ? "yes" : "no");

        getProc("xrGetVulkanGraphicsRequirements2KHR", getGraphicsRequirements);
        getProc("xrCreateVulkanInstanceKHR", createVulkanInstance);
        getProc("xrGetVulkanGraphicsDevice2KHR", getVulkanGraphicsDevice);
        getProc("xrCreateVulkanDeviceKHR", createVulkanDevice);
        if (haveRefreshRate) {
            getProc("xrEnumerateDisplayRefreshRatesFB", enumerateRefreshRates);
            getProc("xrGetDisplayRefreshRateFB", getRefreshRate);
            getProc("xrRequestDisplayRefreshRateFB", requestRefreshRate);
        }

        if (!getGraphicsRequirements || !createVulkanInstance || !getVulkanGraphicsDevice || !createVulkanDevice) {
            dk64vr_logpf("[openxr]", "the Vulkan binding functions are missing; VR mode off");
            xrDestroyInstance(instance);
            instance = XR_NULL_HANDLE;
            return false;
        }

        XrGraphicsRequirementsVulkan2KHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
        getGraphicsRequirements(instance, systemId, &requirements);
        dk64vr_logpf("[openxr]", "Vulkan API %u.%u to %u.%u", XR_VERSION_MAJOR(requirements.minApiVersionSupported), XR_VERSION_MINOR(requirements.minApiVersionSupported),
            XR_VERSION_MAJOR(requirements.maxApiVersionSupported), XR_VERSION_MINOR(requirements.maxApiVersionSupported));

        plume::VulkanCreateHooks hooks;
        hooks.user = this;
        hooks.createInstance = &XrHost::hookCreateInstance;
        hooks.pickPhysicalDevice = &XrHost::hookPickPhysicalDevice;
        hooks.createDevice = &XrHost::hookCreateDevice;
        hooks.deviceCreated = &XrHost::hookDeviceCreated;
        plume::SetVulkanCreateHooks(hooks);
        RT64::SetVrHost(this);
        if (setting("timing") != 0.0) {
            static const char *stageNames[] = { "wait", "frame", "end" };
            sfvr_perf_init(&perf, 10.0, stageNames, 3);
        }

        watchdogStart(3.0);
        installed = true;
        return true;
    }

    void shutdown() {
        watchdogStop();
        RT64::SetVrHost(nullptr);
        if (instance != XR_NULL_HANDLE) {
            xrDestroyInstance(instance);
            instance = XR_NULL_HANDLE;
        }

        dk64vr_settings_free(&settings);
    }

    void filterInput(int player, uint16_t *buttons, float *x, float *y) {
        if ((player != 0) || (buttons == nullptr) || (x == nullptr) || (y == nullptr)) {
            return;
        }

        // The controllers as the N64 pad, OR-merged with any real pad (never key injection).
        // recompinput already blanks the real pad while a recomp menu has the input.
        InputResult vr;
        {
            std::scoped_lock lock(inputMutex);
            vr = latestInput;
        }

        if (!recompinput::game_input_disabled()) {
            mergeN64(*buttons, *x, *y, vr);
        }

        const double now = monotonicSeconds();
        if (!autostartBegun && (setting("diag_autostart") > 0.0)) {
            autostartBegun = true;
            autostart.start(now, setting("diag_autostart"));
        }

        if (autostart.active()) {
            const dk64vr_game_state state = gameState();
            if ((state.frame != 0) && (state.scene == DK64VR_SCENE_GAMEPLAY)) {
                if (gameplaySinceS == 0.0) {
                    gameplaySinceS = now;
                }
                else if ((now - gameplaySinceS) > 3.0) {
                    autostart.stop("in gameplay");
                }
            }
            else {
                gameplaySinceS = 0.0;
            }

            *buttons |= autostart.buttons(now);
        }
    }

    // ----------------------------------------------------------------------------------------
    // plume hooks

    static VkResult hookCreateInstance(void *user, const VkInstanceCreateInfo *createInfo, VkInstance *vkInstance) {
        XrHost *self = static_cast<XrHost *>(user);
        XrVulkanInstanceCreateInfoKHR info = { XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
        info.systemId = self->systemId;
        info.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        info.vulkanCreateInfo = createInfo;
        VkResult vkResult = VK_ERROR_INITIALIZATION_FAILED;
        XrResult res = self->createVulkanInstance(self->instance, &info, vkInstance, &vkResult);
        if (XR_FAILED(res)) {
            dk64vr_logpf("[openxr]", "xrCreateVulkanInstanceKHR failed (%s, VkResult %d)", xrResultName(self->instance, res), int(vkResult));
            return (vkResult != VK_SUCCESS) ? vkResult : VK_ERROR_INITIALIZATION_FAILED;
        }

        dk64vr_logpf("[openxr]", "Vulkan instance created through the runtime (API %u.%u)", VK_VERSION_MAJOR(createInfo->pApplicationInfo->apiVersion), VK_VERSION_MINOR(createInfo->pApplicationInfo->apiVersion));
        return vkResult;
    }

    static VkPhysicalDevice hookPickPhysicalDevice(void *user, VkInstance vkInstance) {
        XrHost *self = static_cast<XrHost *>(user);
        XrVulkanGraphicsDeviceGetInfoKHR info = { XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
        info.systemId = self->systemId;
        info.vulkanInstance = vkInstance;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        XrResult res = self->getVulkanGraphicsDevice(self->instance, &info, &physicalDevice);
        if (XR_FAILED(res)) {
            dk64vr_logpf("[openxr]", "xrGetVulkanGraphicsDevice2KHR failed (%s)", xrResultName(self->instance, res));
            return VK_NULL_HANDLE;
        }

        return physicalDevice;
    }

    static VkResult hookCreateDevice(void *user, VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *createInfo, VkDevice *device) {
        XrHost *self = static_cast<XrHost *>(user);
        XrVulkanDeviceCreateInfoKHR info = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
        info.systemId = self->systemId;
        info.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        info.vulkanPhysicalDevice = physicalDevice;
        info.vulkanCreateInfo = createInfo;
        VkResult vkResult = VK_ERROR_INITIALIZATION_FAILED;
        XrResult res = self->createVulkanDevice(self->instance, &info, device, &vkResult);
        if (XR_FAILED(res)) {
            dk64vr_logpf("[openxr]", "xrCreateVulkanDeviceKHR failed (%s, VkResult %d)", xrResultName(self->instance, res), int(vkResult));
            return (vkResult != VK_SUCCESS) ? vkResult : VK_ERROR_INITIALIZATION_FAILED;
        }

        return vkResult;
    }

    static void hookDeviceCreated(void *user, plume::VulkanDevice *device) {
        XrHost *self = static_cast<XrHost *>(user);
        self->device = device;
        self->queueFamily = device->queueFamilyIndices[0];  // DIRECT
        self->queueMutex = plume::GetVulkanQueueMutex(device, self->queueFamily, 0);
        dk64vr_logpf("[openxr]", "device %s, queue family %u queue 0 shared with the runtime", device->description.name.c_str(), self->queueFamily);
    }

    // ----------------------------------------------------------------------------------------
    // RT64::VrHost: workload thread

    RT64::VrFrame beginFrame() override {
        watchdogBeat("begin frame");
        frameStartS = monotonicSeconds();
        RT64::VrFrame frame;
        pollEvents();
        if ((session == XR_NULL_HANDLE) && (device != nullptr) && !sessionFailed && !exitRequested) {
            createSession();
        }

        if (!sessionRunning) {
            watchdogArm(false);
            frameMode = RT64::VrFrame::Mode::None;
            return frame;
        }

        watchdogArm(true);
        XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
        frameState = { XR_TYPE_FRAME_STATE };
        XrResult res = xrWaitFrame(session, &waitInfo, &frameState);
        if (XR_FAILED(res)) {
            logOnce(waitFailed, "xrWaitFrame failed (%s)", xrResultName(instance, res));
            frameMode = RT64::VrFrame::Mode::None;
            return frame;
        }

        watchdogBeat("after xrWaitFrame");
        waitDoneS = monotonicSeconds();
        {
            std::scoped_lock lock(*queueMutex);
            XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
            res = xrBeginFrame(session, &beginInfo);
        }

        if (XR_FAILED(res)) {
            logOnce(beginFailed, "xrBeginFrame failed (%s)", xrResultName(instance, res));
            frameMode = RT64::VrFrame::Mode::None;
            return frame;
        }

        frameBegun = true;
        frameCount++;
        displayS = double(frameState.predictedDisplayTime) * 1e-9;
        periodS = double(frameState.predictedDisplayPeriod) * 1e-9;
        scheduler.period_s = periodS;
        missed = (lastDisplayS > 0.0) && ((displayS - lastDisplayS) > 1.5 * periodS);
        lastDisplayS = displayS;
        logRateOnce();

        const bool forceRender = (setting("force_render") != 0.0);
        rendering = frameState.shouldRender || forceRender;
        synthetic = forceRender && !frameState.shouldRender;
        eyesRecorded[0] = eyesRecorded[1] = false;
        setPaused(!forceRender && !(focused && frameState.shouldRender), focused ? "headset in standby" : "focus lost");
        if (!rendering) {
            updateInput(false);
            frameMode = RT64::VrFrame::Mode::Flat;
            frame.mode = frameMode;
            frame.flatHeight = PanelHeight;
            return frame;
        }

        locateEyes();
        updateInput(true);
        checkDiagExit();
        const double now = monotonicSeconds();
        probing = (setting("probe_seconds") > 0.0) && ((now - sessionStartS) < setting("probe_seconds"));
        updateStereoLatch();
        if (!probing && stereoLatched) {
            frameMode = RT64::VrFrame::Mode::Stereo;
            frame.eyeWidth = eyeChain[0].width;
            frame.eyeHeight = eyeChain[0].height;
            acquireEyes();
        }
        else {
            frameMode = RT64::VrFrame::Mode::Flat;
        }

        frame.mode = frameMode;
        frame.flatHeight = PanelHeight;
        return frame;
    }

    void workloadArrived(const RT64::VrWorkloadInfo &info) override {
        lastInfo = info;
        const double tick = (info.viOriginalRate > 0) ? (1.0 / double(info.viOriginalRate)) : (1.0 / 30.0);
        const double at = (frameMode != RT64::VrFrame::Mode::None) ? displayS : monotonicSeconds();
        const double tickBefore = scheduler.tick_s;
        if (scheduler.arrive(at, tick) && ((scheduler.rebases == 1) || (std::fabs(tickBefore - scheduler.tick_s) > 1e-9) || (setting("timing") != 0.0))) {
            dk64vr_logf("scheduler: anchored at %.1f Hz (rebase %" PRIu64 ", offset %.1f ms)", 1.0 / scheduler.tick_s, scheduler.rebases, scheduler.last_offset_s * 1000.0);
        }

        if ((info.cameraProjections > 0) != cameraSeen) {
            cameraSeen = (info.cameraProjections > 0);
            dk64vr_logf("camera: %u projection(s) in the main image %08X (%s)", info.cameraProjections, info.mainColorAddress,
                info.cameraFromGroup ? "camera matrix group" : "any perspective");
        }
    }

    void frameWeights(float &curWeight, float &prevWeight) override {
        curWeight = scheduler.weight(displayS);
        prevWeight = scheduler.weight(displayS - periodS);
    }

    bool wantPlayerExtent(float center[3], float &radius) override {
        // Once, a couple of seconds into play, around the player's position.
        if (playerExtentLogged) {
            return false;
        }

        const dk64vr_game_state state = gameState();
        if ((state.frame == 0) || (state.scene != DK64VR_SCENE_GAMEPLAY)) {
            gameplayFrames = 0;
            return false;
        }

        if (++gameplayFrames < 60) {
            return false;
        }

        center[0] = state.player_pos[0];
        center[1] = state.player_pos[1];
        center[2] = state.player_pos[2];
        radius = 40.0f;
        return true;
    }

    void playerExtent(float minY, float maxY, uint32_t vertexCount) override {
        playerExtentLogged = true;
        const dk64vr_game_state state = gameState();
        if (vertexCount == 0) {
            dk64vr_logf("player extent: no camera vertices within 40 units of the player at %.0f %.0f %.0f", state.player_pos[0], state.player_pos[1], state.player_pos[2]);
            return;
        }

        const float worldScale = float(setting("world_scale"));
        dk64vr_logf("player extent: y %.1f to %.1f (%.1f units, %u vertices within 40 units of the player at %.0f %.0f %.0f); %.2f m at world_scale %.0f",
            minY, maxY, maxY - minY, vertexCount, state.player_pos[0], state.player_pos[1], state.player_pos[2], (maxY - minY) / worldScale, worldScale);
    }

    void cameraGroups(uint32_t &primary, uint32_t &secondary) const override {
        primary = CameraGroup;
        secondary = CameraGroupSky;
    }

    void eyeCamera(uint32_t eye, const float view[16], const float proj[16], float outView[16], float outProj[16]) override {
        Mat4 gameView = toMat4(view);
        const Mat4 gameProj = toMat4(proj);
        if (!cameraLogged && (eye == 0)) {
            cameraLogged = true;
            const float s = -gameProj.m[2][3];
            const float fovY = 2.0f * std::atan(s / gameProj.m[1][1]) * 180.0f / 3.1415926f;
            const float nearZ = gameProj.m[3][2] / (gameProj.m[2][2] - s);
            const float farZ = gameProj.m[3][2] / (gameProj.m[2][2] + s);
            dk64vr_logf("camera: game fov %.1f, near %.2f, far %.0f units; world_scale %.1f units/m, level_pitch %d", fovY, nearZ, farZ,
                setting("world_scale"), int(setting("level_pitch")));
        }

        if (setting("level_pitch") != 0.0) {
            gameView = levelView(gameView);
        }

        fromMat4(eyeView(gameView, eyeInHead[eye], float(setting("world_scale"))), outView);
        fromMat4(eyeProjection(eyeFov[eye], gameProj), outProj);
    }

    void recordEye(uint32_t eye, plume::RenderCommandList *commandList, plume::RenderTexture *source, uint32_t width, uint32_t height) override {
        Swapchain &chain = eyeChain[eye];
        if (!chain.acquired || (eye > 1)) {
            return;
        }

        plume::VulkanTexture *image = chain.textures[chain.index].get();
        const uint32_t copyWidth = std::min(width, chain.width);
        const uint32_t copyHeight = std::min(height, chain.height);
        const plume::RenderTextureBarrier toCopy[] = {
            plume::RenderTextureBarrier(source, plume::RenderTextureLayout::COPY_SOURCE),
            plume::RenderTextureBarrier(image, plume::RenderTextureLayout::COPY_DEST)
        };
        commandList->barriers(plume::RenderBarrierStage::COPY, toCopy, 2);

        const plume::RenderBox box(0, 0, int32_t(copyWidth), int32_t(copyHeight));
        commandList->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(image), plume::RenderTextureCopyLocation::Subresource(source), 0, 0, 0, &box);
        commandList->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(source, plume::RenderTextureLayout::SHADER_READ));
        if (dumpDue) {
            recordReadback(commandList, image, chain, eyeReadback[eye], "vr-eye" + std::to_string(eye));
        }

        commandList->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image, plume::RenderTextureLayout::COLOR_WRITE));
        eyesRecorded[eye] = true;
    }

    void eyeDone(uint32_t eye, double cpuMs, double gpuMs) override {
        if (eye < 2) {
            eyeCpuMs[eye] = cpuMs;
            eyeGpuMs[eye] = gpuMs;
            finishReadback(eyeReadback[eye]);
        }
    }

    void endFrame() override {
        watchdogBeat("end frame");
        if (!frameBegun) {
            return;
        }

        const double endStartS = monotonicSeconds();
        frameBegun = false;
        std::vector<const XrCompositionLayerBaseHeader *> layers;
        XrCompositionLayerProjection projection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        XrCompositionLayerProjectionView views[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        bool stereoShown = false;
        if (rendering) {
            stereoShown = (frameMode == RT64::VrFrame::Mode::Stereo) && eyesRecorded[0] && eyesRecorded[1];
            if (!stereoShown) {
                // The probe colours, or black under the panel (lessons 56 and 82: always submit a
                // projection layer). Black needs drawing only until every image of both eye
                // swapchains is black; after that the layer shows the last released image.
                const bool probe = probing;
                if (probe || (blackImages < blackNeeded()) || eyeChain[0].acquired || eyeChain[1].acquired) {
                    clearEyes(probe ? plume::RenderColor(0.85f, 0.15f, 0.1f, 1.0f) : plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f),
                        probe ? plume::RenderColor(0.1f, 0.3f, 0.9f, 1.0f) : plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f), probe);
                    blackImages = probe ? 0 : (blackImages + 1);
                }
            }
            else {
                blackImages = 0;
                if (dumpDue) {
                    dumpsDone++;
                    dumpDue = false;
                    nextDumpFrame = stereoFrames + uint64_t(setting("dump_interval"));
                    diagYawDeg += float(setting("diag_yaw_step"));
                }
            }

            releaseEyes();
            for (uint32_t eye = 0; eye < 2; eye++) {
                views[eye].pose = toXr(eyeSubmitPose[eye]);
                views[eye].fov = eyeXrFov[eye];
                views[eye].subImage.swapchain = eyeChain[eye].handle;
                views[eye].subImage.imageRect.offset = { 0, 0 };
                views[eye].subImage.imageRect.extent = { int32_t(eyeChain[eye].width), int32_t(eyeChain[eye].height) };
                views[eye].subImage.imageArrayIndex = 0;
            }

            projection.space = localSpace;
            projection.viewCount = 2;
            projection.views = views;
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection));

            const bool panelShown = !stereoShown && !probing;
            if (panelShown && panelReady.load()) {
                const float width = float(setting("panel_width"));
                Pose panelPose = origin;
                panelPose = poseMul(origin, Pose { Quat(), { 0.0f, 0.0f, -float(setting("panel_distance")) } });
                quad.space = localSpace;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = panelChain.handle;
                quad.subImage.imageRect.offset = { 0, 0 };
                quad.subImage.imageRect.extent = { int32_t(panelChain.width), int32_t(panelChain.height) };
                quad.pose = toXr(panelPose);
                quad.size = { width, width * float(panelChain.height) / float(panelChain.width) };
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad));
            }

            if (stereoShown) {
                stereoFrames++;
            }

            panelWanted = !stereoShown;
            panelShownLast = !stereoShown && !probing && panelReady.load();
        }

        XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = uint32_t(layers.size());
        endInfo.layers = layers.data();
        XrResult res;
        {
            std::scoped_lock lock(*queueMutex);
            res = xrEndFrame(session, &endInfo);
        }

        if (XR_FAILED(res)) {
            logOnce(endFailed, "xrEndFrame failed (%s)", xrResultName(instance, res));
        }

        const double nowS = monotonicSeconds();
        if (setting("timing") != 0.0) {
            const float stages[3] = { float((waitDoneS - frameStartS) * 1000.0), float((endStartS - waitDoneS) * 1000.0), float((nowS - endStartS) * 1000.0) };
            const float eyes[2] = { stereoShown ? float(eyeCpuMs[0]) : 0.0f, stereoShown ? float(eyeCpuMs[1]) : 0.0f };
            const float gpu = stereoShown ? float(eyeGpuMs[0] + eyeGpuMs[1]) : -1.0f;
            sfvr_perf_add_frame(&perf, nowS, missed ? 1 : 0, stages, eyes, gpu);
            char line[256];
            if (sfvr_perf_poll(&perf, nowS, line, sizeof(line))) {
                const double windowS = nowS - perfWindowStartS;
                printf("%s mode=%s rate=%.1fHz eye=%ux%u game=%.1fHz rebases=%" PRIu64 "\n", line, stereoShown ? "stereo" : "flat", (periodS > 0.0) ? (1.0 / periodS) : 0.0,
                    eyeChain[0].width, eyeChain[0].height, (windowS > 0.0) ? double(scheduler.arrivals - perfWindowArrivals) / windowS : 0.0, scheduler.rebases - perfWindowRebases);
                perfWindowStartS = nowS;
                perfWindowArrivals = scheduler.arrivals;
                perfWindowRebases = scheduler.rebases;
                fflush(stdout);
            }
        }
    }

    // ----------------------------------------------------------------------------------------
    // RT64::VrHost: present thread

    plume::RenderFramebuffer *panelAcquire(plume::RenderTexture **texture, uint32_t *width, uint32_t *height) override {
        panelMutex.lock();
        // Not while the eyes are shown: the panel would cost GPU time and workload-mutex time
        // for nothing, and the runtime need not free images of a swapchain no layer uses.
        if ((panelChain.handle == XR_NULL_HANDLE) || !sessionRunning || !panelWanted.load()) {
            panelMutex.unlock();
            return nullptr;
        }

        XrResult res = acquireImage(panelChain);

        if (XR_FAILED(res)) {
            logOnce(panelFailed, "panel image acquire failed (%s)", xrResultName(instance, res));
            panelMutex.unlock();
            return nullptr;
        }

        panelChain.acquired = true;
        plume::VulkanTexture *image = panelChain.textures[panelChain.index].get();
        image->textureLayout = plume::RenderTextureLayout::COLOR_WRITE;
        image->barrierStages = plume::RenderBarrierStage::GRAPHICS;
        *texture = image;
        *width = panelChain.width;
        *height = panelChain.height;
        return panelChain.framebuffers[panelChain.index].get();
    }

    void panelRelease() override {
        if (!panelChain.acquired) {
            panelMutex.unlock();
            return;
        }

        panelPresents++;
        // Every dump_panel_frame-th present, up to 40, for a timeline of the flat screens.
        const uint64_t dumpPanel = uint64_t(setting("dump_panel_frame"));
        if ((dumpPanel > 0) && ((panelPresents % dumpPanel) == 0) && (panelPresents / dumpPanel <= 40) && panelWorker) {
            plume::VulkanTexture *image = panelChain.textures[panelChain.index].get();
            panelWorker->commandList->begin();
            recordReadback(panelWorker->commandList.get(), image, panelChain, panelReadback, "vr-panel-p" + std::to_string(panelPresents / dumpPanel));
            panelWorker->commandList->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image, plume::RenderTextureLayout::COLOR_WRITE));
            panelWorker->commandList->end();
            panelWorker->execute();
            panelWorker->wait();
            finishReadback(panelReadback);
        }

        {
            std::scoped_lock lock(*queueMutex);
            XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(panelChain.handle, &releaseInfo);
        }

        panelChain.acquired = false;
        panelReady = true;
        panelMutex.unlock();
    }

    // Game thread: the game's Rumble Pak motor (ultramodern's set_rumble), as haptics.
    void setRumble(int channel, bool on) {
        if (channel == 0) {
            rumbleOn = on;
        }
    }

    void threadExit() override {
        watchdogArm(false);
        dk64vr_set_paused(0);
        dk64vr_set_active(0);
        input.destroy();
        destroySession();
    }

private:
    static const uint32_t PanelWidth = 1920;
    static const uint32_t PanelHeight = 1080;

    template<typename T>
    void getProc(const char *name, T &fn) {
        PFN_xrVoidFunction p = nullptr;
        if (XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, &p))) {
            fn = reinterpret_cast<T>(p);
        }
    }

    double setting(const char *name) const {
        return dk64vr_settings_get(&settings, name);
    }

    template<typename... Args>
    void logOnce(bool &flag, const char *fmt, Args... args) {
        if (!flag) {
            flag = true;
            dk64vr_logpf("[openxr]", fmt, args...);
        }
    }

    dk64vr_game_state gameState() const {
        dk64vr_game_state state;
        dk64vr_get_game_state(&state);
        return state;
    }

    void logSettings() {
        dk64vr_logf("settings: world_scale %.1f, resolution_scale %.2f, refresh_rate %d, level_pitch %d, force_render %d, diag_yaw %.0f, dump_frame %d, probe_seconds %.1f, diag_autostart %.0f, diag_exit_seconds %.0f, diag_force_stereo %d, timing %d",
            setting("world_scale"), setting("resolution_scale"), int(setting("refresh_rate")), int(setting("level_pitch")), int(setting("force_render")),
            setting("diag_yaw"), int(setting("dump_frame")), setting("probe_seconds"), setting("diag_autostart"), setting("diag_exit_seconds"),
            int(setting("diag_force_stereo")), int(setting("timing")));
        diagYawDeg = float(setting("diag_yaw"));
        nextDumpFrame = uint64_t(setting("dump_frame"));
    }

    // ----------------------------------------------------------------------------------------
    // Session

    void createSession() {
        XrGraphicsBindingVulkan2KHR binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
        binding.instance = device->renderInterface->instance;
        binding.physicalDevice = device->physicalDevice;
        binding.device = device->vk;
        binding.queueFamilyIndex = queueFamily;
        binding.queueIndex = 0;
        XrSessionCreateInfo createInfo = { XR_TYPE_SESSION_CREATE_INFO };
        createInfo.next = &binding;
        createInfo.systemId = systemId;
        XrResult res = xrCreateSession(instance, &createInfo, &session);
        if (XR_FAILED(res)) {
            dk64vr_logpf("[openxr]", "xrCreateSession failed (%s); VR output off", xrResultName(instance, res));
            sessionFailed = true;
            session = XR_NULL_HANDLE;
            return;
        }

        XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        xrCreateReferenceSpace(session, &spaceInfo, &localSpace);
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        xrCreateReferenceSpace(session, &spaceInfo, &viewSpace);

        uint32_t viewCount = 0;
        xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
        std::vector<XrViewConfigurationView> configViews(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
        xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, configViews.data());
        if (viewCount < 2) {
            dk64vr_logpf("[openxr]", "the runtime has %u stereo views; VR output off", viewCount);
            sessionFailed = true;
            return;
        }

        const double scale = setting("resolution_scale");
        uint32_t eyeSize[2];
        const uint32_t recommended[2] = { configViews[0].recommendedImageRectWidth, configViews[0].recommendedImageRectHeight };
        const uint32_t maximum[2] = { configViews[0].maxImageRectWidth, configViews[0].maxImageRectHeight };
        for (int i = 0; i < 2; i++) {
            uint32_t v = uint32_t(std::lround(double(recommended[i]) * scale));
            v = std::min(v, maximum[i]);
            v &= ~1U;
            eyeSize[i] = std::max(v, 2U);
        }

        uint32_t formatCount = 0;
        xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr);
        std::vector<int64_t> formats(formatCount);
        xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());
        std::string formatList;
        for (int64_t f : formats) {
            formatList += std::to_string(f) + " ";
        }

        auto has = [&](VkFormat f) { return std::find(formats.begin(), formats.end(), int64_t(f)) != formats.end(); };
        dk64vr_logpf("[openxr]", "recommended eye %ux%u (max %ux%u), using %ux%u (resolution_scale %.2f); swapchain formats: %s",
            recommended[0], recommended[1], maximum[0], maximum[1], eyeSize[0], eyeSize[1], scale, formatList.c_str());

        // RT64's main color image is RGBA8 UNORM holding gamma-encoded colour: copied raw into an
        // sRGB image it is decoded once by the compositor (lessons 8, 42).
        const bool dumps = (setting("dump_frame") > 0.0) || (setting("dump_panel_frame") > 0.0);
        if (has(VK_FORMAT_R8G8B8A8_SRGB)) {
            for (int eye = 0; eye < 2; eye++) {
                createSwapchain(eyeChain[eye], eyeSize[0], eyeSize[1], VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, plume::RenderFormat::R8G8B8A8_UNORM, false, dumps);
            }
        }
        else {
            dk64vr_logpf("[openxr]", "no R8G8B8A8_SRGB swapchain format; VR output off");
            sessionFailed = true;
            return;
        }

        // The panel is drawn by RT64's VI renderer and the UI, whose pipelines are BGRA8 UNORM.
        if (has(VK_FORMAT_B8G8R8A8_SRGB)) {
            std::scoped_lock lock(panelMutex);
            createSwapchain(panelChain, PanelWidth, PanelHeight, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, plume::RenderFormat::B8G8R8A8_UNORM, true, dumps);
        }
        else {
            dk64vr_logpf("[openxr]", "no B8G8R8A8_SRGB swapchain format: no flat panel");
        }

        input.create(instance, session, haveFrameProfile);
        frameWorker = std::make_unique<RT64::RenderWorker>(device, "VR Frame", plume::RenderCommandListType::DIRECT);
        panelWorker = std::make_unique<RT64::RenderWorker>(device, "VR Panel", plume::RenderCommandListType::DIRECT);
        dk64vr_logpf("[openxr]", "session created; eye swapchains %ux%u format %d, panel %ux%u", eyeChain[0].width, eyeChain[0].height, int(eyeChain[0].format), panelChain.width, panelChain.height);
    }

    void createSwapchain(Swapchain &chain, uint32_t width, uint32_t height, VkFormat format, VkFormat viewFormat, plume::RenderFormat renderFormat, bool bgra, bool transferSource) {
        XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        if (transferSource) {
            info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        }

        info.format = format;
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        XrResult res = xrCreateSwapchain(session, &info, &chain.handle);
        if (XR_FAILED(res)) {
            dk64vr_logpf("[openxr]", "xrCreateSwapchain %ux%u failed (%s)", width, height, xrResultName(instance, res));
            chain.handle = XR_NULL_HANDLE;
            return;
        }

        chain.width = width;
        chain.height = height;
        chain.format = format;
        chain.bgra = bgra;
        uint32_t count = 0;
        xrEnumerateSwapchainImages(chain.handle, 0, &count, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> images(count, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
        xrEnumerateSwapchainImages(chain.handle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()));
        for (uint32_t i = 0; i < count; i++) {
            auto texture = std::make_unique<plume::VulkanTexture>(device, images[i].image);
            texture->desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
            texture->desc.format = renderFormat;
            texture->desc.width = width;
            texture->desc.height = height;
            texture->desc.depth = 1;
            texture->desc.mipLevels = 1;
            texture->desc.arraySize = 1;
            texture->desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
            texture->fillSubresourceRange();
            texture->createImageView(viewFormat);
            const plume::RenderTexture *attachment = texture.get();
            chain.framebuffers.emplace_back(device->createFramebuffer(plume::RenderFramebufferDesc(&attachment, 1)));
            chain.textures.emplace_back(std::move(texture));
        }
    }

    void destroySwapchain(Swapchain &chain) {
        if (chain.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(chain.handle);
        }

        chain.framebuffers.clear();
        chain.textures.clear();
        chain = Swapchain();
    }

    void destroySession() {
        if (session == XR_NULL_HANDLE) {
            return;
        }

        if (device != nullptr) {
            // Nothing of ours may still run on the GPU when the images go away.
            vkDeviceWaitIdle(device->vk);
        }

        {
            std::scoped_lock lock(panelMutex);
            destroySwapchain(panelChain);
            panelReady = false;
        }

        destroySwapchain(eyeChain[0]);
        destroySwapchain(eyeChain[1]);
        eyeReadback[0].buffer.reset();
        eyeReadback[1].buffer.reset();
        panelReadback.buffer.reset();
        frameWorker.reset();
        panelWorker.reset();
        if (localSpace != XR_NULL_HANDLE) {
            xrDestroySpace(localSpace);
        }

        if (viewSpace != XR_NULL_HANDLE) {
            xrDestroySpace(viewSpace);
        }

        localSpace = viewSpace = XR_NULL_HANDLE;
        xrDestroySession(session);
        session = XR_NULL_HANDLE;
        sessionRunning = false;
        dk64vr_logpf("[openxr]", "session destroyed");
    }

    void pollEvents() {
        XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto &changed = *reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
                onSessionState(changed.state);
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                recentrePending = true;
                dk64vr_logf("recentre: the runtime's reference space changed");
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                dk64vr_logpf("[openxr]", "instance loss pending");
                quitOnce("the runtime is going away");
                break;
            case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                const auto &changed = *reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB *>(&event);
                dk64vr_logpf("[openxr]", "display refresh rate %.1f -> %.1f Hz", changed.fromDisplayRefreshRate, changed.toDisplayRefreshRate);
                break;
            }
            default:
                break;
            }

            event = { XR_TYPE_EVENT_DATA_BUFFER };
        }
    }

    void onSessionState(XrSessionState state) {
        static const char *names[] = { "unknown", "idle", "ready", "synchronized", "visible", "focused", "stopping", "loss pending", "exiting" };
        dk64vr_logpf("[openxr]", "session %s", (int(state) >= 0 && int(state) <= 8) ? names[state] : "?");
        focused = (state == XR_SESSION_STATE_FOCUSED);
        switch (state) {
        case XR_SESSION_STATE_READY: {
            if (sessionFailed) {
                break;
            }

            XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XrResult res = xrBeginSession(session, &beginInfo);
            if (XR_SUCCEEDED(res)) {
                sessionRunning = true;
                sessionStartS = monotonicSeconds();
                perfWindowStartS = sessionStartS;
                recentrePending = true;
                scheduler.reset();
                requestRateOnce();
                dk64vr_set_active(1);
            }
            else {
                dk64vr_logpf("[openxr]", "xrBeginSession failed (%s)", xrResultName(instance, res));
            }

            break;
        }
        case XR_SESSION_STATE_STOPPING:
            sessionRunning = false;
            input.stopHaptics();
            dk64vr_set_active(0);
            xrEndSession(session);
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            sessionRunning = false;
            setPaused(false, "the session is ending");
            dk64vr_set_active(0);
            quitOnce("the runtime ended the session");
            break;
        default:
            break;
        }
    }

    void quitOnce(const char *reason) {
        if (!exitRequested.exchange(true)) {
            std::string why = reason;
            std::thread([why]() { requestQuit(why.c_str()); }).detach();
        }
    }

    void requestRateOnce() {
        if (!enumerateRefreshRates || rateRequested) {
            return;
        }

        rateRequested = true;
        uint32_t count = 0;
        enumerateRefreshRates(session, 0, &count, nullptr);
        std::vector<float> rates(count);
        enumerateRefreshRates(session, count, &count, rates.data());
        float current = 0.0f;
        getRefreshRate(session, &current);
        std::string list;
        float best = 0.0f;
        const float want = float(setting("refresh_rate"));
        for (float r : rates) {
            list += std::to_string(int(std::lround(r))) + " ";
            if ((best == 0.0f) || (std::fabs(r - want) < std::fabs(best - want))) {
                best = r;
            }
        }

        if ((best > 0.0f) && (std::fabs(best - current) > 0.5f)) {
            XrResult res = requestRefreshRate(session, best);
            dk64vr_logpf("[openxr]", "refresh rates offered: %scurrent %.1f Hz, requested %.1f Hz (%s)", list.c_str(), current, best, xrResultName(instance, res));
        }
        else {
            dk64vr_logpf("[openxr]", "refresh rates offered: %scurrent %.1f Hz, wanted %.0f Hz (not requested)", list.c_str(), current, want);
        }
    }

    void logRateOnce() {
        if (!rateLogged && (frameCount == 240)) {
            rateLogged = true;
            float current = 0.0f;
            if (getRefreshRate) {
                getRefreshRate(session, &current);
            }

            dk64vr_logpf("[openxr]", "predictedDisplayPeriod %.3f ms (%.1f Hz), FB rate %.1f Hz", periodS * 1000.0, (periodS > 0.0) ? (1.0 / periodS) : 0.0, current);
        }
    }

    void checkDiagExit() {
        const double seconds = setting("diag_exit_seconds");
        if ((seconds > 0.0) && ((monotonicSeconds() - sessionStartS) > seconds)) {
            quitOnce("diag_exit_seconds");
        }
    }

    // ----------------------------------------------------------------------------------------
    // Poses

    void locateEyes() {
        XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = localSpace;
        XrViewState viewState = { XR_TYPE_VIEW_STATE };
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        uint32_t count = 0;
        xrLocateViews(session, &locateInfo, &viewState, 2, &count, views);
        XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
        xrLocateSpace(viewSpace, localSpace, frameState.predictedDisplayTime, &head);
        const XrViewStateFlags needed = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        const bool viewsValid = (count == 2) && ((viewState.viewStateFlags & needed) == needed);
        const bool headValid = (head.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT)) == (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT);

        // FOV from the runtime; the Frame's measured left-eye FOV (platform.md) when it gives none.
        for (uint32_t eye = 0; eye < 2; eye++) {
            XrFovf fov = views[eye].fov;
            if (!(fov.angleRight - fov.angleLeft > 0.1f) || !(fov.angleUp - fov.angleDown > 0.1f)) {
                fov = (eye == 0) ? XrFovf { -1.028f, 0.885f, 0.864f, -1.048f } : XrFovf { -0.885f, 1.028f, 0.864f, -1.048f };
                logOnce(fovFallback, "the runtime gave no eye FOV; using the Frame's measured one");
            }

            eyeXrFov[eye] = fov;
            eyeFov[eye].left = std::tan(fov.angleLeft);
            eyeFov[eye].right = std::tan(fov.angleRight);
            eyeFov[eye].up = std::tan(fov.angleUp);
            eyeFov[eye].down = std::tan(fov.angleDown);
        }

        // Eye poses relative to the head: from the runtime's views (IPD is never a constant),
        // except when it has none at all.
        Pose eyeFromHead[2];
        const Pose headPose = headValid ? fromXr(head.pose) : Pose();
        if (synthetic) {
            // A synthetic head looks straight ahead with level eyes the runtime's distance apart.
            float ipd = 0.063f;
            if (viewsValid) {
                const XrVector3f a = views[0].pose.position, b = views[1].pose.position;
                ipd = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
            }

            eyeFromHead[0].p = { -0.5f * ipd, 0.0f, 0.0f };
            eyeFromHead[1].p = { 0.5f * ipd, 0.0f, 0.0f };
        }
        else if (viewsValid && headValid) {
            for (uint32_t eye = 0; eye < 2; eye++) {
                eyeFromHead[eye] = poseMul(poseInverse(headPose), fromXr(views[eye].pose));
            }
        }
        else {
            logOnce(ipdFallback, "no eye poses from the runtime; a synthetic head with eyes 63 mm apart");
            eyeFromHead[0].p = { -0.0315f, 0.0f, 0.0f };
            eyeFromHead[1].p = { 0.0315f, 0.0f, 0.0f };
        }

        if (!ipdLogged && viewsValid && headValid) {
            ipdLogged = true;
            const Vec3 a = eyeFromHead[0].p, b = eyeFromHead[1].p;
            dk64vr_logf("eyes: %.1f mm apart; left fov L %.3f R %.3f U %.3f D %.3f rad", 1000.0f * std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)),
                eyeXrFov[0].angleLeft, eyeXrFov[0].angleRight, eyeXrFov[0].angleUp, eyeXrFov[0].angleDown);
        }

        // The head in the game camera's frame. A synthetic head (force_render in standby) looks
        // straight ahead from the origin.
        Pose headInOrigin;
        if (synthetic || !headValid) {
            origin = Pose();
        }
        else {
            if (recentrePending) {
                recentrePending = false;
                origin.q = quatFromYaw(quatYaw(headPose.q));
                origin.p = headPose.p;
                dk64vr_logf("recentre: head yaw %.1f deg, at %.2f %.2f %.2f m", quatYaw(headPose.q) * 180.0f / 3.1415926f, headPose.p.x, headPose.p.y, headPose.p.z);
            }

            headInOrigin = poseMul(poseInverse(origin), headPose);
        }

        Pose yaw;
        yaw.q = quatFromYaw(diagYawDeg * 3.1415926f / 180.0f);
        headInOrigin = poseMul(yaw, headInOrigin);
        headInOriginQ = headInOrigin.q;
        for (uint32_t eye = 0; eye < 2; eye++) {
            eyeInHead[eye] = poseMul(headInOrigin, eyeFromHead[eye]);
            eyeSubmitPose[eye] = poseMul(origin, eyeInHead[eye]);
        }

        dumpDue = (nextDumpFrame > 0) && (dumpsDone < uint64_t(setting("dump_count"))) && (stereoFrames + 1 >= nextDumpFrame);
    }

    void updateStereoLatch() {
        const dk64vr_game_state state = gameState();
        const bool published = (state.frame != 0);
        if (published && (state.scene != lastScene)) {
            dk64vr_logf("scene: %s -> %s", sceneName(lastScene), sceneName(state.scene));
            lastScene = state.scene;
        }

        bool want;
        const char *why;
        const bool uiWanted = uiCapturing || (monotonicSeconds() < uiRequestUntilS);
        if (uiWanted) {
            // A recomp menu (or a toggle on its way): the panel at once, no hysteresis.
            if (stereoLatched) {
                stereoLatched = false;
                latchCount = 0;
                dk64vr_logf("mode: panel (recomp menu)");
            }

            return;
        }
        else if (setting("diag_force_stereo") != 0.0) {
            want = cameraSeen;
            why = "diag_force_stereo";
        }
        else if (published) {
            want = sceneIsStereo(state.scene) && cameraSeen;
            why = sceneName(state.scene);
        }
        else {
            want = false;
            why = "no game state";
        }

        // Hysteresis: the scene can reach us a frame early or late, so a change must hold.
        if (want != stereoLatched) {
            if (++latchCount >= 12) {
                stereoLatched = want;
                latchCount = 0;
                dk64vr_logf("mode: %s (%s, %u camera projection(s))", want ? "stereo" : "panel", why, lastInfo.cameraProjections);
            }
        }
        else {
            latchCount = 0;
        }
    }


    // ----------------------------------------------------------------------------------------
    // Input (workload thread, once per display frame)

    void setPaused(bool paused, const char *why) {
        if (paused != gamePaused) {
            gamePaused = paused;
            dk64vr_set_paused(paused ? 1 : 0);
            dk64vr_logf("game %s (%s)", paused ? "frozen" : "running", paused ? why : "focused and rendering");
            if (paused) {
                input.stopHaptics();
            }
        }
    }

    InputConfig inputConfig() const {
        InputConfig c;
        c.rightStickX = (setting("right_stick_x") != 0.0) ? RightStickX::GameCamera : RightStickX::Turn;
        c.turnMode = TurnMode(int(setting("turn_mode")));
        c.snapAngleDeg = float(setting("snap_angle"));
        c.smoothSpeedDps = float(setting("smooth_turn_speed"));
        c.moveFromView = (setting("move_from_view") != 0.0);
        return c;
    }

    int dominantHand() const {
        return (setting("dominant_hand") != 0.0) ? 0 : 1;  // 0 left, 1 right
    }

    // Yaw of the movement reference (move_relative: head, left or right hand) relative to the
    // game camera's heading, which is the recentred origin's forward.
    float moveYawRad() const {
        const int relative = int(setting("move_relative"));
        if (relative != 0) {
            Pose aim;
            if (input.aimPose((relative == 1) ? 0 : 1, aim)) {
                return quatYaw(quatMul(quatConj(origin.q), aim.q));
            }
        }

        return quatYaw(headInOriginQ);
    }

    void updateInput(bool renderingNow) {
        const bool live = renderingNow && focused && input.ready() && (synthetic == false);
        const InputSample s = input.sample(live, localSpace, frameState.predictedDisplayTime);
        uiCapturing = recompui::is_context_capturing_input();
        uiMouse = uiCapturing && recompui::is_context_capturing_mouse();
        const dk64vr_game_state state = gameState();
        InputContext context;
        context.menuOpen = uiCapturing;
        context.stereoPlay = stereoLatched && !cinemaActive && ((state.scene == DK64VR_SCENE_GAMEPLAY) || (state.scene == DK64VR_SCENE_FIRST_PERSON));
        context.viewYawRad = moveYawRad();
        const float dt = float(std::min(periodS, 0.1));
        const InputResult r = mapper.update(s, inputConfig(), context, displayS, dt);
        if (s.valid && !inputLogged) {
            inputLogged = true;
            dk64vr_logf("input: first live sample (right_stick_x %s, turn_mode %d, snap %d, move_from_view %d)",
                (setting("right_stick_x") != 0.0) ? "camera" : "turn", int(setting("turn_mode")), int(setting("snap_angle")), int(setting("move_from_view")));
        }

        if (r.recentre) {
            recentrePending = true;
            dk64vr_logf("recentre: View held");
        }

        if (r.systemBuzz) {
            sfvr_haptic_event(&input.haptics, SFVR_HAND_BOTH, SFVR_HAPTIC_SYSTEM);
        }

        if (r.turnDeg != 0.0f) {
            applyTurn(r.turnDeg);
        }

        sendNav(r.navHeld);
        updatePointer(s);

        // The game's Rumble Pak: 40 ms pulses on both hands while the motor runs.
        if (rumbleOn.load() && s.valid) {
            if (displayS >= nextRumbleS) {
                sfvr_haptic_submit(&input.haptics, SFVR_HAND_BOTH, 1.0f, SFVR_RUMBLE_PULSE_SECONDS);
                nextRumbleS = displayS + SFVR_RUMBLE_PULSE_SECONDS;
            }
        }

        if (s.valid) {
            input.flushHaptics(float(setting("haptics")));
        }

        {
            std::scoped_lock lock(inputMutex);
            latestInput = r;
        }

        // Action-state log for unattended runs (no wearer can press anything; the log shows the
        // path is live): any change of held N64 buttons, at most a few lines a second.
        if (s.valid && (r.buttons != lastLoggedButtons) && ((displayS - lastButtonLogS) > 0.25)) {
            lastLoggedButtons = r.buttons;
            lastButtonLogS = displayS;
            dk64vr_logf("input: n64 buttons %04X stick %.2f %.2f", r.buttons, r.stickX, r.stickY);
        }
    }

    void applyTurn(float deg) {
        // Rotates the game camera's heading (the camera patch takes it through recomp_vr_take_turn,
        // dk64vr_control.h) by the step; the eye camera follows
        // the game camera, so the world turns about Donkey Kong with no smooth swing.
        turnRemainder += deg;
        const int whole = int(turnRemainder);
        turnRemainder -= float(whole);
        if (whole != 0) {
            dk64vr_request_turn(whole);
        }

        if (std::fabs(deg) >= 5.0f) {
            dk64vr_logf("turn: %.0f deg", deg);
            blinkUntilS = displayS + setting("snap_blink");
        }
    }

    void queueControllerButton(uint8_t button, bool down) {
        SDL_Event event = {};
        event.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
        event.cbutton.which = 0;
        event.cbutton.button = button;
        event.cbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
        recompui::queue_event(event);
    }

    // recompui's menus read SDL controller events; the buttons are the ones recompinput's default
    // profile binds to each menu action (accept A, back X, apply Y, toggle Back, tabs shoulders).
    void sendNav(uint32_t held) {
        static const struct { uint32_t bit; uint8_t button; } map[] = {
            { nav::Up, SDL_CONTROLLER_BUTTON_DPAD_UP },
            { nav::Down, SDL_CONTROLLER_BUTTON_DPAD_DOWN },
            { nav::Left, SDL_CONTROLLER_BUTTON_DPAD_LEFT },
            { nav::Right, SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
            { nav::Accept, SDL_CONTROLLER_BUTTON_A },
            { nav::Back, SDL_CONTROLLER_BUTTON_X },
            { nav::Apply, SDL_CONTROLLER_BUTTON_Y },
            { nav::TabLeft, SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
            { nav::TabRight, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
            { nav::ToggleMenu, SDL_CONTROLLER_BUTTON_BACK },
        };

        const uint32_t changed = held ^ navHeldLast;
        for (const auto &m : map) {
            if (changed & m.bit) {
                queueControllerButton(m.button, (held & m.bit) != 0);
            }
        }

        if ((held & nav::ToggleMenu) && !(navHeldLast & nav::ToggleMenu)) {
            // recompui handles its events only while it draws, and it draws only into the panel:
            // show the panel for a moment so the toggle is seen.
            uiRequestUntilS = monotonicSeconds() + 1.0;
            dk64vr_logf("input: View pressed: recomp menu toggle");
        }

        navHeldLast = held;
    }

    Pose panelPose() const {
        return poseMul(origin, Pose { Quat(), { 0.0f, 0.0f, -float(setting("panel_distance")) } });
    }

    // The menu pointer: the dominant hand's aim ray (aim/pose -Z, lesson 41) against the panel.
    void updatePointer(const InputSample &s) {
        pointer.visible = false;
        const int hand = dominantHand();
        Pose aim;
        if (!s.valid || !panelShownLast || !uiMouse || !input.aimPose(hand, aim) || (panelChain.width == 0)) {
            if (pointerDown) {
                pointerDown = false;
                queueMouseButton(false);
            }

            return;
        }

        const Pose panel = panelPose();
        const float width = float(setting("panel_width"));
        const float height = width * float(panelChain.height) / float(panelChain.width);
        const Quat inv = quatConj(panel.q);
        const Vec3 rel = { aim.p.x - panel.p.x, aim.p.y - panel.p.y, aim.p.z - panel.p.z };
        const Vec3 o = quatRotate(inv, rel);
        const Vec3 d = quatRotate(inv, quatRotate(aim.q, Vec3 { 0.0f, 0.0f, -1.0f }));
        pointer.origin = aim.p;
        pointer.dir = quatRotate(aim.q, Vec3 { 0.0f, 0.0f, -1.0f });
        if (d.z >= -1e-4f) {
            return;
        }

        const float t = -o.z / d.z;
        const float hx = o.x + t * d.x, hy = o.y + t * d.y;
        if ((t <= 0.0f) || (std::fabs(hx) > 0.5f * width) || (std::fabs(hy) > 0.5f * height)) {
            return;
        }

        pointer.visible = true;
        pointer.length = t;
        pointer.hit = { aim.p.x + t * pointer.dir.x, aim.p.y + t * pointer.dir.y, aim.p.z + t * pointer.dir.z };
        pointer.surface = panel;
        const float u = hx / width + 0.5f, v = 0.5f - hy / height;
        const int px = int(u * float(panelChain.width)), py = int(v * float(panelChain.height));
        if ((px != pointerPx) || (py != pointerPy)) {
            pointerPx = px;
            pointerPy = py;
            SDL_Event event = {};
            event.type = SDL_MOUSEMOTION;
            event.motion.x = px;
            event.motion.y = py;
            recompui::queue_event(event);
        }

        const float trigger = (hand == 0) ? s.lTrigger : s.rTrigger;
        const bool down = triggerClick(pointerDown, trigger);
        if (down != pointerDown) {
            pointerDown = down;
            queueMouseButton(down);
            if (down) {
                sfvr_haptic_event(&input.haptics, hand, SFVR_HAPTIC_UI_CLICK);
            }
        }

        // Right stick Y scrolls (standard.md section 3).
        const float ry = s.rStick[1];
        if ((std::fabs(ry) > 0.5f) && (displayS >= nextScrollS)) {
            nextScrollS = displayS + 0.12;
            SDL_Event event = {};
            event.type = SDL_MOUSEWHEEL;
            event.wheel.y = (ry > 0.0f) ? 1 : -1;
            recompui::queue_event(event);
        }
    }

    void queueMouseButton(bool down) {
        SDL_Event event = {};
        event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
        event.button.clicks = 1;
        event.button.x = pointerPx;
        event.button.y = pointerPy;
        recompui::queue_event(event);
    }

    // ----------------------------------------------------------------------------------------
    // Images

    // Acquire under the queue's mutex (the runtime may submit there); the wait, which can depend
    // on another thread ending a frame, outside it.
    XrResult acquireImage(Swapchain &chain) {
        XrResult res;
        {
            std::scoped_lock lock(*queueMutex);
            XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            res = xrAcquireSwapchainImage(chain.handle, &acquireInfo, &chain.index);
        }

        if (XR_SUCCEEDED(res)) {
            XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            waitInfo.timeout = XR_INFINITE_DURATION;
            res = xrWaitSwapchainImage(chain.handle, &waitInfo);
        }

        return res;
    }

    void acquireEyes() {
        for (uint32_t eye = 0; eye < 2; eye++) {
            Swapchain &chain = eyeChain[eye];
            if (chain.acquired || (chain.handle == XR_NULL_HANDLE)) {
                continue;
            }

            XrResult res = acquireImage(chain);

            if (XR_FAILED(res)) {
                logOnce(acquireFailed, "eye image acquire failed (%s)", xrResultName(instance, res));
                continue;
            }

            chain.acquired = true;
            plume::VulkanTexture *image = chain.textures[chain.index].get();
            image->textureLayout = plume::RenderTextureLayout::COLOR_WRITE;
            image->barrierStages = plume::RenderBarrierStage::GRAPHICS;
        }
    }

    void releaseEyes() {
        std::scoped_lock lock(*queueMutex);
        for (uint32_t eye = 0; eye < 2; eye++) {
            Swapchain &chain = eyeChain[eye];
            if (chain.acquired) {
                XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(chain.handle, &releaseInfo);
                chain.acquired = false;
                chain.releasedOnce = true;
            }
        }
    }

    void clearEyes(plume::RenderColor left, plume::RenderColor right, bool probe) {
        acquireEyes();
        if (!frameWorker) {
            return;
        }

        const bool dumpProbe = probe && (setting("dump_frame") > 0.0) && !probeDumped && ((monotonicSeconds() - sessionStartS) > 1.0);
        plume::RenderCommandList *commandList = frameWorker->commandList.get();
        commandList->begin();
        for (uint32_t eye = 0; eye < 2; eye++) {
            Swapchain &chain = eyeChain[eye];
            if (!chain.acquired) {
                continue;
            }

            commandList->setFramebuffer(chain.framebuffers[chain.index].get());
            commandList->clearColor(0, (eye == 0) ? left : right);
            commandList->setFramebuffer(nullptr);
            if (dumpProbe) {
                plume::VulkanTexture *image = chain.textures[chain.index].get();
                recordReadback(commandList, image, chain, eyeReadback[eye], "vr-probe" + std::to_string(eye));
                commandList->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image, plume::RenderTextureLayout::COLOR_WRITE));
            }
        }

        commandList->end();
        frameWorker->execute();
        frameWorker->wait();
        if (dumpProbe) {
            probeDumped = true;
            finishReadback(eyeReadback[0]);
            finishReadback(eyeReadback[1]);
        }
    }

    void recordReadback(plume::RenderCommandList *commandList, plume::VulkanTexture *image, const Swapchain &chain, Readback &readback, const std::string &name) {
        const uint64_t size = uint64_t(chain.width) * chain.height * 4;
        if (!readback.buffer || (readback.width != chain.width) || (readback.height != chain.height)) {
            readback.buffer = device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(size));
            readback.width = chain.width;
            readback.height = chain.height;
        }

        readback.bgra = chain.bgra;
        commandList->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(image, plume::RenderTextureLayout::COPY_SOURCE));
        // plume copies buffers to images only, so the image-to-buffer copy is recorded directly.
        VkBufferImageCopy region = {};
        region.bufferRowLength = chain.width;
        region.bufferImageHeight = chain.height;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { chain.width, chain.height, 1 };
        vkCmdCopyImageToBuffer(static_cast<plume::VulkanCommandList *>(commandList)->vk, image->vk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            static_cast<plume::VulkanBuffer *>(readback.buffer.get())->vk, 1, &region);
        VkMemoryBarrier toHost = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(static_cast<plume::VulkanCommandList *>(commandList)->vk, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
        readback.pending = true;
        char suffix[64];
        snprintf(suffix, sizeof(suffix), "-%03" PRIu64 "-yaw%d.png", dumpsDone, int(std::lround(diagYawDeg)));
        std::error_code ec;
        std::filesystem::create_directories(dataDir / "vr-dumps", ec);
        readback.path = (dataDir / "vr-dumps" / (name + suffix)).string();
        if (name.rfind("vr-eye0", 0) == 0) {
            logDumpState();
        }
    }

    void finishReadback(Readback &readback) {
        if (!readback.pending) {
            return;
        }

        readback.pending = false;
        void *data = readback.buffer->map();
        if (data != nullptr) {
            writePngAsync(readback.path, static_cast<const uint8_t *>(data), readback.width, readback.height, readback.width * 4, readback.bgra);
            readback.buffer->unmap();
        }
    }

    void logDumpState() {
        const dk64vr_game_state state = gameState();
        const Vec3 l = eyeInHead[0].p, r = eyeInHead[1].p;
        dk64vr_logf("dump %" PRIu64 ": stereo frame %" PRIu64 ", diag_yaw %.0f, scene %s, cameras %u, eye L at %.3f %.3f %.3f R at %.3f %.3f %.3f m, weight %.2f, cam %.0f %.0f %.0f -> %.0f %.0f %.0f",
            dumpsDone, stereoFrames + 1, diagYawDeg, sceneName(state.scene), lastInfo.cameraProjections, l.x, l.y, l.z, r.x, r.y, r.z, scheduler.weight(displayS),
            state.cam_eye[0], state.cam_eye[1], state.cam_eye[2], state.cam_at[0], state.cam_at[1], state.cam_at[2]);
    }

    // ----------------------------------------------------------------------------------------
    // State

    dk64vr_settings_t settings = {};
    std::filesystem::path dataDir;
    bool installed = false;
    bool haveRefreshRate = false;
    bool haveFrameProfile = false;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    PFN_xrGetVulkanGraphicsRequirements2KHR getGraphicsRequirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR createVulkanInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getVulkanGraphicsDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createVulkanDevice = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB enumerateRefreshRates = nullptr;
    PFN_xrGetDisplayRefreshRateFB getRefreshRate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestRefreshRate = nullptr;

    plume::VulkanDevice *device = nullptr;
    uint32_t queueFamily = 0;
    std::mutex *queueMutex = nullptr;

    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    std::atomic<bool> sessionRunning = false;
    bool sessionFailed = false;
    std::atomic<bool> exitRequested = false;
    double sessionStartS = 0.0;
    bool rateRequested = false;
    bool rateLogged = false;

    Swapchain eyeChain[2];
    Swapchain panelChain;
    std::mutex panelMutex;
    std::atomic<bool> panelReady = false;
    std::atomic<bool> panelWanted = true;
    uint64_t panelPresents = 0;
    std::unique_ptr<RT64::RenderWorker> frameWorker;
    std::unique_ptr<RT64::RenderWorker> panelWorker;

    XrFrameState frameState = { XR_TYPE_FRAME_STATE };
    bool frameBegun = false;
    bool rendering = false;
    bool synthetic = false;
    bool probing = false;
    RT64::VrFrame::Mode frameMode = RT64::VrFrame::Mode::None;
    uint64_t frameCount = 0;
    uint64_t stereoFrames = 0;
    double displayS = 0.0;
    double lastDisplayS = 0.0;
    double periodS = 1.0 / 90.0;
    bool missed = false;
    double frameStartS = 0.0;
    double waitDoneS = 0.0;
    bool eyesRecorded[2] = {};
    uint32_t blackImages = 0;  // consecutive black frames drawn into the eye swapchains

    uint32_t blackNeeded() const {
        return uint32_t(std::max(eyeChain[0].textures.size(), eyeChain[1].textures.size()));
    }
    double eyeCpuMs[2] = {};
    double eyeGpuMs[2] = {};

    Pose origin;
    bool recentrePending = true;
    Pose eyeInHead[2];
    Pose eyeSubmitPose[2];
    FovTangents eyeFov[2];
    XrFovf eyeXrFov[2] = {};
    float diagYawDeg = 0.0f;

    TickScheduler scheduler;
    RT64::VrWorkloadInfo lastInfo;
    bool cameraSeen = false;
    bool cameraLogged = false;
    bool stereoLatched = false;
    bool playerExtentLogged = false;
    uint32_t gameplayFrames = 0;
    uint32_t latchCount = 0;
    int32_t lastScene = DK64VR_SCENE_UNKNOWN;

    bool dumpDue = false;
    uint64_t nextDumpFrame = 0;
    uint64_t dumpsDone = 0;
    bool probeDumped = false;
    Readback eyeReadback[2];
    Readback panelReadback;

    sfvr_perf perf = {};
    double perfWindowStartS = 0.0;
    uint64_t perfWindowArrivals = 0;
    uint64_t perfWindowRebases = 0;
    Autostart autostart;
    bool autostartBegun = false;
    double gameplaySinceS = 0.0;

    XrInput input;
    InputMapper mapper;
    std::mutex inputMutex;
    InputResult latestInput;
    std::atomic<bool> focused = false;
    std::atomic<bool> rumbleOn = false;
    bool gamePaused = false;
    bool inputLogged = false;
    uint16_t lastLoggedButtons = 0;
    double lastButtonLogS = 0.0;
    double nextRumbleS = 0.0;
    double nextScrollS = 0.0;
    uint32_t navHeldLast = 0;
    bool uiCapturing = false;
    bool uiMouse = false;
    double uiRequestUntilS = 0.0;
    bool panelShownLast = false;
    bool cinemaActive = false;
    Quat headInOriginQ;
    float turnRemainder = 0.0f;
    double blinkUntilS = 0.0;
    struct PointerState {
        bool visible = false;
        Vec3 origin, dir, hit;
        float length = 0.0f;
        Pose surface;
    } pointer;
    bool pointerDown = false;
    int pointerPx = -1, pointerPy = -1;

    bool waitFailed = false, beginFailed = false, endFailed = false, panelFailed = false, acquireFailed = false;
    bool fovFallback = false, ipdFallback = false, ipdLogged = false;
};

XrHost *host = nullptr;

}

bool hostInstall(const std::filesystem::path &appFolder) {
    if (host != nullptr) {
        return true;
    }

    host = new XrHost();
    if (!host->install(appFolder)) {
        delete host;
        host = nullptr;
        dk64vr_logpf("[openxr]", "VR mode off");
        return false;
    }

    return true;
}

void hostShutdown() {
    if (host != nullptr) {
        host->shutdown();
        delete host;
        host = nullptr;
    }
}

void hostFilterInput(int player, uint16_t *buttons, float *x, float *y) {
    if (host != nullptr) {
        host->filterInput(player, buttons, x, y);
    }
}

void hostSetRumble(int channel, bool on) {
    if (host != nullptr) {
        host->setRumble(channel, on);
    }
}

}

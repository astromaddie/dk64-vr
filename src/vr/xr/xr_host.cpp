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
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <condition_variable>
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

#include "dk64vr_camsmooth.h"
#include "dk64vr_game_state.h"
#include "dk64vr_input.h"
#include "dk64vr_control.h"
#include "dk64vr_log.h"
#include "dk64vr_math.h"
#include "dk64vr_sched.h"
#include "dk64vr_settings.h"
#include "sfvr/sfvr_perf.h"
#include "../diag/dk64vr_diag.h"
#include "../menu/dk64vr_menu_ui.h"
#include "xr_input.h"
#include "xr_foveation.h"

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

// An on/off switch from the environment (vr-diag lines); unset keeps the default.
bool envFlag(const char *name, bool fallback) {
    const char *value = getenv(name);
    if ((value == nullptr) || (value[0] == '\0')) {
        return fallback;
    }

    return (value[0] != '0');
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
    bool keepAlpha = false;
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

        // Establish this before the loader enumerates implicit API layers or creates Vulkan.
        // Explicit 0 is a Valve-only diagnostic run; FDM and KHR rate features cannot coexist.
        const char *disableFdm = std::getenv("DISABLE_VULKAN_FDM_INJECTION_LAYER");
        valveFdmDisabled = disableFdm == nullptr || strcmp(disableFdm, "0") != 0;
        if (valveFdmDisabled) setenv("DISABLE_VULKAN_FDM_INJECTION_LAYER", "1", 1);
        else unsetenv("DISABLE_VULKAN_FDM_INJECTION_LAYER"); // some implicit-layer loaders test presence, not the value

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
            haveVisibilityMask = haveVisibilityMask || (strcmp(e.extensionName, XR_KHR_VISIBILITY_MASK_EXTENSION_NAME) == 0);
            haveEyeGaze = haveEyeGaze || (strcmp(e.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0);
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
        haveEyeGaze = haveEyeGaze && setting("foveation_eye_tracked") != 0.0 && setting("foveation") != 0.0 && valveFdmDisabled;
        if (haveEyeGaze) enabled.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);

        // The hidden-area mesh, masked out of the eye passes before their scene (lesson 51).
        if (haveVisibilityMask) {
            enabled.push_back(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);
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
        XrSystemEyeGazeInteractionPropertiesEXT gazeProperties = { XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT };
        if (haveEyeGaze) systemProps.next = &gazeProperties;
        xrGetSystemProperties(instance, systemId, &systemProps);
        haveEyeGaze = haveEyeGaze && gazeProperties.supportsEyeGazeInteraction;
        dk64vr_logpf("[vr-fov]", "eye_tracked_requested=%d XR_EXT_eye_gaze_interaction=%s XR_FB_foveation=not-enabled XR_META_foveation_eye_tracked=not-enabled FDM_DEBUG=%s",
            int(setting("foveation_eye_tracked")), haveEyeGaze ? "enabled-supported" : "unavailable-or-not-requested", std::getenv("FDM_DEBUG") ? std::getenv("FDM_DEBUG") : "unset");
        dk64vr_logpf("[openxr]", "runtime %s %u.%u.%u, system %s, refresh-rate extension %s, Frame controller profile %s", instanceProps.runtimeName,
            XR_VERSION_MAJOR(instanceProps.runtimeVersion), XR_VERSION_MINOR(instanceProps.runtimeVersion), XR_VERSION_PATCH(instanceProps.runtimeVersion),
            systemProps.systemName, haveRefreshRate ? "yes" : "no", haveFrameProfile ? "yes" : "no");

        getProc("xrGetVulkanGraphicsRequirements2KHR", getGraphicsRequirements);
        getProc("xrCreateVulkanInstanceKHR", createVulkanInstance);
        getProc("xrGetVulkanGraphicsDevice2KHR", getVulkanGraphicsDevice);
        getProc("xrCreateVulkanDeviceKHR", createVulkanDevice);
        if (haveVisibilityMask) {
            getProc("xrGetVisibilityMaskKHR", getVisibilityMask);
        }

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

        // Tiler-friendly render passes (Adreno): whole-target clears become load ops, so the
        // driver's low-resolution Z stays usable; RT64_VR_DEFER_CLEARS=0 / RT64_VR_RO_DEPTH_LOAD=0
        // turn them off for comparisons.
        plume::VulkanPassOptions passOptions;
        passOptions.deferFullClears = envFlag("RT64_VR_DEFER_CLEARS", true);
        passOptions.loadReadOnlyDepth = envFlag("RT64_VR_RO_DEPTH_LOAD", true);
        plume::SetVulkanPassOptions(passOptions);
        dk64vr_logf("render passes: deferred clears %d, read-only depth load %d", int(passOptions.deferFullClears), int(passOptions.loadReadOnlyDepth));
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
        stopSaver();
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
        VkDeviceCreateInfo prepared;
        self->foveation.prepareDevice(physicalDevice, *createInfo, prepared, FoveationLevel(int(self->setting("foveation"))), self->valveFdmDisabled);
        XrVulkanDeviceCreateInfoKHR info = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
        info.systemId = self->systemId;
        info.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        info.vulkanPhysicalDevice = physicalDevice;
        info.vulkanCreateInfo = &prepared;
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
        self->foveation.deviceCreated(device);
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
        foveatedEye[0] = foveatedEye[1] = false;
        const bool standby = !forceRender && !(focused && frameState.shouldRender);
        setPaused(standby || vrMenuOpen, standby ? (focused ? "headset in standby" : "focus lost") : "VR settings menu");
        checkDiagExit();
        if (!rendering) {
            updateInput(false);
            frameMode = RT64::VrFrame::Mode::Flat;
            frame.mode = frameMode;
            frame.flatHeight = PanelHeight;
            return frame;
        }

        locateEyes();
        updateInput(true);
        const double now = monotonicSeconds();
        probing = (setting("probe_seconds") > 0.0) && ((now - sessionStartS) < setting("probe_seconds"));
        updateStereoLatch();
        if (!probing && stereoLatched && !frozen) {
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
        if (info.hudCalls > 0) {
            for (int i = 0; i < 7; i++) {
                hudPartChanges[i] += (info.hudChangedParts >> i) & 1;
            }
        }
        const double tick = (info.viOriginalRate > 0) ? (1.0 / double(info.viOriginalRate)) : (1.0 / 30.0);
        const double at = (frameMode != RT64::VrFrame::Mode::None) ? displayS : monotonicSeconds();
        const double tickBefore = scheduler.tick_s;
        if (scheduler.arrive(at, tick) && ((scheduler.rebases == 1) || (std::fabs(tickBefore - scheduler.tick_s) > 1e-9) || (setting("timing") != 0.0))) {
            dk64vr_logf("scheduler: anchored at %.1f Hz (rebase %" PRIu64 ", offset %.1f ms)", 1.0 / scheduler.tick_s, scheduler.rebases, scheduler.last_offset_s * 1000.0);
        }

        // The camera smoothing's history: every game frame's own camera (docs 5c). A scene
        // change, a mode change or a jump in the camera resets it (no swoop across a cut).
        if (info.cameraViewValid) {
            const dk64vr_game_state state = gameState();
            const bool sceneCut = (state.frame != 0) && (state.scene != camTickScene);
            camTickScene = state.scene;
            const bool cut = camSmoother.addTick(toMat4(info.cameraView), sceneCut);
            if (cut) {
                camWindowCuts++;
                camRawStats.restart();
                camOutStats.restart();
                camShownStats.restart();
                if (turnActive || (turnReleaseFrom != 0.0f)) {
                    // A camera cut mid-turn (respawn, warp): the turn is over, nothing to orbit.
                    turnActive = false;
                    turnDelta = turnShownDelta = turnReleaseFrom = 0.0f;
                }

                headingValid = false;
                if (camLogCuts()) {
                    dk64vr_logpf("[vr-cam]", "cut (%s) at game frame %u", sceneCut ? "scene change" : "camera jump", state.frame);
                }
            }
        }
        else if (camSmoother.ready()) {
            camSmoother.reset();
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
        // RT64_VR_SAME_EYE=1: both eyes get the left eye's camera (diagnostics: equal images).
        static const bool sameEye = envFlag("RT64_VR_SAME_EYE", false);
        if (sameEye) {
            eye = 0;
        }

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

        if (cinemaActive) {
            // The game camera as two eyes for the cinema screen (dk64vr_math.h, cinemaEye): the
            // head does not move the picture, only the screen's layer views do.
            const FovTangents gameTan = gameFov(gameProj);
            const float nearZ = gameProj.m[3][2] / (gameProj.m[2][2] - (-gameProj.m[2][3]));
            const Vec3 a = eyeInHead[0].p, b = eyeInHead[1].p;
            const float ipd = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
            const float separation = cinemaSeparation(float(setting("cinema_depth")) * ipd, float(setting("cinema_width")), gameTan, std::fabs(nearZ));
            const CinemaEye ce = cinemaEye(gameView, gameTan, separation, std::fabs(nearZ), int(eye));
            fromMat4(ce.view, outView);
            fromMat4(eyeProjection(ce.tan, gameProj), outProj);
            if (eye == 0) {
                cinemaGameTan = gameTan;
            }

            return;
        }

        // Camera smoothing: one rigid correction for the display frame, applied to every camera
        // projection of both eyes (docs 5c).
        if (camCorrectionFrame != frameCount) {
            updateCameraCorrection(gameView);
        }

        gameView = mul(gameView, camCorrection);

        // Turning (snap or smooth): the view shows the target heading at once, orbited about the
        // game camera's look-at point by what the game camera still has to turn; the game eases
        // its camera after it. Headings are unwrapped, so a smooth turn may run any distance ahead.
        if (turnFrame != frameCount) {
            turnFrame = frameCount;
            updateTurn(gameView);
        }

        if (turnShownDelta != 0.0f) {
            const dk64vr_game_state state = gameState();
            const float dx = state.cam_at[0] - state.cam_eye[0], dz = state.cam_at[2] - state.cam_eye[2];
            gameView = orbitViewBy(gameView, turnShownDelta, std::sqrt(dx * dx + dz * dz));
        }

        if (eye == 0) {
            shownHeading = viewHeading(gameView);
            if (shownStatsFrame != frameCount) {
                shownStatsFrame = frameCount;  // once a frame (a frame can have several camera projections)
                camShownStats.add(cameraPoseFromView(gameView));
            }
        }

        if (setting("level_pitch") != 0.0) {
            gameView = levelView(gameView);
        }

        fromMat4(eyeView(gameView, eyeInHead[eye], float(setting("world_scale"))), outView);
        fromMat4(eyeProjection(eyeFov[eye], gameProj), outProj);
    }

    plume::RenderFramebuffer *eyeFramebuffer(uint32_t eye, uint32_t *width, uint32_t *height) override {
        if ((eye > 1) || !eyeChain[eye].acquired || eyeChain[eye].framebuffers.empty()) {
            return nullptr;
        }

        Swapchain &chain = eyeChain[eye];
        *width = chain.width;
        *height = chain.height;
        return chain.framebuffers[chain.index].get();
    }

    uint32_t hiddenMesh(uint32_t eye, const float **ndcXY) override {
        // Only for eye images that span the headset's FOV (not the cinema's screen views).
        static const bool enabled = envFlag("RT64_VR_HIDDEN_MASK", true);
        if (!enabled || (eye > 1) || (getVisibilityMask == nullptr) || (session == XR_NULL_HANDLE) || cinemaActive || (frameMode != RT64::VrFrame::Mode::Stereo)) {
            return 0;
        }

        if (!hiddenMeshValid[eye]) {
            hiddenMeshValid[eye] = true;
            hiddenTangents[eye].clear();
            hiddenNdc[eye].clear();
            XrVisibilityMaskKHR mask = { XR_TYPE_VISIBILITY_MASK_KHR };
            XrResult res = getVisibilityMask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask);
            if (XR_SUCCEEDED(res) && (mask.vertexCountOutput > 0) && (mask.indexCountOutput > 0)) {
                std::vector<XrVector2f> vertices(mask.vertexCountOutput);
                std::vector<uint32_t> indices(mask.indexCountOutput);
                mask.vertexCapacityInput = uint32_t(vertices.size());
                mask.vertices = vertices.data();
                mask.indexCapacityInput = uint32_t(indices.size());
                mask.indices = indices.data();
                res = getVisibilityMask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask);
                if (XR_SUCCEEDED(res)) {
                    for (uint32_t i = 0; i < mask.indexCountOutput; i++) {
                        if (indices[i] < mask.vertexCountOutput) {
                            hiddenTangents[eye].push_back(vertices[indices[i]]);
                        }
                    }
                }
            }

            if (!hiddenMeshLogged || (eye == 1)) {
                hiddenMeshLogged = true;
                dk64vr_logf("hidden-area mesh, eye %u: %zu triangles (%s)", eye, hiddenTangents[eye].size() / 3, xrResultName(instance, res));
            }
        }

        if (hiddenTangents[eye].size() < 3) {
            return 0;
        }

        // Tangents to the eye image's NDC (y up), for the FOV the eye is rendered with.
        const FovTangents &fov = eyeFov[eye];
        const FovTangents &done = hiddenNdcFov[eye];
        if (hiddenNdc[eye].empty() || (fov.left != done.left) || (fov.right != done.right) || (fov.up != done.up) || (fov.down != done.down)) {
            hiddenNdcFov[eye] = fov;
            hiddenNdc[eye].resize(hiddenTangents[eye].size() * 2);
            for (size_t i = 0; i < hiddenTangents[eye].size(); i++) {
                const XrVector2f &t = hiddenTangents[eye][i];
                hiddenNdc[eye][i * 2 + 0] = (2.0f * t.x - (fov.right + fov.left)) / (fov.right - fov.left);
                hiddenNdc[eye][i * 2 + 1] = (2.0f * t.y - (fov.up + fov.down)) / (fov.up - fov.down);
            }
        }

        *ndcXY = hiddenNdc[eye].data();
        return uint32_t(hiddenTangents[eye].size());
    }

    void recordEye(uint32_t eye, plume::RenderCommandList *commandList, plume::RenderTexture *source, uint32_t width, uint32_t height, bool drawn) override {
        Swapchain &chain = eyeChain[eye];
        if (!chain.acquired || (eye > 1)) {
            return;
        }

        plume::VulkanTexture *image = chain.textures[chain.index].get();
        if (!drawn) {
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
        }

        if (dumpDue) {
            recordReadback(commandList, image, chain, eyeReadback[eye], "vr-eye" + std::to_string(eye));
        }

        commandList->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image, plume::RenderTextureLayout::COLOR_WRITE));
        eyesRecorded[eye] = true;
        if (setting("dump_foveation") != 0.0 && (dumpDue || !foveationDumped[eye])) {
            std::error_code ec;
            std::filesystem::create_directories(dataDir / "vr-dumps", ec);
            const std::string name = "vr-fov-eye" + std::to_string(eye) + "-" + std::to_string(dumpsDone) + ".png";
            foveation.dump(eye, width, height, (dataDir / "vr-dumps" / name).string(), foveatedEye[eye]);
            foveationDumped[eye] = true;
        }
    }

    plume::RenderFramebuffer *foveatedFramebuffer(uint32_t eye, plume::RenderCommandList *commands, plume::RenderFramebuffer *framebuffer) override {
        if (eye >= 2 || cinemaActive || frameMode != RT64::VrFrame::Mode::Stereo || !foveation.enabled()) return framebuffer;
        // The level follows the setting at once (the VR menu); off is no attachment at all.
        const int level = int(setting("foveation"));
        if (level == 0) return framebuffer;
        foveation.setLevel(FoveationLevel(level));
        FoveationCentre centre = projectionCentre(eyeFov[eye]);
        Pose gaze;
        const bool tracked = haveEyeGaze && (setting("foveation_eye_tracked") != 0.0) && runtimeEyesValid && !synthetic && input.gazePose(gaze);
        if (tracked) {
            const Quat rotation = quatMul(quatConj(runtimeEyePose[eye].q), gaze.q);
            centre = gazeCentre(eyeFov[eye], quatRotate(rotation, { 0, 0, -1 }));
        }
        if (eye == 0 && (gazeState < 0 || gazeState != int(tracked))) {
            if (gazeState >= 0) {
                gazeSwitches++;
                gazeSwitchesWindow++;
            }

            gazeState = int(tracked);
            // Dropouts: when gaze went invalid or came back, and how often (also per [vr-perf] window).
            dk64vr_logpf("[vr-fov]", "centre_source=%s at t=%.3f s (session), switch %" PRIu64 " (sampled once at predictedDisplayTime)", tracked ? "eye-gaze" : "fixed-projection",
                monotonicSeconds() - sessionStartS, gazeSwitches);
        }
        auto result = foveation.framebuffer(eye, commands, framebuffer, eyeChain[eye].width, eyeChain[eye].height, centre, frameCount);
        foveatedEye[eye] = foveatedEye[eye] || result != framebuffer;
        return result;
    }

    void setFoveationRate(plume::RenderCommandList *commands, bool useAttachment) override {
        if (foveation.enabled()) static_cast<plume::VulkanCommandList *>(commands)->setShadingRateEnabled(useAttachment);
    }

    plume::RenderFramebuffer *hudAcquire(uint32_t hudCalls, uint64_t hudSignature, uint32_t *width, uint32_t *height) override {
        lastHudCalls = hudCalls;
        hudGameFrames++;
        if ((hudCalls == 0) || (setting("hud_visible") == 0.0) || (hudChain.handle == XR_NULL_HANDLE) || (frameMode != RT64::VrFrame::Mode::Stereo)) {
            hudShown = false;
            hudSignatureShown = 0;
            return nullptr;
        }

        // The same HUD as the image already shown is not drawn again.
        if (hudShown && (hudSignature == hudSignatureShown) && !hudDumpDue) {
            hudSkipsSame++;
            return nullptr;
        }

        hudSignatureShown = hudSignature;
        hudLastDrawnFrame = hudGameFrames;

        if (!hudChain.acquired) {
            if (XR_FAILED(acquireImage(hudChain))) {
                logOnce(hudFailed, "HUD image acquire failed");
                return nullptr;
            }

            hudChain.acquired = true;
            plume::VulkanTexture *image = hudChain.textures[hudChain.index].get();
            image->textureLayout = plume::RenderTextureLayout::COLOR_WRITE;
            image->barrierStages = plume::RenderBarrierStage::GRAPHICS;
        }

        if (!hudLogged) {
            hudLogged = true;
            dk64vr_logf("HUD: first HUD image (%u calls), %ux%u", hudCalls, hudChain.width, hudChain.height);
        }

        *width = hudChain.width;
        *height = hudChain.height;
        return hudChain.framebuffers[hudChain.index].get();
    }

    void hudRecorded(double cpuMs, double gpuMs) override {
        hudShown = true;
        hudCpuSum += cpuMs;
        hudGpuSum += gpuMs;
        hudPasses++;
        if (hudDumpDue && frameWorker) {
            hudDumpDue = false;
            plume::VulkanTexture *image = hudChain.textures[hudChain.index].get();
            frameWorker->commandList->begin();
            recordReadback(frameWorker->commandList.get(), image, hudChain, hudReadback, "vr-hud", hudDumpIndex);
            frameWorker->commandList->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image, plume::RenderTextureLayout::COLOR_WRITE));
            frameWorker->commandList->end();
            frameWorker->execute();
            frameWorker->wait();
            finishReadback(hudReadback);
        }
    }

    void eyeDone(uint32_t eye, const RT64::VrEyeStats &stats) override {
        if (eye < 2) {
            eyeCpuMs[eye] = stats.cpuMs;
            eyeGpuMs[eye] = stats.gpuMs;
            eyeStats[eye] = stats;
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
        std::vector<XrCompositionLayerQuad> &extraQuads = extraQuadStorage;
        extraQuads.clear();
        extraQuads.reserve(8);
        bool stereoShown = false;
        XrCompositionLayerQuad fadeQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        if (rendering) {
            stereoShown = (frameMode == RT64::VrFrame::Mode::Stereo) && eyesRecorded[0] && eyesRecorded[1];
            const bool frozenShown = !stereoShown && frozen && haveFrozenViews && !probing;
            if (frozenShown) {
                // The last stereo images, released already, with the views they were made for.
                blackImages = 0;
            }
            else if (!stereoShown) {
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
            const bool cinemaShown = stereoShown && cinemaActive;
            if (cinemaShown && !cinemaAnchored) {
                anchorCinema();
            }

            // The picture between the letterbox bars (the camera's scissor), a little inside so
            // a bar's edge never shows.
            // The camera's projection spans its viewport's rows (DK64 narrows both to the band in
            // letterboxed scenes), so a frame row maps to a tangent through the viewport.
            const float top = std::clamp(lastInfo.cameraTop, 0.0f, 0.45f);
            const float bottom = std::clamp(lastInfo.cameraBottom, 0.55f, 1.0f);
            const float vTop = lastInfo.viewportTop;
            const float vSpan = std::max(0.1f, lastInfo.viewportBottom - lastInfo.viewportTop);
            auto rowTan = [&](float row) { return cinemaGameTan.up - (row - vTop) / vSpan * 2.0f * cinemaGameTan.up; };
            const float pictureUp = rowTan(top);
            const float pictureDown = rowTan(bottom);
            const float screenWidth = float(setting("cinema_width"));
            const float unitsPerTan = screenWidth / std::max(1e-3f, cinemaGameTan.right - cinemaGameTan.left);
            const float screenHeight = (pictureUp - pictureDown) * unitsPerTan;
            for (uint32_t eye = 0; eye < 2; eye++) {
                views[eye].pose = toXr(eyeSubmitPose[eye]);
                views[eye].fov = eyeXrFov[eye];
                views[eye].subImage.swapchain = eyeChain[eye].handle;
                views[eye].subImage.imageRect.offset = { 0, 0 };
                views[eye].subImage.imageRect.extent = { int32_t(eyeChain[eye].width), int32_t(eyeChain[eye].height) };
                views[eye].subImage.imageArrayIndex = 0;
                if (cinemaShown) {
                    Pose view;
                    view.q = cinemaScreen.q;
                    view.p = eyeSubmitPose[eye].p;
                    const FovTangents t = screenFov(cinemaScreen, screenWidth, screenHeight, view.p);
                    views[eye].pose = toXr(view);
                    views[eye].fov = { std::atan(t.left), std::atan(t.right), std::atan(t.up), std::atan(t.down) };
                    const int32_t h = int32_t(eyeChain[eye].height);
                    const int32_t y0 = int32_t(std::lround(top * float(h))), y1 = int32_t(std::lround(bottom * float(h)));
                    views[eye].subImage.imageRect.offset = { 0, y0 };
                    views[eye].subImage.imageRect.extent = { int32_t(eyeChain[eye].width), std::max(1, y1 - y0) };
                }
            }

            if (cinemaShown && ((lastInfo.cameraTop != cinemaLoggedTop) || (lastInfo.viewportTop != cinemaLoggedViewport))) {
                cinemaLoggedTop = lastInfo.cameraTop;
                cinemaLoggedViewport = lastInfo.viewportTop;
                dk64vr_logf("cinema: picture rows %.3f-%.3f, viewport %.3f-%.3f, game tan %.3f x %.3f, screen %.2f x %.2f m",
                    lastInfo.cameraTop, lastInfo.cameraBottom, lastInfo.viewportTop, lastInfo.viewportBottom, cinemaGameTan.right, cinemaGameTan.up, screenWidth, screenHeight);
            }

            if (cinemaShown) {
                // The HUD (subtitles, text boxes) on the screen, 2 cm in front of the picture
                // plane, covering the whole frame, so text in a letterbox bar sits under or
                // above the picture (lesson 72).
                cinemaHudPose = cinemaScreen;
                const Vec3 n = quatRotate(cinemaScreen.q, Vec3 { 0.0f, 0.0f, 1.0f });
                const Vec3 u = quatRotate(cinemaScreen.q, Vec3 { 0.0f, 1.0f, 0.0f });
                const float frameUp = rowTan(0.0f), frameDown = rowTan(1.0f);
                const float centreShift = (0.5f * (frameUp + frameDown) - 0.5f * (pictureUp + pictureDown)) * unitsPerTan;
                cinemaHudPose.p = { cinemaScreen.p.x + 0.02f * n.x + centreShift * u.x, cinemaScreen.p.y + 0.02f * n.y + centreShift * u.y, cinemaScreen.p.z + 0.02f * n.z + centreShift * u.z };
                cinemaHudSize[0] = screenWidth;
                cinemaHudSize[1] = (frameUp - frameDown) * unitsPerTan;
            }

            if (stereoShown) {
                frozenViews[0] = views[0];
                frozenViews[1] = views[1];
                haveFrozenViews = true;
            }
            else if (frozenShown) {
                views[0] = frozenViews[0];
                views[1] = frozenViews[1];
            }

            projection.space = localSpace;
            projection.viewCount = 2;
            projection.views = views;
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection));

            // The transition fade and the pause dim: view-locked black over the world, under the
            // panel.
            stepFade();
            const int fadeLevel = int(std::lround(fadeAlpha * float(FadeLevels - 1)));
            if (uiReady && (fadeLevel > 0)) {
                Pose ahead;
                ahead.p = { 0.0f, 0.0f, -0.3f };
                fadeQuad = uiQuad(viewSpace, ahead, 96, 32, 3.0f, 3.0f, fadeLevel);
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&fadeQuad));
            }

            const bool panelShown = !stereoShown && !probing && (!frozen || pausePanel || menuPanel);
            if (panelShown && panelReady.load()) {
                const float width = float(setting("panel_width"));
                const Pose panel = panelPose();
                quad.space = localSpace;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.layerFlags = panelImageUiOnly.load() ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
                quad.subImage.swapchain = panelChain.handle;
                quad.subImage.imageRect.offset = { 0, 0 };
                quad.subImage.imageRect.extent = { int32_t(panelChain.width), int32_t(panelChain.height) };
                quad.pose = toXr(panel);
                quad.size = { width, width * float(panelChain.height) / float(panelChain.width) };
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad));
            }

            // The HUD: a roll-free quad following the head (standard.md section 4), nearer than
            // what it overlaps (lesson 2); in first person it marks the game camera's aim.
            if (stereoShown && hudShown && hudReleasedOnce) {
                extraQuads.push_back(hudQuad());
            }

            // The snap blink: opaque black in front of the eyes while the game camera settles on
            // its new heading (head-locked, so no world motion shows).
            if (uiReady && (displayS < blinkUntilS)) {
                Pose ahead;
                ahead.p = { 0.0f, 0.0f, -0.3f };
                extraQuads.push_back(uiQuad(viewSpace, ahead, 96, 32, 3.0f, 3.0f, FadeLevels - 1));
            }

            addPointerLayers(extraQuads);
            for (const XrCompositionLayerQuad &extra : extraQuads) {
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&extra));
            }

            logLayers(layers, views);

            if (stereoShown) {
                stereoFrames++;
            }

            panelWanted = !stereoShown;
            panelShownLast = panelShown && panelReady.load();
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
            if (stereoShown) {
                // Window sums of the eye passes' statistics (RT64::VrEyeStats), per eye.
                statFrames++;
                for (int e = 0; e < 2; e++) {
                    const RT64::VrEyeStats &st = eyeStats[e];
                    statSum[e][0] += st.gpuCopyMs;
                    statSum[e][1] += st.recordMs;
                    statSum[e][2] += st.renderPasses;
                    statSum[e][3] += st.passMpx;
                    statSum[e][4] += st.draws;
                    statSum[e][5] += st.uberDraws;
                    statSum[e][6] += st.decalDraws;
                    statSum[e][7] += st.depthSwitches;
                    statSum[e][8] += st.testZ;
                    statSum[e][9] += st.clears;
                    statSum[e][10] += st.gpuMs;
                    statSum[e][11] += st.submitMs;
                    statSum[e][12] += st.waitMs;
                    statMax[e] = std::max(statMax[e], st.gpuMs + st.gpuCopyMs);
                }
            }
            char line[256];
            if (sfvr_perf_poll(&perf, nowS, line, sizeof(line))) {
                const double windowS = nowS - perfWindowStartS;
                const uint64_t foveatedPasses = device->shadingRatePasses.load(std::memory_order_relaxed);
                printf("%s mode=%s rate=%.1fHz eye=%ux%u game=%.1fHz rebases=%" PRIu64 " hud=%.2f/%.2fms x%u same=%u rate=%u changed=%u/%u/%u/%u/%u/%u/%u foveated_passes=%" PRIu64 " foveation=%s\n", line, stereoShown ? (cinemaActive ? "cinema" : "stereo") : "flat", (periodS > 0.0) ? (1.0 / periodS) : 0.0,
                    eyeChain[0].width, eyeChain[0].height, (windowS > 0.0) ? double(scheduler.arrivals - perfWindowArrivals) / windowS : 0.0, scheduler.rebases - perfWindowRebases,
                    hudPasses ? hudCpuSum / hudPasses : 0.0, hudPasses ? hudGpuSum / hudPasses : 0.0, hudPasses, hudSkipsSame, hudSkipsRate,
                    hudPartChanges[0], hudPartChanges[1], hudPartChanges[2], hudPartChanges[3], hudPartChanges[4], hudPartChanges[5], hudPartChanges[6],
                    foveatedPasses - perfWindowFoveatedPasses, foveationName(FoveationLevel(int(setting("foveation")))));
                perfWindowFoveatedPasses = foveatedPasses;
                if (haveEyeGaze && foveation.enabled()) {
                    printf("[vr-fov] window: gaze switches %u (total %" PRIu64 "), centre_source=%s\n", gazeSwitchesWindow, gazeSwitches, (gazeState == 1) ? "eye-gaze" : "fixed-projection");
                }

                gazeSwitchesWindow = 0;
                if (statFrames > 0) {
                    const double n = double(statFrames);
                    auto avg = [&](int e, int k) { return statSum[e][k] / n; };
                    printf("[vr-perf] eyes gpu=%.2f/%.2f copy=%.2f/%.2f max=%.2f/%.2fms rec=%.2f/%.2f submit=%.2f/%.2f wait=%.2f/%.2fms passes=%.1f/%.1f mpx=%.1f/%.1f draws=%.0f uber=%.0f decal=%.0f dsw=%.1f/%.1f testz=%.1f clears=%.1f\n",
                        avg(0, 10), avg(1, 10), avg(0, 0), avg(1, 0), statMax[0], statMax[1], avg(0, 1), avg(1, 1), avg(0, 11), avg(1, 11), avg(0, 12), avg(1, 12), avg(0, 2), avg(1, 2), avg(0, 3), avg(1, 3),
                        avg(1, 4), avg(1, 5), avg(1, 6), avg(0, 7), avg(1, 7), avg(1, 8), avg(1, 9));
                }
                logCameraWindow();
                statFrames = 0;
                std::fill(&statSum[0][0], &statSum[0][0] + 2 * 13, 0.0);
                statMax[0] = statMax[1] = 0.0;
                std::fill(std::begin(hudPartChanges), std::end(hudPartChanges), 0u);
                hudCpuSum = hudGpuSum = 0.0;
                hudPasses = 0;
                hudSkipsSame = hudSkipsRate = 0;
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
        // The VR menu floats over the frozen world: only recompui's draw, over transparent black
        // (blended by the panel layer, premultiplied). Latched per image so the layer's blend
        // always matches the image it shows.
        panelAcquiredUiOnly = vrMenuShown.load();
        plume::VulkanTexture *image = panelChain.textures[panelChain.index].get();
        image->textureLayout = plume::RenderTextureLayout::COLOR_WRITE;
        image->barrierStages = plume::RenderBarrierStage::GRAPHICS;
        *texture = image;
        *width = panelChain.width;
        *height = panelChain.height;
        return panelChain.framebuffers[panelChain.index].get();
    }

    bool panelUiOnly() override {
        return panelAcquiredUiOnly;
    }

    // VI thread: the game is paused while the VR menu is open, so no workload brings a present;
    // RT64 then presents on every VI tick so recompui keeps drawing the menu.
    bool presentWithoutWorkloads() override {
        return vrMenuShown.load();
    }

    void panelRelease() override {
        if (!panelChain.acquired) {
            panelMutex.unlock();
            return;
        }

        panelPresents++;
        // The VR menu tour's dumps (with alpha: the window over transparent black).
        const int menuDump = menuPanelDump.exchange(-1);
        if ((menuDump >= 0) && panelWorker) {
            plume::VulkanTexture *image = panelChain.textures[panelChain.index].get();
            panelWorker->commandList->begin();
            panelReadback.keepAlpha = true;
            recordReadback(panelWorker->commandList.get(), image, panelChain, panelReadback, "vr-menu-" + menuDumpName, menuDump);
            panelWorker->commandList->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image, plume::RenderTextureLayout::COLOR_WRITE));
            panelWorker->commandList->end();
            panelWorker->execute();
            panelWorker->wait();
            finishReadback(panelReadback);
            panelReadback.keepAlpha = false;
            layersLogDue = true;
            dk64vr_logf("menu: dump %d %s (ui only %d)", menuDump, menuDumpName.c_str(), int(panelAcquiredUiOnly));
        }
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
        panelImageUiOnly = panelAcquiredUiOnly;
        panelReady = true;
        panelMutex.unlock();
    }

    // UI thread (the launcher's init callback): the VR settings menu's document.
    void uiInit() {
        menu::uiCreate();
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
        char scaleText[16];
        if (setting("resolution_scale") == DK64VR_RESOLUTION_AUTO) {
            snprintf(scaleText, sizeof(scaleText), "auto");
        }
        else {
            snprintf(scaleText, sizeof(scaleText), "%.2f", setting("resolution_scale"));
        }

        dk64vr_logf("settings: world_scale %.1f, resolution_scale %s, camera_smoothing %s, refresh_rate %d, level_pitch %d, force_render %d, diag_yaw %.0f, dump_frame %d, probe_seconds %.1f, diag_autostart %.0f, diag_exit_seconds %.0f, diag_force_stereo %d, timing %d",
            setting("world_scale"), scaleText, cameraSmoothingName(CameraSmoothing(int(setting("camera_smoothing")))), int(setting("refresh_rate")), int(setting("level_pitch")), int(setting("force_render")),
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

        // resolution_scale auto: about 2160 an eye whatever SteamVR recommends (its per-app
        // resolution moves the recommendation); a number multiplies the recommendation.
        const double scale = setting("resolution_scale");
        const uint32_t recommended[2] = { configViews[0].recommendedImageRectWidth, configViews[0].recommendedImageRectHeight };
        recommendedEyeWidth = int(recommended[0]);
        const uint32_t maximum[2] = { configViews[0].maxImageRectWidth, configViews[0].maxImageRectHeight };
        const EyeSize chosen = dk64vr::eyeSize(recommended[0], recommended[1], maximum[0], maximum[1], scale);
        const uint32_t eyeSize[2] = { chosen.width, chosen.height };
        if (scale == DK64VR_RESOLUTION_AUTO) {
            dk64vr_logf("eye size: resolution_scale auto, recommended %ux%u -> %ux%u (x%.3f, target 2160, max %ux%u)", recommended[0], recommended[1],
                eyeSize[0], eyeSize[1], chosen.scale, maximum[0], maximum[1]);
        }
        else {
            dk64vr_logf("eye size: resolution_scale %.2f, recommended %ux%u -> %ux%u (max %ux%u)", scale, recommended[0], recommended[1], eyeSize[0], eyeSize[1], maximum[0], maximum[1]);
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
        dk64vr_logpf("[openxr]", "recommended eye %ux%u (max %ux%u), using %ux%u (x%.3f); swapchain formats: %s",
            recommended[0], recommended[1], maximum[0], maximum[1], eyeSize[0], eyeSize[1], chosen.scale, formatList.c_str());

        // RT64's main color image is RGBA8 UNORM holding gamma-encoded colour: copied raw into an
        // sRGB image it is decoded once by the compositor (lessons 8, 42).
        const bool dumps = (setting("dump_frame") > 0.0) || (setting("dump_panel_frame") > 0.0) || (setting("diag_vr_menu_at") > 0.0);
        if (has(VK_FORMAT_R8G8B8A8_SRGB)) {
            for (int eye = 0; eye < 2; eye++) {
                createSwapchain(eyeChain[eye], eyeSize[0], eyeSize[1], VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, plume::RenderFormat::R8G8B8A8_UNORM, false, dumps);
            }

            // The HUD image: half the eye image's size (a stretched 4:3 frame shown on a 4:3
            // quad of about 40 degrees: some 27 pixels a degree, more than the eyes' 17).
            createSwapchain(hudChain, (eyeSize[0] + 1) / 2, (eyeSize[1] + 1) / 2, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, plume::RenderFormat::R8G8B8A8_UNORM, false, dumps);
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

        input.create(instance, session, haveFrameProfile, haveEyeGaze && foveation.enabled());
        frameWorker = std::make_unique<RT64::RenderWorker>(device, "VR Frame", plume::RenderCommandListType::DIRECT);
        createUiImage();
        panelWorker = std::make_unique<RT64::RenderWorker>(device, "VR Panel", plume::RenderCommandListType::DIRECT);
        dk64vr_logpf("[openxr]", "session created; eye swapchains %ux%u format %d, panel %ux%u", eyeChain[0].width, eyeChain[0].height, int(eyeChain[0].format), panelChain.width, panelChain.height);
    }

    void createSwapchain(Swapchain &chain, uint32_t width, uint32_t height, VkFormat format, VkFormat viewFormat, plume::RenderFormat renderFormat, bool bgra, bool transferSource, XrSwapchainCreateFlags createFlags = 0) {
        XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        info.createFlags = createFlags;
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
        foveation.reset();

        {
            std::scoped_lock lock(panelMutex);
            destroySwapchain(panelChain);
            panelReady = false;
        }

        destroySwapchain(eyeChain[0]);
        destroySwapchain(eyeChain[1]);
        destroySwapchain(uiChain);
        destroySwapchain(hudChain);
        hudReadback.buffer.reset();
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
            case XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR:
                hiddenMeshValid[0] = hiddenMeshValid[1] = false;
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
        const XrResult viewResult = xrLocateViews(session, &locateInfo, &viewState, 2, &count, views);
        XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
        xrLocateSpace(viewSpace, localSpace, frameState.predictedDisplayTime, &head);
        const XrViewStateFlags needed = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        const bool viewsValid = (count == 2) && ((viewState.viewStateFlags & needed) == needed);
        const bool headValid = (head.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT)) == (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT);
        runtimeEyesValid = XR_SUCCEEDED(viewResult) && viewsValid && headValid;

        // FOV from the runtime; the Frame's measured left-eye FOV (platform.md) when it gives none.
        for (uint32_t eye = 0; eye < 2; eye++) {
            XrFovf fov = views[eye].fov;
            if (!(fov.angleRight - fov.angleLeft > 0.1f) || !(fov.angleUp - fov.angleDown > 0.1f)) {
                fov = (eye == 0) ? XrFovf { -1.028f, 0.885f, 0.864f, -1.048f } : XrFovf { -0.885f, 1.028f, 0.864f, -1.048f };
                logOnce(fovFallback, "the runtime gave no eye FOV; using the Frame's measured one");
            }

            eyeXrFov[eye] = fov;
            runtimeEyePose[eye] = fromXr(views[eye].pose);
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
                cinemaAnchored = false;
                panelYawRad = 0.0f;
                dk64vr_logf("recentre: head yaw %.1f deg, at %.2f %.2f %.2f m", quatYaw(headPose.q) * 180.0f / 3.1415926f, headPose.p.x, headPose.p.y, headPose.p.z);
            }

            headInOrigin = poseMul(poseInverse(origin), headPose);
            // head_translation scales the head's displacement, never the eye offsets (patterns.md).
            const float translation = float(setting("head_translation"));
            headInOrigin.p = { headInOrigin.p.x * translation, headInOrigin.p.y * translation, headInOrigin.p.z * translation };
        }

        Pose yaw;
        yaw.q = quatFromYaw(diagYawDeg * 3.1415926f / 180.0f);
        headInOrigin = poseMul(yaw, headInOrigin);
        headInOriginQ = headInOrigin.q;
        for (uint32_t eye = 0; eye < 2; eye++) {
            eyeInHead[eye] = poseMul(headInOrigin, eyeFromHead[eye]);
            eyeSubmitPose[eye] = poseMul(origin, eyeInHead[eye]);
        }

        dumpDue = ((nextDumpFrame > 0) && (dumpsDone < uint64_t(setting("dump_count"))) && (stereoFrames + 1 >= nextDumpFrame)) || (menuEyeDump && stereoLatched && !frozen && !probing);
        if (dumpDue && menuEyeDump) {
            menuEyeDump = false;
        }
        if (dumpDue) {
            hudDumpDue = true;  // with the next HUD image
            hudDumpIndex = dumpsDone;
        }
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
            // A recomp menu (or a toggle on its way): the panel at once, no hysteresis. Over
            // stereo play it goes in front of the frozen, dimmed world (updateFreeze), which
            // comes back when the menu closes.
            if (stereoLatched && !menuPanel) {
                menuPanel = true;
                if (!frozen) {
                    frozen = true;
                    freezeLiveCount = 0;
                    dk64vr_logf("freeze: recomp menu (stereo world held%s)", haveFrozenViews ? "" : ", no image yet: black");
                }

                fadeTarget = 0.6f;
                dk64vr_logf("menu panel: on");
            }

            return;
        }
        else if (menuPanel) {
            menuPanel = false;
            dk64vr_logf("menu panel: off");
        }
        else if (setting("diag_force_stereo") != 0.0) {
            want = cameraSeen;
            why = "diag_force_stereo";
        }
        else if (published) {
            const bool cutscenePanel = (state.scene == DK64VR_SCENE_CUTSCENE) && (int(setting("cutscenes")) == 2);
            want = sceneIsStereo(state.scene) && cameraSeen && !cutscenePanel;
            why = cutscenePanel ? "cutscenes on the panel" : sceneName(state.scene);
            // Map transitions and the pause menu keep the stereo world (frozen, faded or dimmed:
            // updateFreeze) instead of flipping to the panel and back.
            if (stereoLatched && sceneHolds(state.scene)) {
                want = true;
            }
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
                transitionBlink();
                resetCameraSmoothing();
                dk64vr_logf("mode: %s (%s, %u camera projection(s))", want ? "stereo" : "panel", why, lastInfo.cameraProjections);
            }
        }
        else {
            latchCount = 0;
        }

        updateFreeze(state, published);
        if (frozen) {
            return;  // the cinema stays as it was until the world is live again
        }

        // The 3D cinema for cutscenes and scripted cameras (standard.md section 4), latched with
        // hysteresis (lesson 71): in after 6 frames of cutscene, out after 24 without.
        const bool cinemaWant = stereoLatched && published && (state.scene == DK64VR_SCENE_CUTSCENE) && (int(setting("cutscenes")) == 0);
        if (cinemaWant != cinemaActive) {
            if (++cinemaLatchCount >= (cinemaWant ? 6u : 24u)) {
                cinemaActive = cinemaWant;
                cinemaLatchCount = 0;
                cinemaAnchored = false;
                transitionBlink();
                resetCameraSmoothing();
                dk64vr_logf("cinema: %s", cinemaActive ? "on" : "off");
            }
        }
        else {
            cinemaLatchCount = 0;
        }
    }

    static bool sceneHolds(int32_t scene) {
        return (scene == DK64VR_SCENE_TRANSITION) || (scene == DK64VR_SCENE_PAUSE);
    }

    // Transitions and the pause menu in stereo (docs section 4, "Transitions and pause"): the
    // world freezes at once on the last stereo images (submitted again with their own poses, so
    // they stay world-locked), a map transition fades it to black, the pause menu dims it and
    // puts the panel in front after 6 frames. The world comes back when the scene has wanted
    // stereo for 6 frames, fading in from black. Single-frame flips never show (lesson 71).
    void updateFreeze(const dk64vr_game_state &state, bool published) {
        const bool hold = stereoLatched && published && sceneHolds(state.scene);
        const bool live = published && sceneIsStereo(state.scene) && cameraSeen;
        if (!stereoLatched) {
            frozen = false;
            pausePanel = menuPanel = false;
            freezeLiveCount = pauseCount = 0;
            fadeTarget = 0.0f;
            return;
        }

        if (hold && !frozen) {
            frozen = true;
            freezeLiveCount = 0;
            dk64vr_logf("freeze: %s (stereo world held%s)", sceneName(state.scene), haveFrozenViews ? "" : ", no image yet: black");
        }

        if (frozen) {
            if (live && !hold && !menuPanel) {
                if (++freezeLiveCount >= 6) {
                    frozen = false;
                    pausePanel = false;
                    pauseCount = 0;
                    resetCameraSmoothing();
                    dk64vr_logf("freeze: off (%s)", sceneName(state.scene));
                }
            }
            else {
                freezeLiveCount = 0;
            }
        }

        // The pause menu's panel, latched both ways.
        const bool pauseWant = frozen && published && (state.scene == DK64VR_SCENE_PAUSE);
        if (pauseWant != pausePanel) {
            if (++pauseCount >= 6) {
                pausePanel = pauseWant;
                pauseCount = 0;
                dk64vr_logf("pause panel: %s", pausePanel ? "on" : "off");
            }
        }
        else {
            pauseCount = 0;
        }

        fadeTarget = !frozen ? 0.0f : (((state.scene == DK64VR_SCENE_TRANSITION) && !pausePanel) ? 1.0f : ((pausePanel || menuPanel) ? 0.6f : fadeTarget));
    }

    // The fade quad's alpha moves towards its target: to black in 0.15 s, back in 0.3 s.
    void stepFade() {
        const float dt = float(std::clamp(periodS, 0.0, 0.1));
        if (fadeAlpha < fadeTarget) {
            fadeAlpha = std::min(fadeTarget, fadeAlpha + dt / 0.15f);
        }
        else if (fadeAlpha > fadeTarget) {
            fadeAlpha = std::max(fadeTarget, fadeAlpha - dt / 0.3f);
        }
    }

    // ----------------------------------------------------------------------------------------
    // Camera smoothing (dk64vr_camsmooth.h, docs 5c)

    CameraSmoothing cameraSmoothingLevel() const {
        if (cinemaActive || !camSmoother.ready()) {
            return CameraSmoothing::Off;
        }

        // First person: the crosshair is the game camera's aim, so no lag there.
        const dk64vr_game_state state = gameState();
        if (state.scene == DK64VR_SCENE_FIRST_PERSON) {
            return CameraSmoothing::Off;
        }

        return CameraSmoothing(std::clamp(int(setting("camera_smoothing")), 0, 3));
    }

    bool camLogCuts() const {
        return (setting("timing") != 0.0) || (setting("diag_camera_log") != 0.0) || (setting("diag_camera_seconds") > 0.0);
    }

    void resetCameraSmoothing() {
        camSmoother.reset();
        camRawStats.restart();
        camOutStats.restart();
        camShownStats.restart();
        headingValid = false;
        turnActive = false;
        turnDelta = turnShownDelta = turnReleaseFrom = 0.0f;
        camCorrection = Mat4();
    }

    // Once per display frame, at its first camera projection: the correction from RT64's linear
    // camera (which the smoother reproduces) to the smoothed one, inverse(linear) * smoothed, so
    // gameView * correction is the smoothed view. `firstView` is RT64's view of that projection,
    // logged against the reproduction (match=).
    void updateCameraCorrection(const Mat4 &firstView) {
        camCorrectionFrame = frameCount;
        camCorrection = Mat4();
        if (!camSmoother.ready()) {
            camLastSampleS = displayS;
            return;
        }

        const CameraSmoothing level = cameraSmoothingLevel();
        if (level != camLevelLogged) {
            camLevelLogged = level;
            dk64vr_logpf("[vr-cam]", "smoothing %s (B-spline %s, orientation follower %.2f s)", cameraSmoothingName(level), (level == CameraSmoothing::Off) ? "off" : "on",
                cameraFollowTime(level));
        }

        const float w = scheduler.weight(displayS);
        const float dt = (camLastSampleS > 0.0) ? float(std::clamp(displayS - camLastSampleS, 0.0, 0.1)) : float(periodS);
        camLastSampleS = displayS;
        const Mat4 linearView = camSmoother.linear(w);
        const Mat4 smoothView = camSmoother.sample(w, dt, level);
        camCorrection = mul(rigidInverse(linearView), smoothView);

        // Diagnostics: the linear (what smoothing off shows) and the smoothed camera's steps.
        const Pose raw = cameraPoseFromView(linearView), out = cameraPoseFromView(smoothView);
        camRawStats.add(raw);
        camOutStats.add(out);
        const Pose rt64 = cameraPoseFromView(firstView);
        const float matchPos = std::sqrt((rt64.p.x - raw.p.x) * (rt64.p.x - raw.p.x) + (rt64.p.y - raw.p.y) * (rt64.p.y - raw.p.y) + (rt64.p.z - raw.p.z) * (rt64.p.z - raw.p.z));
        camMatchPos = std::max(camMatchPos, double(matchPos));
        camMatchDeg = std::max(camMatchDeg, double(quatAngle(rt64.q, raw.q)) * 180.0 / 3.14159265);
        if (setting("diag_camera_log") != 0.0) {
            dk64vr_logpf("[vr-cam]", "f %" PRIu64 " t %.4f w %.3f raw yaw %.3f pitch %.3f pos %.2f %.2f %.2f out yaw %.3f pitch %.3f pos %.2f %.2f %.2f", frameCount, displayS, w,
                cameraYaw(raw) * 57.2957795, cameraPitch(raw) * 57.2957795, raw.p.x, raw.p.y, raw.p.z,
                cameraYaw(out) * 57.2957795, cameraPitch(out) * 57.2957795, out.p.x, out.p.y, out.p.z);
        }
    }

    // The [vr-cam] window summary, with the [vr-perf] line: per display frame, the camera's
    // heading, pitch and position steps (sd, max) and how much the step changes from one frame
    // to the next (sd, max: the abruptness the wearer feels), for the linear camera (raw) and the
    // one shown (out).
    void logCameraWindow() {
        if (camOutStats.frames > 0) {
            auto part = [](const CameraStepStats &st, char *buffer, size_t size) {
                snprintf(buffer, size, "yaw step sd %.3f max %.3f chg sd %.3f max %.3f deg, pitch step sd %.3f max %.3f chg sd %.3f max %.3f deg, pos step sd %.3f max %.3f chg sd %.3f max %.3f units",
                    st.stepSd(0), st.maxAbs[0], st.changeSd(0), st.dMaxAbs[0], st.stepSd(1), st.maxAbs[1], st.changeSd(1), st.dMaxAbs[1], st.stepSd(2), st.maxAbs[2], st.changeSd(2), st.dMaxAbs[2]);
            };
            char raw[400], out[400];
            part(camRawStats, raw, sizeof(raw));
            part(camOutStats, out, sizeof(out));
            printf("[vr-cam] smoothing=%s frames=%u cuts=%u match=%.3f units/%.3f deg | raw: %s | out: %s\n", cameraSmoothingName(camLevelLogged), camOutStats.frames, camWindowCuts,
                camMatchPos, camMatchDeg, raw, out);
            // The view as shown (after the turn's orbit): its mean heading rate and the turn
            // requested from the stick in the window.
            const double windowS = (camShownStats.frames > 0) ? double(camShownStats.frames) * periodS : 0.0;
            printf("[vr-cam] shown: yaw rate %.1f deg/s (mean step %.3f, sd %.3f, max %.3f, chg max %.3f deg), turn requested %.1f deg (%.1f deg/s)\n",
                (windowS > 0.0) ? camShownStats.sum[0] / windowS : 0.0, (camShownStats.frames > 0) ? camShownStats.sum[0] / camShownStats.frames : 0.0, camShownStats.stepSd(0),
                camShownStats.maxAbs[0], camShownStats.dMaxAbs[0], -turnLogDeg, (windowS > 0.0) ? -turnLogDeg / windowS : 0.0);
        }

        camShownStats.clearWindow();
        turnLogDeg = 0.0f;

        camRawStats.clearWindow();
        camOutStats.clearWindow();
        camWindowCuts = 0;
        camMatchPos = camMatchDeg = 0.0;
    }

    // Diagnostic (diag_camera_seconds): camera motion in stereo play without a wearer.
    void diagCamera(InputResult &input, const dk64vr_game_state &state) {
        const double every = setting("diag_camera_seconds");
        if ((every <= 0.0) || !stereoLatched || cinemaActive || (state.scene != DK64VR_SCENE_GAMEPLAY)) {
            return;
        }

        if (diagCameraStartS == 0.0) {
            diagCameraStartS = displayS;
        }

        const double t = displayS - diagCameraStartS;
        const int phase = int(t / every) % 4;
        const double into = std::fmod(t, every);
        if (phase != diagCameraPhase) {
            diagCameraPhase = phase;
            static const char *names[] = { "C-left", "C-right", "stick forward-right", "idle" };
            dk64vr_logpf("[vr-cam]", "diag: %s", names[phase]);
        }

        if ((phase == 0) && (into < 0.15)) {
            input.buttons |= n64::CLeft;
        }
        else if ((phase == 1) && (into < 0.15)) {
            input.buttons |= n64::CRight;
        }
        else if (phase == 2) {
            input.stickX = 0.8f;
            input.stickY = 0.6f;
        }
    }

    // A short black between flat, stereo and cinema (standard.md section 4 asks for a fade;
    // this is a cut through black until a fade layer exists).
    void transitionBlink() {
        blinkUntilS = std::max(blinkUntilS, displayS + 0.2);
    }

    // The cinema screen: placed in front of the head (yaw only, centre at eye height) when it
    // comes on and on recentre.
    void anchorCinema() {
        const Quat head = quatMul(origin.q, headInOriginQ);
        const float yaw = quatYaw(head);
        const Vec3 eyeMid = { 0.5f * (eyeSubmitPose[0].p.x + eyeSubmitPose[1].p.x), 0.5f * (eyeSubmitPose[0].p.y + eyeSubmitPose[1].p.y), 0.5f * (eyeSubmitPose[0].p.z + eyeSubmitPose[1].p.z) };
        cinemaScreen.q = quatFromYaw(yaw);
        const Vec3 ahead = quatRotate(cinemaScreen.q, Vec3 { 0.0f, 0.0f, -float(setting("cinema_distance")) });
        cinemaScreen.p = { eyeMid.x + ahead.x, eyeMid.y, eyeMid.z + ahead.z };
        cinemaAnchored = true;
        dk64vr_logf("cinema: screen %.1f m ahead at yaw %.0f deg, %.1f m wide", setting("cinema_distance"), yaw * 180.0f / 3.1415926f, setting("cinema_width"));
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

        return quatYaw(headInOriginQ) + turnShownDelta;  // plus the turn the view shows ahead of the game camera
    }

    void updateInput(bool renderingNow) {
        const bool live = renderingNow && focused && input.ready() && (synthetic == false);
        const InputSample s = input.sample(live, localSpace, frameState.predictedDisplayTime);
        uiCapturing = recompui::is_context_capturing_input();
        uiMouse = uiCapturing && recompui::is_context_capturing_mouse();
        const dk64vr_game_state state = gameState();
        InputContext context;
        context.menuOpen = uiCapturing;
        // Turning and view-relative movement in third-person play only (first person turns with
        // the head and the game's own aim).
        context.stereoPlay = stereoLatched && !frozen && !cinemaActive && (state.scene == DK64VR_SCENE_GAMEPLAY);
        context.viewYawRad = moveYawRad();
        const float dt = float(std::min(periodS, 0.1));
        InputResult r = mapper.update(s, inputConfig(), context, displayS, dt);
        // Diagnostic: right stick X held at diag_turn_stick for 4 s, released for 4 s, in
        // stereo play, as the mapper would turn with it (snap: one step a hold).
        const double turnStick = setting("diag_turn_stick");
        if ((turnStick != 0.0) && context.stereoPlay && (setting("right_stick_x") == 0.0)) {
            if (diagTurnStartS == 0.0) {
                diagTurnStartS = displayS;
            }

            const bool held = std::fmod(displayS - diagTurnStartS, 8.0) < 4.0;
            if (held != diagTurnHeld) {
                diagTurnHeld = held;
                dk64vr_logpf("[vr-cam]", "diag: right stick %s", held ? "held" : "released");
                if (held && (TurnMode(int(setting("turn_mode"))) == TurnMode::Snap)) {
                    r.turnDeg += float(setting("snap_angle")) * ((turnStick > 0.0) ? 1.0f : -1.0f);
                }
            }

            if (held && (TurnMode(int(setting("turn_mode"))) == TurnMode::Smooth)) {
                r.turnDeg += smoothTurnRate(float(turnStick), float(setting("smooth_turn_speed"))) * dt;
            }
        }
        if (s.valid && !inputLogged) {
            inputLogged = true;
            dk64vr_logf("input: first live sample (right_stick_x %s, turn_mode %d, snap %d, move_from_view %d)",
                (setting("right_stick_x") != 0.0) ? "camera" : "turn", int(setting("turn_mode")), int(setting("snap_angle")), int(setting("move_from_view")));
        }

        lastSample = s;
        if (r.vrMenuChord) {
            sfvr_haptic_event(&input.haptics, SFVR_HAND_BOTH, SFVR_HAPTIC_UI_CLICK);
            if (vrMenuOpen) {
                closeVrMenu("Menu + View");
            }
            else {
                openVrMenu(r.navHeld, "Menu + View");
            }
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

        uint32_t navHeld = r.navHeld;
        const double sinceStart = monotonicSeconds() - sessionStartS;
        if ((setting("diag_menu_at") > 0.0) && (sinceStart > setting("diag_menu_at")) && (diagMenuReads < 2)) {
            // Diagnostic: the View short press, for two reads.
            if (diagMenuReads == 0) {
                dk64vr_logf("diag: opening the recomp menu");
            }

            diagMenuReads++;
            navHeld |= nav::ToggleMenu;
        }

        diagVrMenu(sinceStart);
        // While the VR menu is open it takes the pad and the pointer itself; recompui's own
        // menus (perhaps open under it) get nothing.
        sendNav(vrMenuOpen ? 0u : navHeld);
        updatePointer(s);
        if (vrMenuOpen) {
            feedVrMenu(s, navHeld);
        }

        if (settingsDirty && ((displayS - settingsDirtyS) > 0.5)) {
            requestSettingsSave();
        }

        diagSnap();
        if ((setting("diag_hud_seconds") > 0.0) && stereoLatched && (state.scene == DK64VR_SCENE_GAMEPLAY) && (displayS >= diagHudNextS)) {
            if (diagHudNextS > 0.0) {
                dk64vr_request_hud_diag();
                // An eye dump (with its HUD image) half a second later, while the counters show.
                if ((setting("dump_frame") > 0.0) && (dumpsDone < uint64_t(setting("dump_count")))) {
                    nextDumpFrame = stereoFrames + 36;
                }
            }

            diagHudNextS = displayS + setting("diag_hud_seconds");
        }

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
            // Diagnostic: Start held for 0.2 s at diag_start_at (the in-game pause menu).
            const double startAt = setting("diag_start_at");
            if ((startAt > 0.0) && (sinceStart >= startAt) && (sinceStart < startAt + 0.2)) {
                if (!diagStartLogged) {
                    diagStartLogged = true;
                    dk64vr_logf("diag: pressing Start");
                }

                latestInput.buttons |= n64::Start;
            }

            diagCamera(latestInput, state);
        }

        // Action-state log for unattended runs (no wearer can press anything; the log shows the
        // path is live): any change of held N64 buttons, at most a few lines a second.
        if (s.valid && (r.buttons != lastLoggedButtons) && ((displayS - lastButtonLogS) > 0.25)) {
            lastLoggedButtons = r.buttons;
            lastButtonLogS = displayS;
            dk64vr_logf("input: n64 buttons %04X stick %.2f %.2f", r.buttons, r.stickX, r.stickY);
        }
    }

    // Once a display frame, from the (smoothed) game view: the game camera's unwrapped heading
    // and the orbit the view needs (turnShownDelta).
    void updateTurn(const Mat4 &gameView) {
        const float raw = viewHeading(gameView);
        headingUnwrapped = headingValid ? (headingUnwrapped + wrapAngle(raw - headingRaw)) : raw;
        headingRaw = raw;
        headingValid = true;
        const dk64vr_game_state state = gameState();
        if (turnActive) {
            const float delta = turnTarget - headingUnwrapped;
            turnDelta = delta;
            turnShownDelta = delta;
            // Done when the game camera arrives. Given up when it takes too long after the last
            // turn input, when the game turns it (C-left/C-right), or outside play; the rest of
            // the offset then unwinds over 0.4 s instead of jumping.
            const bool settled = std::fabs(delta) < 0.004f;
            const bool timedOut = (displayS - turnStartS) > 8.0;
            const bool overtaken = (latestInput.buttons & (n64::CLeft | n64::CRight)) != 0;
            const bool leftPlay = (state.scene != DK64VR_SCENE_GAMEPLAY) || frozen;
            if (settled || timedOut || overtaken || leftPlay) {
                dk64vr_logf("turn: game camera %s after %.2f s (%.1f deg left)", settled ? "arrived" : (timedOut ? "timed out" : (overtaken ? "turned by the game" : "left play")),
                    displayS - turnStartS, delta * 180.0f / 3.1415926f);
                turnActive = false;
                turnDelta = 0.0f;
                turnReleaseFrom = (settled || leftPlay) ? 0.0f : delta;
                turnReleaseS = displayS;
                turnShownDelta = turnReleaseFrom;
            }
        }
        else if (turnReleaseFrom != 0.0f) {
            const float t = float(std::clamp((displayS - turnReleaseS) / 0.4, 0.0, 1.0));
            turnShownDelta = turnReleaseFrom * (1.0f - t * t * (3.0f - 2.0f * t));
            if (t >= 1.0f) {
                turnReleaseFrom = 0.0f;
                turnShownDelta = 0.0f;
            }
        }
        else {
            turnShownDelta = 0.0f;
        }
    }

    void applyTurn(float deg) {
        // Rotates the game camera's heading (the camera patch takes it through recomp_vr_take_turn,
        // dk64vr_control.h) by the step; the eye camera follows the game camera, so the world
        // turns about Donkey Kong. The view shows the new heading at once (updateTurn); the game
        // camera eases after it. Smooth turning calls this every frame with a small step.
        if (!turnActive) {
            // From the heading shown now (including any offset still unwinding).
            turnTarget = (headingValid ? headingUnwrapped : 0.0f) + turnShownDelta;
            turnReleaseFrom = 0.0f;
        }

        turnTarget -= deg * 3.1415926f / 180.0f;
        turnActive = true;
        turnStartS = displayS;

        turnRemainder += deg;
        const int whole = int(turnRemainder);
        turnRemainder -= float(whole);
        if (whole != 0) {
            dk64vr_request_turn(whole);
        }

        turnLogDeg += deg;
        if (std::fabs(deg) >= 5.0f) {
            dk64vr_logf("turn: %.0f deg", deg);
            blinkUntilS = displayS + setting("snap_blink");
        }
    }

    // Diagnostic: a snap turn right every diag_snap_seconds of stereo play, then the game
    // camera's heading for the next 12 display frames (direction and settle time).
    void diagSnap() {
        const double every = setting("diag_snap_seconds");
        const dk64vr_game_state state = gameState();
        if ((every <= 0.0) || !stereoLatched || (state.scene != DK64VR_SCENE_GAMEPLAY)) {
            return;
        }

        const float fx = state.cam_at[0] - state.cam_eye[0], fz = state.cam_at[2] - state.cam_eye[2];
        const float heading = std::atan2(-fx, -fz) * 180.0f / 3.1415926f;  // counter-clockwise from above
        if (diagSnapFrames >= 0) {
            dk64vr_logf("diag snap: +%d frames, game camera heading %.1f deg, shown %.1f deg, game frame %u", diagSnapFrames, heading, shownHeading * 180.0f / 3.1415926f, state.frame);
            diagSnapFrames = (diagSnapFrames >= 12) ? -1 : (diagSnapFrames + 1);
            return;
        }

        if (diagSnapNextS == 0.0) {
            diagSnapNextS = displayS + every;
        }
        else if (displayS >= diagSnapNextS) {
            diagSnapNextS = displayS + every;
            dk64vr_logf("diag snap: turning right %d deg from heading %.1f deg (a right turn lowers it)", int(setting("snap_angle")), heading);
            applyTurn(float(setting("snap_angle")));
            diagSnapFrames = 0;
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

    // ----------------------------------------------------------------------------------------
    // The VR settings menu (src/vr/menu, docs section 9a): Menu + View held 0.5 s. The game
    // pauses while it is open (setPaused), a stereo world freezes dimmed behind it (the recomp
    // menu path in updateStereoLatch: its context captures input), and the panel turns to face
    // the head and shows only the menu, over transparent black.

    menu::Facts menuFacts() const {
        menu::Facts f;
        f.displayHz = (periodS > 0.0) ? (1.0 / periodS) : 0.0;
        f.recommendedEye = recommendedEyeWidth;
        f.foveationLive = foveation.enabled();
        return f;
    }

    void openVrMenu(uint32_t navHeld, const char *why) {
        if (vrMenuOpen) {
            return;
        }

        if (!menu::uiReady()) {
            logOnce(menuMissing, "menu: the VR settings document is not loaded; Menu + View does nothing");
            return;
        }

        panelYawRad = quatYaw(headInOriginQ);  // placed in front of the head (standard.md section 3)
        if (!menu::uiOpen(settings, menuFacts(), displayS, navHeld)) {
            panelYawRad = 0.0f;
            return;
        }

        vrMenuOpen = true;
        vrMenuShown = true;
        menuPointerDown = false;
        const dk64vr_game_state state = gameState();
        dk64vr_logf("menu: VR settings open (%s; scene %s, %s, head yaw %.0f deg)", why, sceneName(state.scene),
            stereoLatched ? "stereo: world frozen behind it" : "panel", panelYawRad * 180.0f / 3.1415926f);
    }

    void closeVrMenu(const char *why) {
        if (!vrMenuOpen) {
            return;
        }

        menu::uiClose();
        vrMenuOpen = false;
        vrMenuShown = false;
        panelYawRad = 0.0f;
        menuPointerDown = false;
        mapper.ignoreHeld(lastSample);  // the B, View or chord press that closed it stays out of the game
        if (settingsDirty) {
            requestSettingsSave();
        }

        dk64vr_logf("menu: VR settings closed (%s)", why);
    }

    void feedVrMenu(const InputSample &s, uint32_t navHeld) {
        menu::FrameIn in;
        in.nowS = displayS;
        in.navHeld = navHeld | diagMenuNav;
        in.scroll = s.valid ? s.rStick[1] : 0.0f;
        in.pointerOn = menuPointerOn;
        in.pointerDown = menuPointerDown;
        in.px = menuPointerX;
        in.py = menuPointerY;
        menu::uiFrame(in, menuFacts());
        menu::FrameOut out = menu::uiTake();
        for (const menu::StructSettings::Change &c : out.changes) {
            dk64vr_settings_set(&settings, c.key.c_str(), c.value);
            dk64vr_logf("menu: %s = %g", c.key.c_str(), dk64vr_settings_get(&settings, c.key.c_str()));
            settingsDirty = true;
            settingsDirtyS = displayS;
        }

        if (out.recentre) {
            recentrePending = true;
            sfvr_haptic_event(&input.haptics, SFVR_HAND_BOTH, SFVR_HAPTIC_SYSTEM);
            dk64vr_logf("recentre: VR menu");
        }

        if (out.hover && s.valid) {
            sfvr_haptic_event(&input.haptics, dominantHand(), SFVR_HAPTIC_UI_HOVER);
        }

        menuLast = out;
        if (out.close) {
            closeVrMenu("closed in the menu");
        }
    }

    // vr.toml is written on its own thread (an fsync never stalls a display frame), half a second
    // after the last change and when the menu closes.
    void requestSettingsSave() {
        settingsDirty = false;
        {
            std::scoped_lock lock(saverMutex);
            saverCopy = settings;
            saverPending = true;
            if (!saverThread.joinable()) {
                saverThread = std::thread([this]() { saverLoop(); });
            }
        }

        saverCv.notify_one();
    }

    void saverLoop() {
        std::unique_lock lock(saverMutex);
        while (true) {
            saverCv.wait(lock, [this]() { return saverPending || saverStop; });
            if (saverPending) {
                const dk64vr_settings_t copy = saverCopy;
                saverPending = false;
                lock.unlock();
                const bool ok = dk64vr_settings_save(&copy);
                dk64vr_logf("settings: %s %s", ok ? "saved" : "could not save", copy.path);
                lock.lock();
                continue;
            }

            break;
        }
    }

    void stopSaver() {
        if (settingsDirty) {
            requestSettingsSave();
        }

        {
            std::scoped_lock lock(saverMutex);
            saverStop = true;
        }

        saverCv.notify_one();
        if (saverThread.joinable()) {
            saverThread.join();
        }
    }

    // Diagnostic (diag_vr_menu_at): a tour of the menu without a wearer, each step's panel dumped
    // with a layers line (and the stereo eyes once, just before it opens, for composites). Pad
    // presses and the pointer (aimed at an element the document reports, and one click) go
    // through the same paths as a wearer's. It never changes a setting: no Left/Right, and
    // Defaults is only armed.
    struct TourStep {
        const char *name;
        uint32_t nav;         // pressed at the step's start
        int extraDowns;       // more Down presses, 0.3 s apart
        const char *pointAt;  // the pointer aims at this data-act's centre ("" none)
        bool click;           // trigger at 40% of the step
        const char *act;      // run as a click would, at the start
    };

    void diagVrMenu(double sinceStart) {
        diagMenuNav = 0;
        diagPointerDown = false;
        const double at = setting("diag_vr_menu_at");
        if ((at <= 0.0) || tourDone) {
            diagPointerU = -1.0f;
            return;
        }

        static const TourStep fullTour[] = {
            { "c-open", 0, 0, "", false, nullptr },
            { "c-pad", nav::Down, 0, "", false, nullptr },
            { "c-pointer", 0, 0, "set:turn_mode:1", false, nullptr },
            { "b-comfort", 0, 0, "all", true, nullptr },
            { "b-controls", nav::TabRight, 0, "", false, nullptr },
            { "b-hud", nav::TabRight, 0, "", false, nullptr },
            { "b-display", nav::TabRight, 0, "", false, nullptr },
            { "b-room", nav::TabRight, 0, "", false, nullptr },
            { "b-focus", nav::TabRight, 3, "", false, nullptr },
            { "b-scroll", nav::Down, 5, "", false, nullptr },
            { "b-pointer", 0, 0, "def", false, nullptr },
            { "b-armed", 0, 0, "", false, "def" },
            { "c-back", nav::Back, 0, "", false, nullptr },
            { "closed", nav::Back, 0, "", false, nullptr },
        };
        const int tabOnly = int(setting("diag_vr_menu_tab"));
        static const char *const tabActs[] = { "tab:0", "tab:1", "tab:2", "tab:3", "tab:4" };
        const TourStep tabTour[] = {
            { "c-open", 0, 0, "", false, nullptr },
            { "b-tab", 0, 0, "", false, (tabOnly >= 0 && tabOnly <= 4) ? tabActs[tabOnly] : "tab:0" },
        };
        const TourStep *steps = (tabOnly >= 0) ? tabTour : fullTour;
        const int count = (tabOnly >= 0) ? 2 : int(sizeof(fullTour) / sizeof(fullTour[0]));

        if (tourStep < 0) {
            // In stereo play if the run gets there within a minute, so the frozen world shows.
            if ((sinceStart < at) || (!stereoLatched && (sinceStart < at + 60.0))) {
                return;
            }

            if (stereoLatched && !menuEyeDumpAsked) {
                menuEyeDumpAsked = true;
                menuEyeDump = true;  // the world as it will freeze, for composites
                tourStepS = displayS;
                return;
            }

            if (menuEyeDumpAsked && (displayS - tourStepS) < 0.3) {
                return;
            }

            tourStep = 0;
            tourStepS = displayS;
            tourActed = false;
            dk64vr_logf("diag menu: tour begins (%s, %d steps)", stereoLatched ? "stereo play" : "panel", count);
        }

        const double stepLen = std::max(1.0, setting("diag_vr_menu_step"));
        double t = displayS - tourStepS;
        if (t >= stepLen) {
            if (tourStep + 1 >= count) {
                if (tabOnly < 0) {
                    tourDone = true;
                    diagPointerU = -1.0f;
                    menu::uiLocate("");
                    dk64vr_logf("diag menu: tour done (menu %s)", vrMenuOpen ? "still open" : "closed");
                }
                return;  // the tab view stays up for the rest of the run
            }

            tourStep++;
            tourStepS = displayS;
            tourActed = false;
            t = 0.0;
        }

        const TourStep &step = steps[tourStep];
        if (!tourActed) {
            tourActed = true;
            if (tourStep == 0) {
                openVrMenu(0, "diagnostic tour");
            }

            if (step.act != nullptr) {
                menu::uiAct(step.act);
            }

            menu::uiLocate(step.pointAt);
            dk64vr_logf("diag menu: step %d %s", tourStep, step.name);
        }

        if (step.nav && (t < 0.12)) {
            diagMenuNav |= step.nav;
        }

        for (int i = 0; i < step.extraDowns; i++) {
            const double from = 0.3 * (i + 1);
            if ((t >= from) && (t < from + 0.12)) {
                diagMenuNav |= nav::Down;
            }
        }

        if ((step.pointAt[0] != '\0') && menuLast.locateValid) {
            diagPointerU = menuLast.locateX / float(PanelWidth);
            diagPointerV = menuLast.locateY / float(PanelHeight);
        }
        else if (step.pointAt[0] == '\0') {
            diagPointerU = -1.0f;
        }

        diagPointerDown = step.click && (t >= 0.4 * stepLen) && (t < 0.4 * stepLen + 0.15);
        const double dumpAt = 0.75 * stepLen;
        if ((t >= dumpAt) && (tourDumped != tourStep)) {
            tourDumped = tourStep;
            menuDumpName = step.name;
            menuPanelDump = tourStep;
            dk64vr_logf("diag menu: %s: screen %s, tab %d, focus %s, pointer %s", step.name, (menuLast.screen == menu::Screen::Quick) ? "quick" : "full",
                int(menuLast.tab), menuLast.focus.c_str(), (diagPointerU >= 0.0f) ? "on" : "off");
        }
    }

    Pose panelPose() const {
        const Pose turned = poseMul(origin, Pose { quatFromYaw(panelYawRad), {} });
        return poseMul(turned, Pose { Quat(), { 0.0f, 0.0f, -float(setting("panel_distance")) } });
    }

    // The menu pointer: the dominant hand's aim ray (aim/pose -Z, lesson 41) against the panel.
    void updatePointer(const InputSample &s) {
        pointer.visible = false;
        menuPointerOn = false;
        const int hand = dominantHand();
        Pose aim;
        bool haveAim = s.valid && input.aimPose(hand, aim);
        if (vrMenuOpen) {
            // The trigger, whether or not the ray is on the panel (a drag that leaves it and
            // comes back is not a new click).
            const bool down = s.valid ? triggerClick(menuPointerDown, (hand == 0) ? s.lTrigger : s.rTrigger) : diagPointerDown;
            if (down && !menuPointerDown && s.valid) {
                sfvr_haptic_event(&input.haptics, hand, SFVR_HAPTIC_UI_CLICK);
            }

            menuPointerDown = down;
        }
        else {
            menuPointerDown = false;
        }

        const bool tourAim = (diagPointerU >= 0.0f) && vrMenuOpen;
        if (!haveAim && ((setting("diag_pointer") != 0.0) || tourAim)) {
            // Diagnostic: a hand 25 cm right of and 30 cm below the recentre origin, aimed at a
            // point right of and above the panel's centre (or where the menu tour points).
            const Pose panel = panelPose();
            const Vec3 handPos = quatRotate(panel.q, Vec3 { 0.25f, -0.3f, 0.0f });
            aim.p = { origin.p.x + handPos.x, origin.p.y + handPos.y, origin.p.z + handPos.z };
            const float panelW = float(setting("panel_width"));
            const float panelH = panelW * float(PanelHeight) / float(PanelWidth);
            const Vec3 target = tourAim ? quatRotate(panel.q, Vec3 { (diagPointerU - 0.5f) * panelW, (0.5f - diagPointerV) * panelH, 0.0f })
                                        : quatRotate(panel.q, Vec3 { 0.15f * panelW, 0.1f, 0.0f });
            const Vec3 to = normalize({ panel.p.x + target.x - aim.p.x, panel.p.y + target.y - aim.p.y, panel.p.z + target.z - aim.p.z });
            const Vec3 up = { 0.0f, 1.0f, 0.0f };
            const Vec3 x = normalize(cross(to, up));
            const Vec3 y = cross(x, to);
            aim.q = quatFromBasis(x, y, Vec3 { -to.x, -to.y, -to.z });
            haveAim = true;
        }

        if (!haveAim || !panelShownLast || !(uiMouse || vrMenuOpen || (setting("diag_pointer") != 0.0)) || (panelChain.width == 0)) {
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
        if (vrMenuOpen) {
            // The VR menu hit-tests the pointer itself (dk64vr_menu_ui.cpp).
            menuPointerOn = true;
            menuPointerX = u * float(panelChain.width);
            menuPointerY = v * float(panelChain.height);
            return;
        }

        if ((px != pointerPx) || (py != pointerPy)) {
            pointerPx = px;
            pointerPy = py;
            SDL_Event event = {};
            event.type = SDL_MOUSEMOTION;
            event.motion.x = px;
            event.motion.y = py;
            recompui::queue_event(event);
        }

        if (!s.valid) {
            return;  // a synthetic pointer moves and shows, never clicks
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


    // The static UI image (one 128x64 sRGB image, filled once; lesson 38): the pointer's dot
    // (x 0-63), the beam's cross-section (x 64-95) and black (x 96-127) in FadeLevels bands of 4
    // rows, alpha 0 to 1 (the snap blink, the transition fade and the pause dim).
    static const uint32_t UiWidth = 128, UiHeight = 64;
    static const int FadeLevels = 16;

    void createUiImage() {
        if (!frameWorker) {
            return;
        }

        createSwapchain(uiChain, UiWidth, UiHeight, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, plume::RenderFormat::R8G8B8A8_UNORM, false, false, XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT);
        if (uiChain.handle == XR_NULL_HANDLE) {
            return;
        }

        // Unpremultiplied RGBA (the layers say so). Colours are sRGB-encoded bytes.
        std::vector<uint8_t> pixels(UiWidth * UiHeight * 4, 0);
        auto put = [&](uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, float a) {
            uint8_t *p = &pixels[(y * UiWidth + x) * 4];
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = uint8_t(std::lround(std::fmax(0.0f, std::fmin(1.0f, a)) * 255.0f));
        };

        for (uint32_t y = 0; y < UiHeight; y++) {
            for (uint32_t x = 0; x < 64; x++) {
                // Dot: a light disc with a dark rim, so it shows on light and dark menus.
                const float dx = float(x) + 0.5f - 32.0f, dy = float(y) + 0.5f - 32.0f;
                const float r = std::sqrt(dx * dx + dy * dy);
                if (r < 22.0f) {
                    put(x, y, 235, 250, 255, std::fmin(1.0f, 22.0f - r));
                }
                else {
                    put(x, y, 20, 30, 40, std::fmin(1.0f, 31.0f - r) * 0.85f);
                }
            }

            for (uint32_t x = 64; x < 96; x++) {
                // Beam: bright core, soft edges across the width.
                const float u = (float(x - 64) + 0.5f) / 32.0f;
                const float a = std::fmax(0.0f, 1.0f - std::fabs(u - 0.5f) * 2.0f);
                put(x, y, 200, 235, 255, a * a * 0.9f);
            }

            for (uint32_t x = 96; x < 128; x++) {
                put(x, y, 0, 0, 0, float(y / 4) / float(FadeLevels - 1));
            }
        }

        if (XR_FAILED(acquireImage(uiChain))) {
            dk64vr_logpf("[openxr]", "UI image acquire failed; no pointer");
            destroySwapchain(uiChain);
            return;
        }

        std::unique_ptr<plume::RenderBuffer> upload = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(pixels.size()));
        void *mapped = upload->map();
        memcpy(mapped, pixels.data(), pixels.size());
        upload->unmap();
        plume::VulkanTexture *image = uiChain.textures[uiChain.index].get();
        image->textureLayout = plume::RenderTextureLayout::COLOR_WRITE;
        image->barrierStages = plume::RenderBarrierStage::GRAPHICS;
        plume::RenderCommandList *commandList = frameWorker->commandList.get();
        commandList->begin();
        commandList->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(image, plume::RenderTextureLayout::COPY_DEST));
        commandList->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(image),
            plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), plume::RenderFormat::R8G8B8A8_UNORM, UiWidth, UiHeight, 1, UiWidth));
        commandList->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image, plume::RenderTextureLayout::COLOR_WRITE));
        commandList->end();
        frameWorker->execute();
        frameWorker->wait();
        {
            std::scoped_lock lock(*queueMutex);
            XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(uiChain.handle, &releaseInfo);
        }

        uiReady = true;
        dk64vr_logf("UI image ready (pointer, blink)");
    }

    XrCompositionLayerQuad hudQuad() const {
        const float distance = float(setting("hud_distance"));
        const float width = 2.0f * distance * std::tan(20.0f * 3.1415926f / 180.0f) * float(setting("hud_scale")) / 100.0f;
        Pose head;
        head.q = quatMul(origin.q, headInOriginQ);
        head.p = { 0.5f * (eyeSubmitPose[0].p.x + eyeSubmitPose[1].p.x), 0.5f * (eyeSubmitPose[0].p.y + eyeSubmitPose[1].p.y), 0.5f * (eyeSubmitPose[0].p.z + eyeSubmitPose[1].p.z) };
        Quat facing = head.q;
        const dk64vr_game_state state = gameState();
        if (state.scene == DK64VR_SCENE_FIRST_PERSON) {
            // The crosshair is the game camera's aim: the recentred heading (the game camera's)
            // with the game camera's pitch.
            const float dx = state.cam_at[0] - state.cam_eye[0], dy = state.cam_at[1] - state.cam_eye[1], dz = state.cam_at[2] - state.cam_eye[2];
            const float pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
            facing = quatMul(origin.q, Quat { std::sin(0.5f * pitch), 0.0f, 0.0f, std::cos(0.5f * pitch) });
        }

        XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        quad.space = localSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = hudChain.handle;
        quad.subImage.imageRect.offset = { 0, 0 };
        quad.subImage.imageRect.extent = { int32_t(hudChain.width), int32_t(hudChain.height) };
        quad.pose = toXr(noRollPose(facing, head.p, distance));
        quad.size = { width, width * 0.75f };
        if (cinemaActive && cinemaAnchored) {
            quad.pose = toXr(cinemaHudPose);
            quad.size = { cinemaHudSize[0], cinemaHudSize[1] };
        }

        return quad;
    }

    // fadeLevel >= 0 selects one band of the black column (2 rows in its middle, so filtering never
    // reaches a neighbour); -1 the whole height.
    XrCompositionLayerQuad uiQuad(XrSpace space, const Pose &pose, int32_t x, int32_t width, float sizeX, float sizeY, int fadeLevel = -1) const {
        XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        quad.space = space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = uiChain.handle;
        quad.subImage.imageRect.offset = { x, (fadeLevel >= 0) ? (fadeLevel * 4 + 1) : 0 };
        quad.subImage.imageRect.extent = { width, (fadeLevel >= 0) ? 2 : int32_t(UiHeight) };
        quad.pose = toXr(pose);
        quad.size = { sizeX, sizeY };
        return quad;
    }

    // The pointer, composited above everything (standard.md section 3, lessons 80 and 83): the
    // dot lies flat on the panel, lifted 1 cm towards the viewer; the beam stops 2 cm short of it.
    void addPointerLayers(std::vector<XrCompositionLayerQuad> &quads) {
        if (!uiReady || !pointer.visible) {
            return;
        }

        const Vec3 normal = quatRotate(pointer.surface.q, Vec3 { 0.0f, 0.0f, 1.0f });
        Pose dot = pointer.surface;
        dot.p = { pointer.hit.x + 0.01f * normal.x, pointer.hit.y + 0.01f * normal.y, pointer.hit.z + 0.01f * normal.z };
        const float beamLength = pointer.length - 0.02f;
        if (beamLength > 0.05f) {
            const Vec3 end = { pointer.origin.x + beamLength * pointer.dir.x, pointer.origin.y + beamLength * pointer.dir.y, pointer.origin.z + beamLength * pointer.dir.z };
            const Vec3 eye = { 0.5f * (eyeSubmitPose[0].p.x + eyeSubmitPose[1].p.x), 0.5f * (eyeSubmitPose[0].p.y + eyeSubmitPose[1].p.y), 0.5f * (eyeSubmitPose[0].p.z + eyeSubmitPose[1].p.z) };
            quads.push_back(uiQuad(localSpace, beamPose(pointer.origin, end, eye), 64, 32, 0.006f, beamLength));
        }

        // The dot shrinks while the trigger holds a VR menu control (the mockup's pressed state).
        const float dotSize = 0.018f * float(setting("panel_distance")) * ((vrMenuOpen && menuPointerDown) ? 0.75f : 1.0f);
        quads.push_back(uiQuad(localSpace, dot, 0, 64, dotSize, dotSize));
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
        if (hudChain.acquired) {
            XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(hudChain.handle, &releaseInfo);
            hudChain.acquired = false;
            hudReleasedOnce = true;
        }

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

    void recordReadback(plume::RenderCommandList *commandList, plume::VulkanTexture *image, const Swapchain &chain, Readback &readback, const std::string &name, int64_t index = -1) {
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
        snprintf(suffix, sizeof(suffix), "-%03" PRIu64 "-yaw%d.png", (index >= 0) ? uint64_t(index) : dumpsDone, int(std::lround(diagYawDeg)));
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
            writePngAsync(readback.path, static_cast<const uint8_t *>(data), readback.width, readback.height, readback.width * 4, readback.bgra, readback.keepAlpha);
            readback.buffer->unmap();
        }
    }

    // Diagnostics: every layer's geometry in LOCAL space (quads in VIEW space are marked), for
    // tools/steam_frame/composite.py to rebuild what SteamVR composites from the image dumps.
    // Every 2 s while panel dumps run, and with every eye dump.
    void logLayers(const std::vector<const XrCompositionLayerBaseHeader *> &layers, const XrCompositionLayerProjectionView *views) {
        const bool periodic = (setting("dump_panel_frame") > 0.0) && ((frameCount % 144) == 0);
        if (!periodic && !layersLogDue) {
            return;
        }

        layersLogDue = false;
        std::string line = "layers: frame " + std::to_string(frameCount);
        char buffer[256];
        for (const XrCompositionLayerBaseHeader *layer : layers) {
            if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                for (int eye = 0; eye < 2; eye++) {
                    const XrPosef &p = views[eye].pose;
                    const XrFovf &f = views[eye].fov;
                    snprintf(buffer, sizeof(buffer), " | proj%d p %.4f %.4f %.4f q %.5f %.5f %.5f %.5f fov %.4f %.4f %.4f %.4f rect %d %d %d %d", eye,
                        p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w,
                        f.angleLeft, f.angleRight, f.angleUp, f.angleDown, views[eye].subImage.imageRect.offset.x, views[eye].subImage.imageRect.offset.y,
                        views[eye].subImage.imageRect.extent.width, views[eye].subImage.imageRect.extent.height);
                    line += buffer;
                }
            }
            else if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                const XrCompositionLayerQuad *q = reinterpret_cast<const XrCompositionLayerQuad *>(layer);
                const bool blended = (q->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
                const char *name = (q->subImage.swapchain == panelChain.handle) ? (blended ? "panelpm" : "panel") : ((q->subImage.swapchain == hudChain.handle) ? "hud" : "ui");
                snprintf(buffer, sizeof(buffer), " | %s%s p %.4f %.4f %.4f q %.5f %.5f %.5f %.5f size %.4f %.4f rect %d %d %d %d", name, (q->space == viewSpace) ? "@view" : "",
                    q->pose.position.x, q->pose.position.y, q->pose.position.z, q->pose.orientation.x, q->pose.orientation.y, q->pose.orientation.z, q->pose.orientation.w,
                    q->size.width, q->size.height, q->subImage.imageRect.offset.x, q->subImage.imageRect.offset.y, q->subImage.imageRect.extent.width, q->subImage.imageRect.extent.height);
                line += buffer;
            }
        }

        dk64vr_logf("%s", line.c_str());
    }

    void logDumpState() {
        layersLogDue = true;
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
    bool haveVisibilityMask = false;
    PFN_xrGetVisibilityMaskKHR getVisibilityMask = nullptr;
    // The runtime's hidden triangles an eye (view-plane tangents), and the same in the eye image's
    // normalized device coordinates for the FOV they were last converted with.
    std::vector<XrVector2f> hiddenTangents[2];
    std::vector<float> hiddenNdc[2];
    FovTangents hiddenNdcFov[2];
    bool hiddenMeshValid[2] = {};
    bool hiddenMeshLogged = false;
    bool haveEyeGaze = false;
    bool valveFdmDisabled = true;
    XrFoveation foveation;
    bool foveatedEye[2] = {};
    bool foveationDumped[2] = {};
    int gazeState = -1;
    uint64_t gazeSwitches = 0;
    uint32_t gazeSwitchesWindow = 0;
    uint64_t perfWindowFoveatedPasses = 0;
    bool runtimeEyesValid = false;
    Pose runtimeEyePose[2];
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
    Swapchain uiChain;
    Swapchain hudChain;
    bool hudShown = false;
    bool hudReleasedOnce = false;
    bool hudLogged = false;
    bool hudDumpDue = false;
    uint64_t hudDumpIndex = 0;
    uint32_t lastHudCalls = 0;
    uint64_t hudSignatureShown = 0;
    uint64_t hudGameFrames = 0;
    uint64_t hudLastDrawnFrame = 0;
    uint32_t hudSkipsSame = 0, hudSkipsRate = 0;
    uint32_t hudPartChanges[7] = {};
    double hudCpuSum = 0.0, hudGpuSum = 0.0;
    uint32_t hudPasses = 0;
    Readback hudReadback = makeAlphaReadback();
    static Readback makeAlphaReadback() {
        Readback r;
        r.keepAlpha = true;
        return r;
    }
    std::atomic<bool> layersLogDue = false;
    bool uiReady = false;
    std::vector<XrCompositionLayerQuad> extraQuadStorage;
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
    RT64::VrEyeStats eyeStats[2];
    uint64_t statFrames = 0;
    double statSum[2][13] = {};
    double statMax[2] = {};

    Pose origin;
    bool recentrePending = true;
    Pose eyeInHead[2];
    Pose eyeSubmitPose[2];
    FovTangents eyeFov[2];
    XrFovf eyeXrFov[2] = {};
    float diagYawDeg = 0.0f;

    TickScheduler scheduler;
    CameraSmoother camSmoother;
    CameraStepStats camRawStats, camOutStats, camShownStats;  // shown: after the turn orbit
    double diagTurnStartS = 0.0;
    uint64_t shownStatsFrame = UINT64_MAX;
    bool diagTurnHeld = false;
    Mat4 camCorrection;
    uint64_t camCorrectionFrame = UINT64_MAX;
    double camLastSampleS = 0.0;
    int32_t camTickScene = DK64VR_SCENE_UNKNOWN;
    uint32_t camWindowCuts = 0;
    double camMatchPos = 0.0, camMatchDeg = 0.0;
    CameraSmoothing camLevelLogged = CameraSmoothing(-1);
    double diagCameraStartS = 0.0;
    int diagCameraPhase = -1;
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
    bool frozen = false;          // transitions and pause: the last stereo images held
    bool pausePanel = false;      // the pause menu's panel over the frozen world
    bool menuPanel = false;       // a recomp menu's panel over the frozen world
    uint32_t freezeLiveCount = 0, pauseCount = 0;
    bool haveFrozenViews = false;
    XrCompositionLayerProjectionView frozenViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    float fadeAlpha = 0.0f, fadeTarget = 0.0f;
    bool cinemaActive = false;
    bool cinemaAnchored = false;
    float cinemaLoggedTop = -1.0f, cinemaLoggedViewport = -1.0f;
    uint32_t cinemaLatchCount = 0;
    Pose cinemaScreen;
    Pose cinemaHudPose;
    float cinemaHudSize[2] = { 3.6f, 2.7f };
    FovTangents cinemaGameTan = { -0.552f, 0.552f, 0.414f, -0.414f };
    Quat headInOriginQ;
    float turnRemainder = 0.0f;
    bool turnActive = false;
    float turnTarget = 0.0f;          // unwrapped heading the view shows while turning
    float turnDelta = 0.0f;           // what the game camera still has to turn
    float turnShownDelta = 0.0f;      // the orbit applied this frame
    float turnReleaseFrom = 0.0f;     // an abandoned turn's offset, unwinding
    double turnReleaseS = 0.0;
    uint64_t turnFrame = UINT64_MAX;
    bool headingValid = false;
    float headingRaw = 0.0f, headingUnwrapped = 0.0f;
    float turnLogDeg = 0.0f;          // smooth turning: degrees requested in the [vr-cam] window
    double turnStartS = 0.0;
    float shownHeading = 0.0f;
    double blinkUntilS = 0.0;
    struct PointerState {
        bool visible = false;
        Vec3 origin, dir, hit;
        float length = 0.0f;
        Pose surface;
    } pointer;
    bool pointerDown = false;
    uint32_t diagMenuReads = 0;
    bool diagStartLogged = false;
    double diagHudNextS = 0.0;
    double diagSnapNextS = 0.0;
    int diagSnapFrames = -1;
    int pointerPx = -1, pointerPy = -1;

    // The VR settings menu.
    bool vrMenuOpen = false;
    std::atomic<bool> vrMenuShown = false;     // read by the present thread (panelAcquire)
    bool panelAcquiredUiOnly = false;          // present thread: the image being drawn
    std::atomic<bool> panelImageUiOnly = false;  // the last released panel image is menu only
    float panelYawRad = 0.0f;                  // the panel's turn from the recentre forward
    bool menuPointerOn = false, menuPointerDown = false;
    float menuPointerX = 0.0f, menuPointerY = 0.0f;
    InputSample lastSample;
    menu::FrameOut menuLast;
    bool menuMissing = false;
    int recommendedEyeWidth = 0;
    bool settingsDirty = false;
    double settingsDirtyS = 0.0;
    std::thread saverThread;
    std::mutex saverMutex;
    std::condition_variable saverCv;
    dk64vr_settings_t saverCopy = {};
    bool saverPending = false, saverStop = false;
    // Its diagnostic tour.
    int tourStep = -1, tourDumped = -1;
    double tourStepS = 0.0;
    bool tourActed = false, tourDone = false;
    bool menuEyeDump = false, menuEyeDumpAsked = false;
    uint32_t diagMenuNav = 0;
    float diagPointerU = -1.0f, diagPointerV = 0.0f;
    bool diagPointerDown = false;
    std::atomic<int> menuPanelDump = -1;
    std::string menuDumpName;

    bool hudFailed = false;
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

bool hostActive() {
    return host != nullptr;
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

void hostUiInit() {
    if (host != nullptr) {
        host->uiInit();
    }
}

void hostSetRumble(int channel, bool on) {
    if (host != nullptr) {
        host->setRumble(channel, on);
    }
}

}

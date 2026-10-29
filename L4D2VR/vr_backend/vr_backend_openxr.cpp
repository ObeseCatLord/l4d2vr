// OpenXR backend (OpenXR port M2: frames only).
//
// The mod keeps talking OpenVR shapes (vr_backend.h); this file answers them
// with OpenXR on DXVK's own Vulkan instance and device (XR_KHR_vulkan_enable):
//
//   * Instance + system: created once, before Direct3DCreate9 needs the Vulkan
//     extension lists (L4D2VR_XrGetVulkan*Extensions), from the bundled
//     openxr_loader.dll next to d3d9.dll. Under Proton the loader reaches
//     wineopenxr, which only works once steam.exe has finished its OpenVR
//     init (HKCU\Software\Wine\VR state = 1), so we wait for that first.
//   * Session: created when DXVK has created its device
//     (L4D2VR_VrBackendDeviceCreated), on the physical device the runtime asked
//     for (DXVK pins it as adapter 0). An event thread pumps xrPollEvent and
//     begins/ends the session on READY/STOPPING.
//   * Frames (openxr_frame_ring.h): WaitGetPoses = xrWaitFrame on the calling
//     thread (pose waiter, main thread or loading pump); SubmitExplicitTimingData
//     = xrBeginFrame; Submit = copy the eye image into this eye's swapchain;
//     PostPresentHandoff = xrEndFrame with a projection layer carrying the pose
//     the eye was rendered with. A waited frame nobody submitted is closed by the
//     next WaitGetPoses with the last submitted layers, so the runtime keeps
//     receiving whole frames while the game is busy or stalled (loading pump).
//   * Queue rule: every xr call that may use DXVK's VkQueue (begin/end frame,
//     swapchain acquire/wait/release, session begin/end) runs under DXVK's
//     submission lock. The submit-side calls arrive with the mod already holding
//     it (LockPreparedSubmissionQueue / LockSubmissionQueue); WaitGetPoses and
//     the event thread take it themselves through DXVK's callbacks. Lock order:
//     DXVK queue lock, then m_Mutex.
//   * Controllers and input (M3): openxr_input_runtime.inl. Controllers are
//     pseudo devices 1 (left) and 2 (right) whose poses are the OpenVR raw
//     pose rebuilt from the grip pose; the IVRInput facade maps the mod's
//     actions onto OpenXR actions with per-profile suggested bindings.
//   * Overlays are kept as state but not composited yet (M4).

#include "vr_backend.h"
#include "openxr_frame_ring.h"
#include "openxr_pose_math.h"
#include "openxr_loader_probe.h"
#include "openxr_input_profiles.h"
#include "openxr_input_logic.h"

#include "../game.h"

#include <Windows.h>

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
#undef VK_USE_PLATFORM_WIN32_KHR

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace l4d2vr_backend
{
namespace
{
    namespace xrm = l4d2vr_openxr;

    // ---- OpenXR function table ---------------------------------------------
#define L4D2VR_XR_GLOBAL_FUNCS(X) \
    X(xrEnumerateInstanceExtensionProperties) X(xrEnumerateApiLayerProperties) X(xrCreateInstance)
#define L4D2VR_XR_INSTANCE_FUNCS(X) \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrResultToString) X(xrGetSystem) X(xrGetSystemProperties) \
    X(xrEnumerateViewConfigurationViews) X(xrEnumerateEnvironmentBlendModes) X(xrCreateSession) X(xrDestroySession) \
    X(xrBeginSession) X(xrEndSession) X(xrPollEvent) X(xrEnumerateReferenceSpaces) X(xrCreateReferenceSpace) \
    X(xrLocateSpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) \
    X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews) \
    X(xrGetVulkanInstanceExtensionsKHR) X(xrGetVulkanDeviceExtensionsKHR) X(xrGetVulkanGraphicsDeviceKHR) \
    X(xrGetVulkanGraphicsRequirementsKHR) \
    X(xrStringToPath) X(xrPathToString) X(xrCreateActionSet) X(xrCreateAction) \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrCreateActionSpace) X(xrSyncActions) \
    X(xrGetActionStateFloat) X(xrGetActionStateVector2f) X(xrApplyHapticFeedback) X(xrGetCurrentInteractionProfile) \
    X(xrEnumerateBoundSourcesForAction)

    struct XrApi
    {
        PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
#define L4D2VR_XR_DECL(name) PFN_##name name = nullptr;
        L4D2VR_XR_GLOBAL_FUNCS(L4D2VR_XR_DECL)
        L4D2VR_XR_INSTANCE_FUNCS(L4D2VR_XR_DECL)
        L4D2VR_XR_DECL(xrConvertWin32PerformanceCounterToTimeKHR)
        L4D2VR_XR_DECL(xrGetDisplayRefreshRateFB)
#undef L4D2VR_XR_DECL
    };

    // ---- Vulkan function table (DXVK's own loader) --------------------------
#define L4D2VR_VK_INSTANCE_FUNCS(X) \
    X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties)
#define L4D2VR_VK_DEVICE_FUNCS(X) \
    X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) \
    X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkUnmapMemory)

    struct VkApi
    {
#define L4D2VR_VK_DECL(name) PFN_##name name = nullptr;
        L4D2VR_VK_INSTANCE_FUNCS(L4D2VR_VK_DECL)
        L4D2VR_VK_DEVICE_FUNCS(L4D2VR_VK_DECL)
#undef L4D2VR_VK_DECL
    };

    constexpr uint32_t kHmd = vr::k_unTrackedDeviceIndex_Hmd;
    constexpr uint32_t kLeftHand = 1;   // pseudo device indices (XrInputRuntime::kDeviceLeft/Right)
    constexpr uint32_t kRightHand = 2;

    const char* SessionStateName(XrSessionState state)
    {
        switch (state)
        {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
        }
    }

    xrm::Pose ToPose(const XrPosef& p)
    {
        xrm::Pose pose;
        pose.orientation = { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w };
        pose.position = { p.position.x, p.position.y, p.position.z };
        return pose;
    }

    XrPosef ToXrPose(const xrm::Pose& p)
    {
        XrPosef pose{};
        pose.orientation = { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w };
        pose.position = { p.position.x, p.position.y, p.position.z };
        return pose;
    }

    XrFovf ToXrFov(const xrm::Fov& f) { return XrFovf{ f.angleLeft, f.angleRight, f.angleUp, f.angleDown }; }
    xrm::Fov ToFov(const XrFovf& f) { return xrm::Fov{ f.angleLeft, f.angleRight, f.angleUp, f.angleDown }; }

    std::string ModuleDirectory()
    {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&ModuleDirectory), &self);
        char path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
        std::string dir(path, n);
        const size_t slash = dir.find_last_of("\\/");
        return slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
    }

    bool IsWine()
    {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }

    std::string EnvValue(const char* name)
    {
        char buffer[2048] = {};
        const DWORD n = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
        return (n > 0 && n < sizeof(buffer)) ? std::string(buffer, n) : std::string();
    }

#include "openxr_input_runtime.inl"

    // ---- The runtime ---------------------------------------------------------
    class OpenXrRuntime;

    class XrSystemFacade final : public IVrSystem
    {
    public:
        explicit XrSystemFacade(OpenXrRuntime& rt) : m_Rt(rt) {}
        void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) override;
        void GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) override;
        vr::HmdMatrix34_t GetEyeToHeadTransform(vr::EVREye eEye) override;
        void GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow,
            vr::TrackedDevicePose_t* pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount) override;
        vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole unDeviceType) override;
        vr::ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t unDeviceIndex) override;
        bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t unDeviceIndex) override;
        float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop,
            vr::ETrackedPropertyError* pError) override;
        int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop,
            vr::ETrackedPropertyError* pError) override;
        uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop,
            char* pchValue, uint32_t unBufferSize, vr::ETrackedPropertyError* pError) override;
        void TriggerHapticPulse(vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId, unsigned short usDurationMicroSec) override;
        bool TriggerHapticVibration(vr::TrackedDeviceIndex_t unControllerDeviceIndex, float durationSeconds, float frequency,
            float amplitude) override;

    private:
        OpenXrRuntime& m_Rt;
    };

    class XrCompositorFacade final : public IVrCompositor
    {
    public:
        explicit XrCompositorFacade(OpenXrRuntime& rt) : m_Rt(rt) {}
        vr::ETrackingUniverseOrigin GetTrackingSpace() override;
        vr::EVRCompositorError WaitGetPoses(vr::TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount,
            vr::TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) override;
        vr::EVRCompositorError Submit(vr::EVREye eEye, const vr::Texture_t* pTexture,
            const vr::VRTextureBounds_t* pBounds, vr::EVRSubmitFlags nSubmitFlags) override;
        void ClearLastSubmittedFrame() override;
        void PostPresentHandoff() override;
        bool GetFrameTiming(vr::Compositor_FrameTiming* pTiming, uint32_t unFramesAgo) override;
        float GetFrameTimeRemaining() override;
        void SetExplicitTimingMode(vr::EVRCompositorTimingMode eTimingMode) override;
        vr::EVRCompositorError SubmitExplicitTimingData() override;

    private:
        OpenXrRuntime& m_Rt;
    };

    // M2: overlays keep their OpenVR-shaped state (so visibility checks and
    // transforms read back what was set) but nothing is composited yet (M4).
    class XrOverlayFacade final : public IVrOverlay
    {
    public:
        vr::EVROverlayError FindOverlay(const char* pchOverlayKey, vr::VROverlayHandle_t* pOverlayHandle) override
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!pchOverlayKey || !pOverlayHandle) return vr::VROverlayError_InvalidParameter;
            for (const auto& it : m_Overlays)
            {
                if (it.second.key == pchOverlayKey)
                {
                    *pOverlayHandle = it.first;
                    return vr::VROverlayError_None;
                }
            }
            *pOverlayHandle = vr::k_ulOverlayHandleInvalid;
            return vr::VROverlayError_UnknownOverlay;
        }
        vr::EVROverlayError CreateOverlay(const char* pchOverlayKey, const char* pchOverlayName, vr::VROverlayHandle_t* pOverlayHandle) override
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!pchOverlayKey || !pOverlayHandle) return vr::VROverlayError_InvalidParameter;
            for (const auto& it : m_Overlays)
                if (it.second.key == pchOverlayKey)
                    return vr::VROverlayError_KeyInUse;
            const vr::VROverlayHandle_t handle = ++m_NextHandle;
            OverlayState& state = m_Overlays[handle];
            state.key = pchOverlayKey;
            state.name = pchOverlayName ? pchOverlayName : "";
            *pOverlayHandle = handle;
            return vr::VROverlayError_None;
        }
        vr::EVROverlayError DestroyOverlay(vr::VROverlayHandle_t ulOverlayHandle) override
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_Overlays.erase(ulOverlayHandle) ? vr::VROverlayError_None : vr::VROverlayError_InvalidHandle;
        }
        vr::EVROverlayError SetOverlayFlag(vr::VROverlayHandle_t h, vr::VROverlayFlags eOverlayFlag, bool bEnabled) override
        {
            return With(h, [&](OverlayState& s) { if (bEnabled) s.flags |= uint64_t(eOverlayFlag); else s.flags &= ~uint64_t(eOverlayFlag); });
        }
        vr::EVROverlayError SetOverlayAlpha(vr::VROverlayHandle_t h, float fAlpha) override { return With(h, [&](OverlayState& s) { s.alpha = fAlpha; }); }
        vr::EVROverlayError SetOverlayTexelAspect(vr::VROverlayHandle_t h, float f) override { return With(h, [&](OverlayState& s) { s.texelAspect = f; }); }
        vr::EVROverlayError SetOverlaySortOrder(vr::VROverlayHandle_t h, uint32_t u) override { return With(h, [&](OverlayState& s) { s.sortOrder = u; }); }
        vr::EVROverlayError SetOverlayWidthInMeters(vr::VROverlayHandle_t h, float f) override { return With(h, [&](OverlayState& s) { s.width = f; }); }
        vr::EVROverlayError GetOverlayWidthInMeters(vr::VROverlayHandle_t h, float* pf) override
        { return With(h, [&](OverlayState& s) { if (pf) *pf = s.width; }); }
        vr::EVROverlayError SetOverlayCurvature(vr::VROverlayHandle_t h, float f) override { return With(h, [&](OverlayState& s) { s.curvature = f; }); }
        vr::EVROverlayError GetOverlayCurvature(vr::VROverlayHandle_t h, float* pf) override
        { return With(h, [&](OverlayState& s) { if (pf) *pf = s.curvature; }); }
        vr::EVROverlayError SetOverlayTextureBounds(vr::VROverlayHandle_t h, const vr::VRTextureBounds_t* b) override
        { return With(h, [&](OverlayState& s) { if (b) s.bounds = *b; }); }
        vr::EVROverlayError GetOverlayTransformType(vr::VROverlayHandle_t h, vr::VROverlayTransformType* pe) override
        { return With(h, [&](OverlayState&) { if (pe) *pe = vr::VROverlayTransform_Absolute; }); }
        vr::EVROverlayError SetOverlayTransformAbsolute(vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin origin,
            const vr::HmdMatrix34_t* m) override
        { return With(h, [&](OverlayState& s) { s.origin = origin; if (m) s.transform = *m; }); }
        vr::EVROverlayError GetOverlayTransformAbsolute(vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin* origin,
            vr::HmdMatrix34_t* m) override
        { return With(h, [&](OverlayState& s) { if (origin) *origin = s.origin; if (m) *m = s.transform; }); }
        vr::EVROverlayError GetOverlayTransformTrackedDeviceRelative(vr::VROverlayHandle_t, vr::TrackedDeviceIndex_t* punTrackedDevice,
            vr::HmdMatrix34_t*) override
        {
            if (punTrackedDevice) *punTrackedDevice = vr::k_unTrackedDeviceIndexInvalid;
            return vr::VROverlayError_RequestFailed;
        }
        vr::EVROverlayError ShowOverlay(vr::VROverlayHandle_t h) override { return With(h, [&](OverlayState& s) { s.visible = true; }); }
        vr::EVROverlayError HideOverlay(vr::VROverlayHandle_t h) override { return With(h, [&](OverlayState& s) { s.visible = false; }); }
        bool IsOverlayVisible(vr::VROverlayHandle_t h) override
        {
            bool visible = false;
            With(h, [&](OverlayState& s) { visible = s.visible; });
            return visible;
        }
        bool PollNextOverlayEvent(vr::VROverlayHandle_t, vr::VREvent_t*, uint32_t) override { return false; }
        vr::EVROverlayError SetOverlayInputMethod(vr::VROverlayHandle_t h, vr::VROverlayInputMethod e) override
        { return With(h, [&](OverlayState& s) { s.inputMethod = e; }); }
        vr::EVROverlayError SetOverlayMouseScale(vr::VROverlayHandle_t h, const vr::HmdVector2_t* v) override
        { return With(h, [&](OverlayState& s) { if (v) s.mouseScale = *v; }); }
        bool ComputeOverlayIntersection(vr::VROverlayHandle_t, const vr::VROverlayIntersectionParams_t*,
            vr::VROverlayIntersectionResults_t*) override { return false; }
        vr::EVROverlayError SetOverlayTexture(vr::VROverlayHandle_t h, const vr::Texture_t*) override
        {
            NoteContent(h);
            return With(h, [&](OverlayState& s) { ++s.contentUpdates; });
        }
        vr::EVROverlayError SetOverlayRaw(vr::VROverlayHandle_t h, void*, uint32_t, uint32_t, uint32_t) override
        {
            NoteContent(h);
            return With(h, [&](OverlayState& s) { ++s.contentUpdates; });
        }
        vr::EVROverlayError ShowKeyboardForOverlay(vr::VROverlayHandle_t, vr::EGamepadTextInputMode, vr::EGamepadTextInputLineMode,
            uint32_t, const char*, uint32_t, const char*, uint64_t) override { return vr::VROverlayError_RequestFailed; }
        uint32_t GetKeyboardText(char* pchText, uint32_t cchText) override
        {
            if (pchText && cchText) pchText[0] = '\0';
            return 0;
        }
        void HideKeyboard() override {}

    private:
        struct OverlayState
        {
            std::string key, name;
            bool visible = false;
            float alpha = 1.0f, texelAspect = 1.0f, width = 1.0f, curvature = 0.0f;
            uint32_t sortOrder = 0;
            uint64_t flags = 0;
            vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
            vr::HmdMatrix34_t transform{};
            vr::VRTextureBounds_t bounds{ 0.0f, 0.0f, 1.0f, 1.0f };
            vr::VROverlayInputMethod inputMethod = vr::VROverlayInputMethod_None;
            vr::HmdVector2_t mouseScale{};
            uint64_t contentUpdates = 0;
        };

        template<class F>
        vr::EVROverlayError With(vr::VROverlayHandle_t handle, F&& f)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            auto it = m_Overlays.find(handle);
            if (it == m_Overlays.end())
                return vr::VROverlayError_InvalidHandle;
            f(it->second);
            return vr::VROverlayError_None;
        }

        void NoteContent(vr::VROverlayHandle_t handle)
        {
            std::string key;
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                auto it = m_Overlays.find(handle);
                if (it == m_Overlays.end() || !m_LoggedContent.insert(it->second.key).second)
                    return;
                key = it->second.key;
            }
            Game::logMsg("[VR][OpenXR] overlay '%s' has content but overlays are not composited yet (M4); it is not shown", key.c_str());
        }

        std::mutex m_Mutex;
        std::map<vr::VROverlayHandle_t, OverlayState> m_Overlays;
        std::set<std::string> m_LoggedContent;
        vr::VROverlayHandle_t m_NextHandle = 0;
    };

    // The controller tip component (vr_controller_tip.h): the aim pose in the
    // raw frame (SteamVR derives its OpenXR aim pose from the same component).
    class XrRenderModelsFacade final : public IVrRenderModels
    {
    public:
        explicit XrRenderModelsFacade(XrInputRuntime& input) : m_Input(input) {}
        bool GetComponentStateForDevicePath(const char* pchRenderModelName, const char* pchComponentName,
            vr::VRInputValueHandle_t devicePath, const vr::RenderModel_ControllerMode_State_t*,
            vr::RenderModel_ComponentState_t* pComponentState) override
        {
            if (!pchRenderModelName || !pchComponentName || !pComponentState ||
                std::strcmp(pchComponentName, vr::k_pch_Controller_Component_Tip) != 0)
                return false;
            const int hand = devicePath == XrInputRuntime::kSourceLeft ? 0 : devicePath == XrInputRuntime::kSourceRight ? 1 : -1;
            vr::HmdMatrix34_t tip{};
            if (hand < 0 || !m_Input.TipLocal(hand, tip))
                return false;
            *pComponentState = {};
            pComponentState->mTrackingToComponentLocal = tip;
            pComponentState->mTrackingToComponentRenderModel = tip;
            pComponentState->uProperties = vr::VRComponentProperty_IsVisible | vr::VRComponentProperty_IsStatic;
            return true;
        }
    private:
        XrInputRuntime& m_Input;
    };

    class XrApplicationsFacade final : public IVrApplications
    {
    public:
        vr::EVRApplicationError AddApplicationManifest(const char*, bool) override { return vr::VRApplicationError_None; }
    };

    class OpenXrRuntime
    {
    public:
        OpenXrRuntime()
            : m_Input(m_Xr), m_SystemFacade(*this), m_CompositorFacade(*this), m_RenderModelsFacade(m_Input)
        {
            m_Ring.SetAssertHook(&OpenXrRuntime::OnRingViolation, this);
        }

        // ---- Instance -------------------------------------------------------
        bool EnsureInstance(std::string& reason)
        {
            std::lock_guard<std::mutex> lock(m_InitMutex);
            if (!m_InitAttempted)
            {
                m_InitAttempted = true;
                m_InitOk = CreateInstance(m_InitFailure);
                if (m_InitOk)
                    StartEventThread();
            }
            reason = m_InitFailure;
            return m_InitOk;
        }

        bool InstanceReady() const { return m_InitOk; }
        const char* InitFailureText() const { return m_InitFailure.c_str(); }

        void RecommendedSize(uint32_t* width, uint32_t* height) const
        {
            uint32_t w = 0, h = 0;
            for (const auto& view : m_ViewConfig)
            {
                w = (std::max)(w, view.recommendedImageRectWidth);
                h = (std::max)(h, view.recommendedImageRectHeight);
            }
            if (width) *width = w;
            if (height) *height = h;
        }

        bool VulkanExtensions(bool device, std::string& out)
        {
            std::string reason;
            if (!EnsureInstance(reason))
                return false;
            auto query = device ? m_Xr.xrGetVulkanDeviceExtensionsKHR : m_Xr.xrGetVulkanInstanceExtensionsKHR;
            uint32_t n = 0;
            XrResult r = query(m_Instance, m_SystemId, 0, &n, nullptr);
            if (XR_FAILED(r))
            {
                Game::logMsg("[VR][OpenXR] xrGetVulkan%sExtensionsKHR failed: %s", device ? "Device" : "Instance", ResultName(r).c_str());
                return false;
            }
            std::vector<char> buffer(n + 1, '\0');
            r = query(m_Instance, m_SystemId, n, &n, buffer.data());
            if (XR_FAILED(r))
                return false;
            out.assign(buffer.data(), strnlen(buffer.data(), buffer.size()));
            Game::logMsg("[VR][OpenXR] Vulkan %s extensions: %s", device ? "device" : "instance", out.c_str());
            return true;
        }

        bool GraphicsDevice(void* vkInstance, void** physicalDevice)
        {
            std::string reason;
            if (!EnsureInstance(reason) || !vkInstance || !physicalDevice)
                return false;
            VkPhysicalDevice device = VK_NULL_HANDLE;
            const XrResult r = m_Xr.xrGetVulkanGraphicsDeviceKHR(m_Instance, m_SystemId, static_cast<VkInstance>(vkInstance), &device);
            if (XR_FAILED(r) || device == VK_NULL_HANDLE)
            {
                Game::logMsg("[VR][OpenXR] xrGetVulkanGraphicsDeviceKHR failed: %s", ResultName(r).c_str());
                return false;
            }
            m_RuntimePhysicalDevice = device;
            *physicalDevice = device;
            return true;
        }

        // ---- Session --------------------------------------------------------
        void DeviceCreated(const L4D2VR_VkDeviceInfo& info)
        {
            if (!m_InitOk)
                return;
            if (m_Session != XR_NULL_HANDLE)
            {
                if (info.device != m_Device.device)
                    Game::logMsg("[VR][OpenXR] a second D3D9 device was created; the OpenXR session stays on the first one");
                return;
            }
            m_Device = info;
            if (!LoadVulkan())
                return;

            const VkPhysicalDevice physical = static_cast<VkPhysicalDevice>(info.physicalDevice);
            VkPhysicalDeviceProperties props{};
            m_Vk.vkGetPhysicalDeviceProperties(physical, &props);
            Game::logMsg("[VR][OpenXR] DXVK device on '%s' (queue family %u index %u); runtime GPU %s",
                props.deviceName, info.queueFamilyIndex, info.queueIndex,
                m_RuntimePhysicalDevice == VK_NULL_HANDLE ? "unknown"
                    : (m_RuntimePhysicalDevice == physical ? "matches" : "DIFFERS (session will likely fail)"));

            XrGraphicsRequirementsVulkanKHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
            XrResult r = m_Xr.xrGetVulkanGraphicsRequirementsKHR(m_Instance, m_SystemId, &requirements);
            if (XR_FAILED(r))
            {
                Game::logMsg("[VR][OpenXR] xrGetVulkanGraphicsRequirementsKHR failed: %s", ResultName(r).c_str());
                return;
            }
            const XrVersion dxvkApi = XR_MAKE_VERSION(1, 3, 0);
            Game::logMsg("[VR][OpenXR] Vulkan requirements %u.%u..%u.%u, DXVK instance 1.3%s",
                XR_VERSION_MAJOR(requirements.minApiVersionSupported), XR_VERSION_MINOR(requirements.minApiVersionSupported),
                XR_VERSION_MAJOR(requirements.maxApiVersionSupported), XR_VERSION_MINOR(requirements.maxApiVersionSupported),
                (dxvkApi < requirements.minApiVersionSupported) ? " (BELOW the runtime minimum)" : "");

            QueueLock queue(*this);
            std::lock_guard<std::mutex> lock(m_Mutex);
            XrGraphicsBindingVulkanKHR binding{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
            binding.instance = static_cast<VkInstance>(info.instance);
            binding.physicalDevice = physical;
            binding.device = static_cast<VkDevice>(info.device);
            binding.queueFamilyIndex = info.queueFamilyIndex;
            binding.queueIndex = info.queueIndex;
            XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
            sci.next = &binding;  // the graphics binding is the first (and only) struct in the chain
            sci.systemId = m_SystemId;
            r = m_Xr.xrCreateSession(m_Instance, &sci, &m_Session);
            if (XR_FAILED(r))
            {
                m_Session = XR_NULL_HANDLE;
                Game::logMsg("[VR][OpenXR] xrCreateSession failed: %s", ResultName(r).c_str());
                return;
            }
            CreateSpacesLocked();
            m_Input.SessionCreated(m_Session);
            uint32_t n = 0;
            m_Xr.xrEnumerateSwapchainFormats(m_Session, 0, &n, nullptr);
            m_SwapchainFormats.assign(n, 0);
            m_Xr.xrEnumerateSwapchainFormats(m_Session, n, &n, m_SwapchainFormats.data());
            std::string formats;
            for (int64_t f : m_SwapchainFormats)
                formats += std::to_string(static_cast<long long>(f)) + " ";
            Game::logMsg("[VR][OpenXR] session created; swapchain formats: %s", formats.c_str());
            m_SessionCv.notify_all();
        }

        // ---- Frames: wait ---------------------------------------------------
        vr::EVRCompositorError WaitGetPoses(vr::TrackedDevicePose_t* renderPoses, uint32_t renderCount,
            vr::TrackedDevicePose_t* gamePoses, uint32_t gameCount)
        {
            FillInvalid(renderPoses, renderCount);
            FillInvalid(gamePoses, gameCount);
            if (!Running())
            {
                // Not running yet (or stopped): pace the caller instead of spinning.
                Sleep(11);
                return vr::VRCompositorError_DoNotHaveFocus;
            }

            bool needStale = false;
            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                if (m_Ring.WaitInFlight())
                    return JoinInFlightWait(lock, renderPoses, renderCount, gamePoses, gameCount);
                needStale = m_Ring.HasWaited();
                if (!needStale)
                    m_Ring.BeginWait();
            }
            if (needStale)
            {
                QueueLock queue(*this);
                std::unique_lock<std::mutex> lock(m_Mutex);
                if (m_Ring.WaitInFlight())
                {
                    queue.Release();
                    return JoinInFlightWait(lock, renderPoses, renderCount, gamePoses, gameCount);
                }
                if (m_Ring.HasWaited())
                    EndStaleLocked();
                if (!m_Ring.BeginWait())
                    return vr::VRCompositorError_RequestFailed;
            }

            XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
            XrFrameState frameState{ XR_TYPE_FRAME_STATE };
            const auto waitStart = std::chrono::steady_clock::now();
            const XrResult r = m_Xr.xrWaitFrame(m_Session, &waitInfo, &frameState);
            const double waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                const bool ok = XR_SUCCEEDED(r);
                m_Ring.CompleteWait(ok, frameState.predictedDisplayTime, frameState.predictedDisplayPeriod,
                    frameState.shouldRender == XR_TRUE);
                if (ok)
                {
                    m_LastDisplayTime = frameState.predictedDisplayTime;
                    m_LastDisplayPeriod = frameState.predictedDisplayPeriod;
                    m_WaitMsSum += waitMs;
                    m_WaitMsMax = (std::max)(m_WaitMsMax, waitMs);
                }
                else
                {
                    m_LastWaitError = r;
                }
            }
            m_WaitCv.notify_all();
            if (XR_FAILED(r))
            {
                Sleep(5);
                return vr::VRCompositorError_RequestFailed;
            }

            LocateHmdAndViews(frameState.predictedDisplayTime);
            CopyLatestPoses(renderPoses, renderCount, gamePoses, gameCount);
            return vr::VRCompositorError_None;
        }

        // ---- Frames: submit side (caller holds DXVK's queue lock) -----------
        vr::EVRCompositorError SubmitExplicitTimingData()
        {
            if (!Running())
                return vr::VRCompositorError_None;
            std::lock_guard<std::mutex> lock(m_Mutex);
            BeginForGameLocked();
            return vr::VRCompositorError_None;
        }

        vr::EVRCompositorError Submit(vr::EVREye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds,
            vr::EVRSubmitFlags flags)
        {
            if (!texture || !texture->handle || texture->eType != vr::TextureType_Vulkan)
                return vr::VRCompositorError_InvalidTexture;
            const auto* data = static_cast<const vr::VRVulkanTextureData_t*>(texture->handle);
            if (!data->m_nImage || data->m_nWidth == 0 || data->m_nHeight == 0)
                return vr::VRCompositorError_InvalidTexture;
            if (data->m_nSampleCount > 1)
                return vr::VRCompositorError_InvalidTexture;
            if (!Running())
            {
                ++m_DroppedSubmits;
                return vr::VRCompositorError_None;
            }
            const int e = eye == vr::Eye_Right ? 1 : 0;

            std::lock_guard<std::mutex> lock(m_Mutex);
            BeginForGameLocked();
            if (!EnsureSwapchainLocked(e, data->m_nWidth, data->m_nHeight, static_cast<VkFormat>(data->m_nFormat)))
                return vr::VRCompositorError_RequestFailed;
            // 32-bit: VkImage is a 64-bit non-dispatchable handle, like m_nImage.
            if (!CopyIntoSwapchainLocked(e, (VkImage)data->m_nImage))
                return vr::VRCompositorError_RequestFailed;

            // The pose the eye was rendered from: the render pose the mod passes
            // with the texture, else the HMD pose WaitGetPoses last returned.
            vr::HmdMatrix34_t head = m_LastHmdPose.mDeviceToAbsoluteTracking;
            if ((flags & vr::Submit_TextureWithPose) != 0)
                head = static_cast<const vr::VRTextureWithPose_t*>(texture)->mDeviceToAbsoluteTracking;
            LayerView& view = m_Layer[e];
            bool flipped = false;
            view.rect = xrm::ImageRectFromBounds(bounds, m_Swapchain[e].width, m_Swapchain[e].height, &flipped);
            if (flipped && !m_LoggedFlippedBounds)
            {
                m_LoggedFlippedBounds = true;
                Game::logMsg("[VR][OpenXR] flipped texture bounds are not supported by OpenXR image rects; submitted unflipped");
            }
            view.pose = xrm::EyePoseFromHead(head, m_EyeToHead[e], true);
            view.fov = m_RenderFovValid[e] ? m_RenderFov[e] : m_CurrentFov[e];
            view.swapchain = m_Swapchain[e].handle;
            view.valid = true;
            ++m_SubmittedEyes;

            if (!m_ExplicitTiming && e == 1)
                EndForGameLocked();
            return vr::VRCompositorError_None;
        }

        void PostPresentHandoff()
        {
            if (!Running())
                return;
            std::lock_guard<std::mutex> lock(m_Mutex);
            EndForGameLocked();
        }

        void ClearLastSubmittedFrame()
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Layer[0].valid = false;
            m_Layer[1].valid = false;
        }

        void SetExplicitTimingMode(vr::EVRCompositorTimingMode mode)
        {
            m_ExplicitTiming = mode == vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff;
        }

        bool GetFrameTiming(vr::Compositor_FrameTiming* timing)
        {
            if (!timing || timing->m_nSize < offsetof(vr::Compositor_FrameTiming, m_HmdPose) + sizeof(vr::TrackedDevicePose_t))
                return false;
            std::lock_guard<std::mutex> lock(m_Mutex);
            const uint64_t id = m_Ring.LastTicketId();
            if (id == 0)
                return false;
            timing->m_nFrameIndex = static_cast<uint32_t>(id);
            timing->m_nNumFramePresents = 1;
            timing->m_nNumMisPresented = 0;
            timing->m_nNumDroppedFrames = 0;
            timing->m_nReprojectionFlags = 0;
            LARGE_INTEGER qpc{}, freq{};
            QueryPerformanceCounter(&qpc);
            QueryPerformanceFrequency(&freq);
            timing->m_flSystemTimeInSeconds = freq.QuadPart ? static_cast<double>(qpc.QuadPart) / static_cast<double>(freq.QuadPart) : 0.0;
            timing->m_HmdPose = m_LastHmdPose;
            return true;
        }

        float FrameTimeRemaining()
        {
            int64_t displayTime = 0;
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                displayTime = m_LastDisplayTime;
            }
            XrTime now = 0;
            if (!displayTime || !NowXrTime(now))
                return 0.011f;
            const double seconds = static_cast<double>(displayTime - now) / 1e9;
            return static_cast<float>(std::clamp(seconds, 0.0, 0.1));
        }

        // ---- System ---------------------------------------------------------
        void ProjectionRaw(vr::EVREye eye, float* left, float* right, float* top, float* bottom)
        {
            const int e = eye == vr::Eye_Right ? 1 : 0;
            WaitForViews();
            std::lock_guard<std::mutex> lock(m_Mutex);
            xrm::Fov fov = m_CurrentFov[e];
            if (!xrm::FovUsable(fov))
            {
                // No view yet (no session): a symmetric 100 degree view.
                fov = xrm::Fov{ -0.8727f, 0.8727f, 0.8727f, -0.8727f };
                Game::logMsg("[VR][OpenXR] no view FOV from the runtime yet; using a default for eye %d", e);
            }
            // The game renders with this FOV from now on; projection layers
            // declare the same one.
            if (!m_RenderFovValid[e])
            {
                m_RenderFov[e] = fov;
                m_RenderFovValid[e] = true;
            }
            const xrm::RawProjection raw = xrm::ToRawProjection(m_RenderFov[e]);
            if (left) *left = raw.left;
            if (right) *right = raw.right;
            if (top) *top = raw.top;
            if (bottom) *bottom = raw.bottom;
        }

        vr::HmdMatrix34_t EyeToHead(vr::EVREye eye)
        {
            WaitForViews();
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_EyeToHead[eye == vr::Eye_Right ? 1 : 0];
        }

        void PredictPoses(float secondsFromNow, vr::TrackedDevicePose_t* poses, uint32_t count)
        {
            FillInvalid(poses, count);
            if (!poses || count == 0 || m_Session == XR_NULL_HANDLE)
                return;
            XrTime time = 0;
            if (NowXrTime(time))
                time += static_cast<XrTime>(std::clamp(secondsFromNow, 0.0f, 0.5f) * 1e9f);
            else
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                time = m_LastDisplayTime;
            }
            if (time <= 0)
                return;
            vr::TrackedDevicePose_t hmd{};
            if (LocateHmd(time, hmd))
                poses[kHmd] = hmd;
            if (count > kRightHand)
            {
                m_Input.LocateHand(0, m_BaseSpace, time, poses[kLeftHand]);
                m_Input.LocateHand(1, m_BaseSpace, time, poses[kRightHand]);
            }
        }

        bool HmdConnected() const { return m_Session != XR_NULL_HANDLE; }

        float DisplayHz()
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_RefreshRateHz > 1.0f)
                return m_RefreshRateHz;
            return xrm::DisplayHzFromPeriod(m_LastDisplayPeriod, 90.0f);
        }

        vr::ETrackingUniverseOrigin TrackingOrigin() const
        {
            return m_BaseSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ? vr::TrackingUniverseSeated : vr::TrackingUniverseStanding;
        }

        uint32_t MaxLayers() const { return m_MaxLayers; }

        bool RequestEyeDump(const char* path)
        {
            if (!path || !*path || m_Session == XR_NULL_HANDLE)
                return false;
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_DumpPath = path;
            return true;
        }

        IVrSystem* SystemFacade() { return &m_SystemFacade; }
        IVrCompositor* CompositorFacade() { return &m_CompositorFacade; }
        IVrInput* InputFacade() { return &m_Input; }
        XrInputRuntime& Input() { return m_Input; }
        IVrOverlay* OverlayFacade() { return &m_OverlayFacade; }
        IVrRenderModels* RenderModelsFacade() { return &m_RenderModelsFacade; }
        IVrApplications* ApplicationsFacade() { return &m_ApplicationsFacade; }

    private:
        struct Swapchain
        {
            XrSwapchain handle = XR_NULL_HANDLE;
            uint32_t width = 0, height = 0;
            VkFormat sourceFormat = VK_FORMAT_UNDEFINED;
            int64_t format = 0;
            std::vector<VkImage> images;
            int64_t acquired = -1;  // image acquired but not yet released (a timed-out wait)
            bool released = false;  // at least one image released (usable in a layer)
        };

        struct LayerView
        {
            bool valid = false;
            XrSwapchain swapchain = XR_NULL_HANDLE;  // released at least once
            xrm::Rect rect;
            xrm::Pose pose;
            xrm::Fov fov;
        };

        class QueueLock
        {
        public:
            explicit QueueLock(OpenXrRuntime& rt) : m_Rt(rt)
            {
                if (m_Rt.m_Device.lockQueue)
                {
                    m_Rt.m_Device.lockQueue(m_Rt.m_Device.queueContext);
                    m_Locked = true;
                }
            }
            ~QueueLock() { Release(); }
            void Release()
            {
                if (m_Locked)
                {
                    m_Rt.m_Device.unlockQueue(m_Rt.m_Device.queueContext);
                    m_Locked = false;
                }
            }
        private:
            OpenXrRuntime& m_Rt;
            bool m_Locked = false;
        };

        static void OnRingViolation(xrm::RingViolation violation, void* context)
        {
            auto* self = static_cast<OpenXrRuntime*>(context);
            const uint64_t count = self->m_Ring.Stats().violations;
            if (count <= 20 || (count % 1000) == 0)
                Game::logMsg("[VR][OpenXR][ASSERT] frame ring violation: %s (count %llu)", xrm::Name(violation),
                    static_cast<unsigned long long>(count));
        }

        std::string ResultName(XrResult r) const
        {
            char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
            if (m_Instance != XR_NULL_HANDLE && m_Xr.xrResultToString &&
                XR_SUCCEEDED(m_Xr.xrResultToString(m_Instance, r, buffer)))
                return std::string(buffer) + " (" + std::to_string(static_cast<int>(r)) + ")";
            return std::to_string(static_cast<int>(r));
        }

        // Proton's steam.exe sets HKCU\Software\Wine\VR "state" to 1 once its
        // OpenVR runtime (XRizer, OpenComposite, SteamVR) and the OpenXR info
        // are ready; wineopenxr refuses to work before that.
        void WaitForWineVrState()
        {
            const auto start = std::chrono::steady_clock::now();
            DWORD last = 0xFFFFFFFEu;
            bool keySeen = false;
            for (;;)
            {
                const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                HKEY key = nullptr;
                if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\VR", 0, KEY_READ, &key) == ERROR_SUCCESS)
                {
                    keySeen = true;
                    DWORD value = 0, size = sizeof(value), type = 0;
                    const bool read = RegQueryValueExA(key, "state", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
                        type == REG_DWORD;
                    RegCloseKey(key);
                    if (read && value != last)
                    {
                        Game::logMsg("[VR][OpenXR] Wine VR state = %d after %.1f s", static_cast<int>(value), elapsed);
                        last = value;
                    }
                    if (read && value != 0)
                        return;
                }
                // steam.exe creates the key before the game starts when the
                // launch is a Steam game (SteamGameId); without it the key
                // never appears.
                if ((!keySeen && elapsed > 5.0) || elapsed > 45.0)
                {
                    Game::logMsg("[VR][OpenXR] Wine VR state not ready after %.1f s (key %s); continuing", elapsed,
                        keySeen ? "present" : "missing");
                    return;
                }
                Sleep(50);
            }
        }

        // Developer hook: L4D2VR_XR_VALIDATION=1 enables the Khronos core
        // validation layer through the loader's environment (never through the
        // create info, which wineopenxr would forward to the host loader). Under
        // Wine the Win32 loader ignores XR_API_LAYER_PATH, so the layer manifest
        // named by L4D2VR_XR_VALIDATION_LAYER_JSON is registered in the prefix.
        void ConfigureValidationLayer(bool wine, const std::string& gameDir)
        {
            if (EnvValue("L4D2VR_XR_VALIDATION") != "1")
                return;
            const std::string json = EnvValue("L4D2VR_XR_VALIDATION_LAYER_JSON");
            if (wine && !json.empty())
            {
                HKEY key = nullptr;
                LONG r = RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\Khronos\\OpenXR\\1\\ApiLayers\\Explicit", 0, nullptr, 0,
                    KEY_WRITE, nullptr, &key, nullptr);
                if (r == ERROR_SUCCESS)
                {
                    DWORD zero = 0;
                    r = RegSetValueExA(key, json.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
                    RegCloseKey(key);
                }
                Game::logMsg("[VR][OpenXR] validation: registered %s in the prefix -> %ld", json.c_str(), r);
            }
            else if (!json.empty())
            {
                const size_t slash = json.find_last_of("\\/");
                SetEnvironmentVariableA("XR_API_LAYER_PATH", slash == std::string::npos ? "." : json.substr(0, slash).c_str());
            }
            const std::string output = gameDir + "\\l4d2vr_xr_validation.txt";
            SetEnvironmentVariableA("XR_ENABLE_API_LAYERS", "XR_APILAYER_LUNARG_core_validation");
            SetEnvironmentVariableA("XR_CORE_VALIDATION_EXPORT_TYPE", "text");
            SetEnvironmentVariableA("XR_CORE_VALIDATION_FILE_NAME", output.c_str());
            Game::logMsg("[VR][OpenXR] validation: XR_APILAYER_LUNARG_core_validation enabled, output %s", output.c_str());
        }

        bool CreateInstance(std::string& reason)
        {
            const bool wine = IsWine();
            const std::string dir = ModuleDirectory();
            ConfigureValidationLayer(wine, dir);
            if (wine)
                WaitForWineVrState();

            xrm::LoaderProbe probe;
            if (!xrm::ProbeLoader(dir + "\\openxr_loader.dll", wine, probe))
            {
                reason = probe.reason;
                return false;
            }
            m_Loader = probe.module;
            m_Xr.xrGetInstanceProcAddr = probe.getInstanceProcAddr;
#define L4D2VR_XR_LOAD_GLOBAL(name) \
            m_Xr.xrGetInstanceProcAddr(XR_NULL_HANDLE, #name, reinterpret_cast<PFN_xrVoidFunction*>(&m_Xr.name)); \
            if (!m_Xr.name) { reason = "loader lacks " #name; return false; }
            L4D2VR_XR_GLOBAL_FUNCS(L4D2VR_XR_LOAD_GLOBAL)
#undef L4D2VR_XR_LOAD_GLOBAL
            const std::set<std::string>& available = probe.extensions;
            XrResult r = XR_SUCCESS;
            std::vector<const char*> enable{ XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
            const char* optional[] = {
                XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME,
                XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
                XR_EXT_LOCAL_FLOOR_EXTENSION_NAME,
            };
            for (const char* name : optional)
            {
                if (available.count(name))
                {
                    enable.push_back(name);
                    m_Extensions.insert(name);
                }
            }
            // Controller profiles that need an extension (Cosmos, Quest Pro/3, Pico, ...).
            for (const char* name : XrInputRuntime::WantedExtensions())
            {
                if (available.count(name))
                {
                    enable.push_back(name);
                    m_Extensions.insert(name);
                }
            }

            XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
            strncpy_s(ci.applicationInfo.applicationName, "Left 4 Dead 2 VR", _TRUNCATE);
            ci.applicationInfo.applicationVersion = 1;
            strncpy_s(ci.applicationInfo.engineName, "L4D2VR (Source + DXVK)", _TRUNCATE);
            ci.applicationInfo.engineVersion = 1;
            ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
            ci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
            ci.enabledExtensionNames = enable.data();
            const auto createStart = std::chrono::steady_clock::now();
            r = m_Xr.xrCreateInstance(&ci, &m_Instance);
            if (XR_FAILED(r))
            {
                m_Instance = XR_NULL_HANDLE;
                reason = "xrCreateInstance failed (" + std::to_string(static_cast<int>(r)) + ")";
                return false;
            }
#define L4D2VR_XR_LOAD_INSTANCE(name) \
            m_Xr.xrGetInstanceProcAddr(m_Instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&m_Xr.name)); \
            if (!m_Xr.name) { reason = "runtime lacks " #name; DestroyInstance(); return false; }
            L4D2VR_XR_INSTANCE_FUNCS(L4D2VR_XR_LOAD_INSTANCE)
#undef L4D2VR_XR_LOAD_INSTANCE
            if (m_Extensions.count(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME))
                m_Xr.xrGetInstanceProcAddr(m_Instance, "xrConvertWin32PerformanceCounterToTimeKHR",
                    reinterpret_cast<PFN_xrVoidFunction*>(&m_Xr.xrConvertWin32PerformanceCounterToTimeKHR));
            if (m_Extensions.count(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
                m_Xr.xrGetInstanceProcAddr(m_Instance, "xrGetDisplayRefreshRateFB",
                    reinterpret_cast<PFN_xrVoidFunction*>(&m_Xr.xrGetDisplayRefreshRateFB));

            XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
            m_Xr.xrGetInstanceProperties(m_Instance, &ip);
            m_RuntimeName = ip.runtimeName;

            XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
            sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            r = m_Xr.xrGetSystem(m_Instance, &sgi, &m_SystemId);
            if (XR_FAILED(r))
            {
                reason = r == XR_ERROR_FORM_FACTOR_UNAVAILABLE ? "no headset connected (XR_ERROR_FORM_FACTOR_UNAVAILABLE)"
                    : "xrGetSystem failed: " + ResultName(r);
                DestroyInstance();
                return false;
            }
            XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
            m_Xr.xrGetSystemProperties(m_Instance, m_SystemId, &sp);
            m_MaxLayers = sp.graphicsProperties.maxLayerCount;

            uint32_t views = 0;
            r = m_Xr.xrEnumerateViewConfigurationViews(m_Instance, m_SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &views, nullptr);
            if (XR_FAILED(r) || views != 2)
            {
                reason = "the headset has no stereo view configuration";
                DestroyInstance();
                return false;
            }
            m_ViewConfig.assign(2, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
            m_Xr.xrEnumerateViewConfigurationViews(m_Instance, m_SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &views, m_ViewConfig.data());

            // Input: action sets, actions and suggested bindings (before any session).
            m_Input.CreateActions(m_Instance, m_Extensions, m_RuntimeName);

            std::string enabledList;
            for (const char* name : enable)
                enabledList += std::string(name) + " ";
            Game::logMsg("[VR][OpenXR] instance ready in %.2f s: runtime '%s' %u.%u.%u, system '%s', %u layers max, eyes %ux%u (max %ux%u), wine=%d, extensions: %s",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - createStart).count(),
                ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion),
                sp.systemName, m_MaxLayers, m_ViewConfig[0].recommendedImageRectWidth, m_ViewConfig[0].recommendedImageRectHeight,
                m_ViewConfig[0].maxImageRectWidth, m_ViewConfig[0].maxImageRectHeight, wine ? 1 : 0, enabledList.c_str());
            return true;
        }

        void DestroyInstance()
        {
            // Handles are nulled at destruction: wineopenxr crashes on a
            // destroyed handle instead of returning XR_ERROR_HANDLE_INVALID.
            if (m_Instance != XR_NULL_HANDLE && m_Xr.xrDestroyInstance)
                m_Xr.xrDestroyInstance(m_Instance);
            m_Instance = XR_NULL_HANDLE;
            m_SystemId = XR_NULL_SYSTEM_ID;
        }

        bool LoadVulkan()
        {
            auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(m_Device.getInstanceProcAddr);
            const VkInstance instance = static_cast<VkInstance>(m_Device.instance);
            const VkDevice device = static_cast<VkDevice>(m_Device.device);
            if (!gipa || !instance || !device)
            {
                Game::logMsg("[VR][OpenXR] DXVK passed no Vulkan device; no session");
                return false;
            }
#define L4D2VR_VK_LOAD_INSTANCE(name) \
            m_Vk.name = reinterpret_cast<PFN_##name>(gipa(instance, #name)); \
            if (!m_Vk.name) { Game::logMsg("[VR][OpenXR] Vulkan lacks " #name); return false; }
            L4D2VR_VK_INSTANCE_FUNCS(L4D2VR_VK_LOAD_INSTANCE)
#undef L4D2VR_VK_LOAD_INSTANCE
#define L4D2VR_VK_LOAD_DEVICE(name) \
            m_Vk.name = reinterpret_cast<PFN_##name>(m_Vk.vkGetDeviceProcAddr(device, #name)); \
            if (!m_Vk.name) { Game::logMsg("[VR][OpenXR] Vulkan lacks " #name); return false; }
            L4D2VR_VK_DEVICE_FUNCS(L4D2VR_VK_LOAD_DEVICE)
#undef L4D2VR_VK_LOAD_DEVICE
            m_Vk.vkGetPhysicalDeviceMemoryProperties(static_cast<VkPhysicalDevice>(m_Device.physicalDevice), &m_MemoryProperties);

            VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = m_Device.queueFamilyIndex;
            if (m_Vk.vkCreateCommandPool(device, &pci, nullptr, &m_CommandPool) != VK_SUCCESS)
                return false;
            VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cai.commandPool = m_CommandPool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            if (m_Vk.vkAllocateCommandBuffers(device, &cai, &m_CommandBuffer) != VK_SUCCESS)
                return false;
            VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            return m_Vk.vkCreateFence(device, &fci, nullptr, &m_Fence) == VK_SUCCESS;
        }

        void CreateSpacesLocked()
        {
            uint32_t n = 0;
            m_Xr.xrEnumerateReferenceSpaces(m_Session, 0, &n, nullptr);
            std::vector<XrReferenceSpaceType> types(n);
            m_Xr.xrEnumerateReferenceSpaces(m_Session, n, &n, types.data());
            auto has = [&](XrReferenceSpaceType t) { return std::find(types.begin(), types.end(), t) != types.end(); };

            // Standing play: STAGE (floor at 0, the OpenVR "standing" universe),
            // then LOCAL_FLOOR, then LOCAL (seated; the mod recentres app-side).
            XrReferenceSpaceCreateInfo rci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
            rci.poseInReferenceSpace.orientation.w = 1.0f;
            const XrReferenceSpaceType order[] = {
                XR_REFERENCE_SPACE_TYPE_STAGE, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR, XR_REFERENCE_SPACE_TYPE_LOCAL };
            for (XrReferenceSpaceType type : order)
            {
                if (!has(type) || (type == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR && !m_Extensions.count(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME)))
                    continue;
                rci.referenceSpaceType = type;
                if (XR_SUCCEEDED(m_Xr.xrCreateReferenceSpace(m_Session, &rci, &m_BaseSpace)))
                {
                    m_BaseSpaceType = type;
                    break;
                }
                m_BaseSpace = XR_NULL_HANDLE;
            }
            rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            m_Xr.xrCreateReferenceSpace(m_Session, &rci, &m_ViewSpace);
            Game::logMsg("[VR][OpenXR] tracking space %s",
                m_BaseSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE (standing)"
                : m_BaseSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR ? "LOCAL_FLOOR (standing)" : "LOCAL (seated)");
        }

        bool Running() const { return m_Running.load(std::memory_order_acquire); }

        // ---- Events and session state ---------------------------------------
        void StartEventThread()
        {
            std::thread([this]() { EventThreadMain(); }).detach();
        }

        void EventThreadMain()
        {
            auto lastStats = std::chrono::steady_clock::now();
            while (!m_Lost.load(std::memory_order_acquire))
            {
                PollEvents();
                const auto now = std::chrono::steady_clock::now();
                if (now - lastStats >= std::chrono::seconds(5))
                {
                    lastStats = now;
                    LogStats();
                }
                Sleep(5);
            }
            Game::logMsg("[VR][OpenXR] event thread stopped (session or instance lost)");
        }

        void PollEvents()
        {
            for (;;)
            {
                XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
                const XrResult r = m_Xr.xrPollEvent(m_Instance, &event);
                if (r == XR_EVENT_UNAVAILABLE)
                    return;
                if (XR_FAILED(r))
                {
                    Game::logMsg("[VR][OpenXR] xrPollEvent failed: %s", ResultName(r).c_str());
                    return;
                }
                switch (event.type)
                {
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                    OnSessionState(reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state);
                    break;
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    Game::logMsg("[VR][OpenXR] instance loss pending: VR stops (restart the game to recover)");
                    m_Running.store(false, std::memory_order_release);
                    m_Lost.store(true, std::memory_order_release);
                    break;
                case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                    Game::logMsg("[VR][OpenXR] reference space change pending (recentre)");
                    break;
                case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                    Game::logMsg("[VR][OpenXR] interaction profile changed");
                    m_Input.NotifyProfileChanged();
                    break;
                case XR_TYPE_EVENT_DATA_EVENTS_LOST:
                    Game::logMsg("[VR][OpenXR] %u events lost", reinterpret_cast<const XrEventDataEventsLost&>(event).lostEventCount);
                    break;
                default:
                    break;
                }
            }
        }

        void OnSessionState(XrSessionState state)
        {
            Game::logMsg("[VR][OpenXR] session state %s -> %s", SessionStateName(m_State), SessionStateName(state));
            m_State = state;
            if (state == XR_SESSION_STATE_FOCUSED && !m_LoggedFocused)
            {
                m_LoggedFocused = true;
                Game::logMsg("[VR][OpenXR] session FOCUSED");
            }
            switch (state)
            {
            case XR_SESSION_STATE_READY:
            {
                QueueLock queue(*this);
                std::lock_guard<std::mutex> lock(m_Mutex);
                XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult r = m_Xr.xrBeginSession(m_Session, &bi);
                Game::logMsg("[VR][OpenXR] xrBeginSession: %s", ResultName(r).c_str());
                if (XR_SUCCEEDED(r))
                {
                    m_Ring.Reset();
                    m_Running.store(true, std::memory_order_release);
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
            {
                m_Running.store(false, std::memory_order_release);
                QueueLock queue(*this);
                std::unique_lock<std::mutex> lock(m_Mutex);
                // Let a wait in flight return before ending the session.
                m_WaitCv.wait_for(lock, std::chrono::milliseconds(200), [this]() { return !m_Ring.WaitInFlight(); });
                const XrResult r = m_Xr.xrEndSession(m_Session);
                m_Ring.Reset();
                Game::logMsg("[VR][OpenXR] xrEndSession: %s", ResultName(r).c_str());
                break;
            }
            case XR_SESSION_STATE_LOSS_PENDING:
            case XR_SESSION_STATE_EXITING:
                m_Running.store(false, std::memory_order_release);
                m_Lost.store(true, std::memory_order_release);
                Game::logMsg("[VR][OpenXR] session %s: VR stops (restart the game to recover)", SessionStateName(state));
                break;
            default:
                break;
            }
        }

        void LogStats()
        {
            if (m_Session == XR_NULL_HANDLE)
                return;
            xrm::FrameRingStats stats;
            double waitAvg = 0.0, waitMax = 0.0;
            uint64_t ends = 0, projectionEnds = 0, emptyEnds = 0, endErrors = 0, eyes = 0, dropped = 0;
            XrResult lastEndError = XR_SUCCESS;
            float hz = 0.0f;
            vr::TrackedDevicePose_t hmd{};
            vr::TrackedDevicePose_t hands[2]{};
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                stats = m_Ring.Stats();
                const uint64_t waits = stats.waits - m_StatsPrevWaits;
                waitAvg = waits ? m_WaitMsSum / static_cast<double>(waits) : 0.0;
                waitMax = m_WaitMsMax;
                m_StatsPrevWaits = stats.waits;
                m_WaitMsSum = 0.0;
                m_WaitMsMax = 0.0;
                ends = m_EndCalls; projectionEnds = m_ProjectionEnds; emptyEnds = m_EmptyEnds; endErrors = m_EndErrors;
                lastEndError = m_LastEndError;
                eyes = m_SubmittedEyes;
                dropped = m_DroppedSubmits;
                hz = xrm::DisplayHzFromPeriod(m_LastDisplayPeriod, 0.0f);
                hmd = m_LastHmdPose;
                hands[0] = m_LastHandPose[0];
                hands[1] = m_LastHandPose[1];
            }
            // HMD pose (tracking space): position and yaw/pitch of the view direction.
            const vr::HmdMatrix34_t& m = hmd.mDeviceToAbsoluteTracking;
            const float fx = -m.m[0][2], fy = -m.m[1][2], fz = -m.m[2][2];
            const float yawDeg = std::atan2(-fx, -fz) * 57.29578f;
            const float pitchDeg = std::asin(std::clamp(fy, -1.0f, 1.0f)) * 57.29578f;
            Game::logMsg("[VR][OpenXR][Frames] state=%s hz=%.1f waits=%llu waitFail=%llu begins=%llu ends=%llu (xrEndFrame %llu: projection %llu, empty %llu, errors %llu last=%d) stale=%llu discarded=%llu violations=%llu eyes=%llu droppedEyes=%llu wait avg=%.2f max=%.2f ms hmd=%d pos=(%.3f %.3f %.3f) yaw=%.1f pitch=%.1f",
                SessionStateName(m_State), hz,
                static_cast<unsigned long long>(stats.waits), static_cast<unsigned long long>(stats.waitFailures),
                static_cast<unsigned long long>(stats.begins), static_cast<unsigned long long>(stats.ends),
                static_cast<unsigned long long>(ends), static_cast<unsigned long long>(projectionEnds),
                static_cast<unsigned long long>(emptyEnds), static_cast<unsigned long long>(endErrors), static_cast<int>(lastEndError),
                static_cast<unsigned long long>(stats.staleEnds), static_cast<unsigned long long>(stats.discarded),
                static_cast<unsigned long long>(stats.violations), static_cast<unsigned long long>(eyes),
                static_cast<unsigned long long>(dropped), waitAvg, waitMax,
                hmd.bPoseIsValid ? 1 : 0, m.m[0][3], m.m[1][3], m.m[2][3], yawDeg, pitchDeg);
            const vr::HmdMatrix34_t& l = hands[0].mDeviceToAbsoluteTracking;
            const vr::HmdMatrix34_t& r = hands[1].mDeviceToAbsoluteTracking;
            Game::logMsg("[VR][OpenXR][Controllers] %s | left valid=%d pos=(%.3f %.3f %.3f) fwd=(%.2f %.2f %.2f) | right valid=%d pos=(%.3f %.3f %.3f) fwd=(%.2f %.2f %.2f)",
                m_Input.StatsLine().c_str(),
                hands[0].bPoseIsValid ? 1 : 0, l.m[0][3], l.m[1][3], l.m[2][3], -l.m[0][2], -l.m[1][2], -l.m[2][2],
                hands[1].bPoseIsValid ? 1 : 0, r.m[0][3], r.m[1][3], r.m[2][3], -r.m[0][2], -r.m[1][2], -r.m[2][2]);
        }

        // ---- Frame helpers (m_Mutex held) -----------------------------------
        void BeginForGameLocked()
        {
            xrm::FrameTicket ticket;
            if (m_Ring.Begin(&ticket) != xrm::FrameRing::BeginResult::Begun)
                return;
            CallBeginFrameLocked();
        }

        void CallBeginFrameLocked()
        {
            XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
            const XrResult r = m_Xr.xrBeginFrame(m_Session, &bi);
            if (XR_FAILED(r))
            {
                // The runtime refused the frame: forget it so we never end it.
                Game::logMsg("[VR][OpenXR] xrBeginFrame failed: %s", ResultName(r).c_str());
                m_Ring.End();
            }
        }

        void EndForGameLocked()
        {
            if (!m_Ring.HasBegun())
            {
                // The game's eyes landed while a wait was in flight: if that wait
                // has finished, its frame carries them now; otherwise they go out
                // with the next frame the wait thread closes.
                if (!m_Ring.HasWaited())
                    return;
                BeginForGameLocked();
                if (!m_Ring.HasBegun())
                    return;
            }
            EndBegunLocked();
        }

        void EndStaleLocked()
        {
            if (!m_Ring.BeginStale())
                return;
            CallBeginFrameLocked();
            if (m_Ring.HasBegun())
                EndBegunLocked();
        }

        void EndBegunLocked()
        {
            xrm::FrameTicket ticket;
            if (!m_Ring.End(&ticket))
                return;
            XrCompositionLayerProjectionView views[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
            XrCompositionLayerProjection projection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
            const XrCompositionLayerBaseHeader* layers[1] = {};
            uint32_t layerCount = 0;
            const bool haveProjection = ticket.shouldRender && m_BaseSpace != XR_NULL_HANDLE &&
                m_Layer[0].valid && m_Layer[1].valid;
            if (haveProjection)
            {
                for (int e = 0; e < 2; ++e)
                {
                    views[e].pose = ToXrPose(m_Layer[e].pose);
                    views[e].fov = ToXrFov(m_Layer[e].fov);
                    views[e].subImage.swapchain = m_Layer[e].swapchain;
                    views[e].subImage.imageRect.offset = { m_Layer[e].rect.x, m_Layer[e].rect.y };
                    views[e].subImage.imageRect.extent = { m_Layer[e].rect.width, m_Layer[e].rect.height };
                    views[e].subImage.imageArrayIndex = 0;
                }
                projection.space = m_BaseSpace;
                projection.viewCount = 2;
                projection.views = views;
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                layerCount = 1;
            }
            XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
            ei.displayTime = ticket.displayTime;
            ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            ei.layerCount = layerCount;
            ei.layers = layerCount ? layers : nullptr;
            const XrResult r = m_Xr.xrEndFrame(m_Session, &ei);
            ++m_EndCalls;
            if (XR_FAILED(r))
            {
                ++m_EndErrors;
                m_LastEndError = r;
                if (m_EndErrors <= 10 || (m_EndErrors % 500) == 0)
                    Game::logMsg("[VR][OpenXR] xrEndFrame failed: %s (frame %llu, %u layers)", ResultName(r).c_str(),
                        static_cast<unsigned long long>(ticket.id), layerCount);
            }
            else if (layerCount)
                ++m_ProjectionEnds;
            else
                ++m_EmptyEnds;
        }

        vr::EVRCompositorError JoinInFlightWait(std::unique_lock<std::mutex>& lock, vr::TrackedDevicePose_t* renderPoses,
            uint32_t renderCount, vr::TrackedDevicePose_t* gamePoses, uint32_t gameCount)
        {
            // Another thread is inside xrWaitFrame: wait for it like a second
            // WaitGetPoses caller would under OpenVR, and share its frame.
            ++m_JoinedWaits;
            m_WaitCv.wait_for(lock, std::chrono::milliseconds(100), [this]() { return !m_Ring.WaitInFlight(); });
            const bool ok = !m_Ring.WaitInFlight() && m_Ring.HasWaited();
            lock.unlock();
            if (!ok)
                return vr::VRCompositorError_RequestFailed;
            CopyLatestPoses(renderPoses, renderCount, gamePoses, gameCount);
            return vr::VRCompositorError_None;
        }

        // ---- Poses ----------------------------------------------------------
        static void FillInvalid(vr::TrackedDevicePose_t* poses, uint32_t count)
        {
            for (uint32_t i = 0; poses && i < count; ++i)
            {
                poses[i] = {};
                poses[i].eTrackingResult = vr::TrackingResult_Uninitialized;
            }
        }

        bool NowXrTime(XrTime& out) const
        {
            if (!m_Xr.xrConvertWin32PerformanceCounterToTimeKHR || m_Instance == XR_NULL_HANDLE)
                return false;
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            return XR_SUCCEEDED(m_Xr.xrConvertWin32PerformanceCounterToTimeKHR(m_Instance, &qpc, &out));
        }

        bool LocateHmd(XrTime time, vr::TrackedDevicePose_t& out) const
        {
            if (m_ViewSpace == XR_NULL_HANDLE || m_BaseSpace == XR_NULL_HANDLE)
                return false;
            XrSpaceVelocity velocity{ XR_TYPE_SPACE_VELOCITY };
            XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
            location.next = &velocity;
            if (XR_FAILED(m_Xr.xrLocateSpace(m_ViewSpace, m_BaseSpace, time, &location)))
                return false;
            const bool haveVelocity = (velocity.velocityFlags & (XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)) != 0;
            out = xrm::ToTrackedPose(ToPose(location.pose), location.locationFlags,
                xrm::Vec3{ velocity.linearVelocity.x, velocity.linearVelocity.y, velocity.linearVelocity.z },
                xrm::Vec3{ velocity.angularVelocity.x, velocity.angularVelocity.y, velocity.angularVelocity.z }, haveVelocity);
            return true;
        }

        void LocateHmdAndViews(XrTime time)
        {
            vr::TrackedDevicePose_t hmd{};
            const bool haveHmd = LocateHmd(time, hmd);
            vr::TrackedDevicePose_t hands[2];
            for (int h = 0; h < 2; ++h)
                m_Input.LocateHand(h, m_BaseSpace, time, hands[h]);

            XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = time;
            li.space = m_ViewSpace;
            XrViewState vs{ XR_TYPE_VIEW_STATE };
            XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
            uint32_t n = 0;
            const bool haveViews = m_ViewSpace != XR_NULL_HANDLE &&
                XR_SUCCEEDED(m_Xr.xrLocateViews(m_Session, &li, &vs, 2, &n, views)) && n == 2 &&
                (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;

            bool notify = false;
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                if (haveHmd)
                    m_LastHmdPose = hmd;
                m_LastHandPose[0] = hands[0];
                m_LastHandPose[1] = hands[1];
                if (haveViews)
                {
                    for (int e = 0; e < 2; ++e)
                    {
                        m_EyeToHead[e] = xrm::ToMatrix(ToPose(views[e].pose));
                        m_CurrentFov[e] = ToFov(views[e].fov);
                    }
                    if (!m_ViewsReady)
                    {
                        m_ViewsReady = true;
                        notify = true;
                        Game::logMsg("[VR][OpenXR] views: L fov (%.3f %.3f %.3f %.3f) eye (%.4f %.4f %.4f), R fov (%.3f %.3f %.3f %.3f) eye (%.4f %.4f %.4f)",
                            views[0].fov.angleLeft, views[0].fov.angleRight, views[0].fov.angleUp, views[0].fov.angleDown,
                            views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
                            views[1].fov.angleLeft, views[1].fov.angleRight, views[1].fov.angleUp, views[1].fov.angleDown,
                            views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z);
                    }
                }
                if (m_Xr.xrGetDisplayRefreshRateFB && m_RefreshRateHz <= 0.0f)
                {
                    float rate = 0.0f;
                    if (XR_SUCCEEDED(m_Xr.xrGetDisplayRefreshRateFB(m_Session, &rate)))
                        m_RefreshRateHz = rate;
                }
            }
            if (notify)
                m_SessionCv.notify_all();
        }

        void CopyLatestPoses(vr::TrackedDevicePose_t* renderPoses, uint32_t renderCount,
            vr::TrackedDevicePose_t* gamePoses, uint32_t gameCount)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (renderPoses && renderCount > kHmd)
                renderPoses[kHmd] = m_LastHmdPose;
            if (gamePoses && gameCount > kHmd)
                gamePoses[kHmd] = m_LastHmdPose;
            if (renderPoses && renderCount > kRightHand)
            {
                renderPoses[kLeftHand] = m_LastHandPose[0];
                renderPoses[kRightHand] = m_LastHandPose[1];
            }
            if (gamePoses && gameCount > kRightHand)
            {
                gamePoses[kLeftHand] = m_LastHandPose[0];
                gamePoses[kRightHand] = m_LastHandPose[1];
            }
        }

        // GetProjectionRaw / GetEyeToHeadTransform need a located view, which
        // needs the session (DXVK's device). VR::VR asks for them on its own
        // thread before the D3D9 device exists, so wait here (bounded).
        void WaitForViews()
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            if (m_ViewsReady)
                return;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!m_ViewsReady && std::chrono::steady_clock::now() < deadline)
            {
                if (m_Session != XR_NULL_HANDLE && m_ViewSpace != XR_NULL_HANDLE)
                {
                    // Views can be located before the first frame: ask at "now"
                    // (or any recent time) directly.
                    lock.unlock();
                    XrTime now = 0;
                    if (NowXrTime(now))
                        LocateHmdAndViews(now);
                    lock.lock();
                    if (m_ViewsReady)
                        break;
                }
                m_SessionCv.wait_for(lock, std::chrono::milliseconds(50));
            }
            if (!m_ViewsReady)
                Game::logMsg("[VR][OpenXR] no located views after 60 s (session %s)", m_Session != XR_NULL_HANDLE ? "created" : "missing");
        }

        // ---- Swapchains and the eye copy (queue lock + m_Mutex held) --------
        int64_t PickSwapchainFormat(VkFormat source) const
        {
            auto offered = [&](VkFormat f) {
                return std::find(m_SwapchainFormats.begin(), m_SwapchainFormats.end(), static_cast<int64_t>(f)) != m_SwapchainFormats.end();
            };
            // Same bits, sRGB view: the game's UNORM eye images hold gamma-encoded
            // colour (OpenVR ColorSpace_Auto treats them so), so copying them
            // unchanged into an sRGB swapchain shows them as players see today.
            VkFormat preferred[2] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
            switch (source)
            {
            case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
                preferred[0] = VK_FORMAT_B8G8R8A8_SRGB; preferred[1] = VK_FORMAT_B8G8R8A8_UNORM; break;
            case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
                preferred[0] = VK_FORMAT_R8G8B8A8_SRGB; preferred[1] = VK_FORMAT_R8G8B8A8_UNORM; break;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
                preferred[0] = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break;
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                preferred[0] = VK_FORMAT_R16G16B16A16_SFLOAT; break;
            default:
                break;
            }
            for (VkFormat f : preferred)
                if (f != VK_FORMAT_UNDEFINED && offered(f))
                    return f;
            return 0;
        }

        bool EnsureSwapchainLocked(int eye, uint32_t width, uint32_t height, VkFormat source)
        {
            Swapchain& sc = m_Swapchain[eye];
            if (sc.handle != XR_NULL_HANDLE && sc.width == width && sc.height == height && sc.sourceFormat == source)
                return true;
            // The menu/loading path submits the 512x512 blank texture between
            // eye frames: keep a few swapchains per eye and switch between them
            // instead of recreating one on every change.
            std::vector<Swapchain>& pool = m_SwapchainPool[eye];
            if (sc.handle != XR_NULL_HANDLE)
                pool.push_back(sc);
            sc = Swapchain();
            for (auto it = pool.begin(); it != pool.end(); ++it)
            {
                if (it->width == width && it->height == height && it->sourceFormat == source)
                {
                    sc = *it;
                    pool.erase(it);
                    return true;
                }
            }
            while (pool.size() > 2)
            {
                // Never destroy a swapchain a pending layer still shows.
                auto victim = std::find_if(pool.begin(), pool.end(), [this](const Swapchain& old) {
                    return old.handle != m_Layer[0].swapchain && old.handle != m_Layer[1].swapchain;
                });
                if (victim == pool.end())
                    break;
                if (victim->acquired >= 0)
                {
                    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                    m_Xr.xrReleaseSwapchainImage(victim->handle, &ri);
                }
                m_Xr.xrDestroySwapchain(victim->handle);
                pool.erase(victim);
            }
            const int64_t format = PickSwapchainFormat(source);
            if (!format)
            {
                if (!m_LoggedNoFormat)
                {
                    m_LoggedNoFormat = true;
                    Game::logMsg("[VR][OpenXR] no swapchain format matches the eye image format %d", static_cast<int>(source));
                }
                return false;
            }
            XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            ci.format = format;
            ci.sampleCount = 1;
            ci.width = width;
            ci.height = height;
            ci.faceCount = 1;
            ci.arraySize = 1;
            ci.mipCount = 1;
            XrResult r = m_Xr.xrCreateSwapchain(m_Session, &ci, &sc.handle);
            if (XR_FAILED(r))
            {
                sc.handle = XR_NULL_HANDLE;
                Game::logMsg("[VR][OpenXR] xrCreateSwapchain(eye %d, %ux%u, format %lld) failed: %s", eye, width, height,
                    static_cast<long long>(format), ResultName(r).c_str());
                return false;
            }
            uint32_t n = 0;
            m_Xr.xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
            std::vector<XrSwapchainImageVulkanKHR> images(n, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
            r = m_Xr.xrEnumerateSwapchainImages(sc.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
            if (XR_FAILED(r) || n == 0)
            {
                m_Xr.xrDestroySwapchain(sc.handle);
                sc = Swapchain();
                return false;
            }
            for (const auto& image : images)
                sc.images.push_back(image.image);
            sc.width = width;
            sc.height = height;
            sc.sourceFormat = source;
            sc.format = format;
            Game::logMsg("[VR][OpenXR] eye %d swapchain %ux%u format %lld (eye image format %d), %u images", eye, width, height,
                static_cast<long long>(format), static_cast<int>(source), n);
            return true;
        }

        static void ImageBarrier(const VkApi& vk, VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
            VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
        {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vk.vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        bool CopyIntoSwapchainLocked(int eye, VkImage source)
        {
            Swapchain& sc = m_Swapchain[eye];
            const VkDevice device = static_cast<VkDevice>(m_Device.device);
            XrResult r = XR_SUCCESS;
            if (sc.acquired < 0)
            {
                XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                uint32_t acquiredIndex = 0;
                r = m_Xr.xrAcquireSwapchainImage(sc.handle, &ai, &acquiredIndex);
                if (XR_FAILED(r))
                {
                    Game::logMsg("[VR][OpenXR] xrAcquireSwapchainImage(eye %d) failed: %s", eye, ResultName(r).c_str());
                    return false;
                }
                sc.acquired = acquiredIndex;
            }
            const uint32_t index = static_cast<uint32_t>(sc.acquired);
            XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = 100000000;  // 100 ms
            r = m_Xr.xrWaitSwapchainImage(sc.handle, &wi);
            if (r != XR_SUCCESS)
            {
                Game::logMsg("[VR][OpenXR] xrWaitSwapchainImage(eye %d) returned %s", eye, ResultName(r).c_str());
                if (r == XR_TIMEOUT_EXPIRED)
                    return false;  // stays acquired; the next submit waits for it again
                XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                m_Xr.xrReleaseSwapchainImage(sc.handle, &ri);
                sc.acquired = -1;
                return false;
            }
            const VkImage target = sc.images[index % sc.images.size()];

            // Optional debug readback of what the runtime receives (self-test shots).
            const bool dump = eye == 0 && !m_DumpPath.empty();
            VkBuffer dumpBuffer = VK_NULL_HANDLE;
            VkDeviceMemory dumpMemory = VK_NULL_HANDLE;
            const VkDeviceSize dumpSize = static_cast<VkDeviceSize>(sc.width) * sc.height * 4u;
            bool dumpReady = dump && (sc.format == VK_FORMAT_B8G8R8A8_SRGB || sc.format == VK_FORMAT_R8G8B8A8_SRGB ||
                sc.format == VK_FORMAT_B8G8R8A8_UNORM || sc.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                CreateHostBuffer(dumpSize, dumpBuffer, dumpMemory);

            VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            m_Vk.vkResetCommandBuffer(m_CommandBuffer, 0);
            m_Vk.vkBeginCommandBuffer(m_CommandBuffer, &bi);
            // DXVK left the eye image in TRANSFER_SRC_OPTIMAL (TransferSurface) and
            // flushed its writes to this queue before we got the queue lock; make
            // them visible to our transfer read.
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            m_Vk.vkCmdPipelineBarrier(m_CommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                1, &mb, 0, nullptr, 0, nullptr);
            ImageBarrier(m_Vk, m_CommandBuffer, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.extent = { sc.width, sc.height, 1 };
            m_Vk.vkCmdCopyImage(m_CommandBuffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            if (dumpReady)
            {
                ImageBarrier(m_Vk, m_CommandBuffer, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy bic{};
                bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                bic.imageExtent = { sc.width, sc.height, 1 };
                m_Vk.vkCmdCopyImageToBuffer(m_CommandBuffer, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dumpBuffer, 1, &bic);
                ImageBarrier(m_Vk, m_CommandBuffer, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
            else
            {
                // OpenXR wants color swapchain images released in COLOR_ATTACHMENT_OPTIMAL.
                ImageBarrier(m_Vk, m_CommandBuffer, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
            m_Vk.vkEndCommandBuffer(m_CommandBuffer);
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1;
            si.pCommandBuffers = &m_CommandBuffer;
            m_Vk.vkResetFences(device, 1, &m_Fence);
            const VkResult vr = m_Vk.vkQueueSubmit(static_cast<VkQueue>(m_Device.queue), 1, &si, m_Fence);
            bool copied = vr == VK_SUCCESS;
            if (copied)
                copied = m_Vk.vkWaitForFences(device, 1, &m_Fence, VK_TRUE, 1000000000ull) == VK_SUCCESS;
            if (!copied)
                Game::logMsg("[VR][OpenXR] eye %d copy failed (vkQueueSubmit %d)", eye, static_cast<int>(vr));

            if (dumpReady)
            {
                if (copied)
                    WriteDump(dumpMemory, sc, m_Layer[eye]);
                m_Vk.vkDestroyBuffer(device, dumpBuffer, nullptr);
                m_Vk.vkFreeMemory(device, dumpMemory, nullptr);
            }
            if (dump)
                m_DumpPath.clear();

            XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            r = m_Xr.xrReleaseSwapchainImage(sc.handle, &ri);
            sc.acquired = -1;
            if (XR_FAILED(r))
            {
                Game::logMsg("[VR][OpenXR] xrReleaseSwapchainImage(eye %d) failed: %s", eye, ResultName(r).c_str());
                return false;
            }
            sc.released = true;
            return copied;
        }

        bool CreateHostBuffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory)
        {
            const VkDevice device = static_cast<VkDevice>(m_Device.device);
            VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ci.size = size;
            ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (m_Vk.vkCreateBuffer(device, &ci, nullptr, &buffer) != VK_SUCCESS)
                return false;
            VkMemoryRequirements req{};
            m_Vk.vkGetBufferMemoryRequirements(device, buffer, &req);
            uint32_t type = UINT32_MAX;
            const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; ++i)
                if ((req.memoryTypeBits & (1u << i)) && (m_MemoryProperties.memoryTypes[i].propertyFlags & want) == want)
                {
                    type = i;
                    break;
                }
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = type;
            if (type == UINT32_MAX || m_Vk.vkAllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS)
            {
                m_Vk.vkDestroyBuffer(device, buffer, nullptr);
                buffer = VK_NULL_HANDLE;
                return false;
            }
            m_Vk.vkBindBufferMemory(device, buffer, memory, 0);
            return true;
        }

        // 32-bit top-down BMP of the swapchain image as the runtime receives it,
        // with the submitted image rect outlined in magenta.
        void WriteDump(VkDeviceMemory memory, const Swapchain& sc, const LayerView& previous)
        {
            void* mapped = nullptr;
            const VkDevice device = static_cast<VkDevice>(m_Device.device);
            if (m_Vk.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped)
                return;
            const uint32_t w = sc.width, h = sc.height;
            std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4u);
            std::memcpy(pixels.data(), mapped, pixels.size());
            m_Vk.vkUnmapMemory(device, memory);
            const bool rgba = sc.format == VK_FORMAT_R8G8B8A8_SRGB || sc.format == VK_FORMAT_R8G8B8A8_UNORM;
            if (rgba)
                for (size_t i = 0; i < pixels.size(); i += 4)
                    std::swap(pixels[i], pixels[i + 2]);
            uint64_t sum = 0;
            for (size_t i = 0; i < pixels.size(); i += 4)
                sum += pixels[i] + pixels[i + 1] + pixels[i + 2];
            if (previous.valid)
            {
                const xrm::Rect& rc = previous.rect;
                auto put = [&](int32_t x, int32_t y) {
                    if (x < 0 || y < 0 || x >= static_cast<int32_t>(w) || y >= static_cast<int32_t>(h)) return;
                    uint8_t* p = &pixels[(static_cast<size_t>(y) * w + x) * 4u];
                    p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 255;
                };
                for (int32_t x = rc.x; x < rc.x + rc.width; ++x) { put(x, rc.y); put(x, rc.y + rc.height - 1); }
                for (int32_t y = rc.y; y < rc.y + rc.height; ++y) { put(rc.x, y); put(rc.x + rc.width - 1, y); }
            }
            FILE* file = nullptr;
            if (fopen_s(&file, m_DumpPath.c_str(), "wb") != 0 || !file)
                return;
            const uint32_t dataSize = static_cast<uint32_t>(pixels.size());
            uint8_t header[54] = { 'B', 'M' };
            auto put32 = [&](int offset, uint32_t v) { std::memcpy(header + offset, &v, 4); };
            auto put16 = [&](int offset, uint16_t v) { std::memcpy(header + offset, &v, 2); };
            put32(2, 54 + dataSize); put32(10, 54); put32(14, 40); put32(18, w);
            put32(22, static_cast<uint32_t>(-static_cast<int32_t>(h))); put16(26, 1); put16(28, 32); put32(34, dataSize);
            fwrite(header, 1, sizeof(header), file);
            fwrite(pixels.data(), 1, pixels.size(), file);
            fclose(file);
            Game::logMsg("[VR][OpenXR] wrote the left-eye swapchain image (%ux%u format %lld, mean %.1f, last rect %d,%d %dx%d) to %s",
                w, h, static_cast<long long>(sc.format), static_cast<double>(sum) / (3.0 * w * h),
                previous.rect.x, previous.rect.y, previous.rect.width, previous.rect.height, m_DumpPath.c_str());
        }

        // ---- State ----------------------------------------------------------
        std::mutex m_InitMutex;
        bool m_InitAttempted = false;
        bool m_InitOk = false;
        std::string m_InitFailure;

        HMODULE m_Loader = nullptr;
        XrApi m_Xr;
        XrInstance m_Instance = XR_NULL_HANDLE;
        XrSystemId m_SystemId = XR_NULL_SYSTEM_ID;
        std::set<std::string> m_Extensions;
        std::string m_RuntimeName;
        std::vector<XrViewConfigurationView> m_ViewConfig;
        uint32_t m_MaxLayers = 0;
        VkPhysicalDevice m_RuntimePhysicalDevice = VK_NULL_HANDLE;

        L4D2VR_VkDeviceInfo m_Device{};
        VkApi m_Vk;
        VkPhysicalDeviceMemoryProperties m_MemoryProperties{};
        VkCommandPool m_CommandPool = VK_NULL_HANDLE;
        VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;
        VkFence m_Fence = VK_NULL_HANDLE;

        std::mutex m_Mutex;
        std::condition_variable m_WaitCv;
        std::condition_variable m_SessionCv;
        XrSession m_Session = XR_NULL_HANDLE;
        XrSpace m_BaseSpace = XR_NULL_HANDLE;
        XrSpace m_ViewSpace = XR_NULL_HANDLE;
        XrReferenceSpaceType m_BaseSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        std::vector<int64_t> m_SwapchainFormats;
        XrSessionState m_State = XR_SESSION_STATE_UNKNOWN;
        std::atomic<bool> m_Running{ false };
        std::atomic<bool> m_Lost{ false };
        bool m_LoggedFocused = false;
        bool m_ExplicitTiming = false;

        xrm::FrameRing m_Ring;
        int64_t m_LastDisplayTime = 0;
        int64_t m_LastDisplayPeriod = 0;
        XrResult m_LastWaitError = XR_SUCCESS;
        float m_RefreshRateHz = 0.0f;

        vr::TrackedDevicePose_t m_LastHmdPose{};
        vr::TrackedDevicePose_t m_LastHandPose[2]{};
        vr::HmdMatrix34_t m_EyeToHead[2] = {
            { { { 1, 0, 0, -0.032f }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } } },
            { { { 1, 0, 0, 0.032f }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } } } };
        xrm::Fov m_CurrentFov[2];
        xrm::Fov m_RenderFov[2];
        bool m_RenderFovValid[2] = { false, false };
        bool m_ViewsReady = false;

        Swapchain m_Swapchain[2];
        std::vector<Swapchain> m_SwapchainPool[2];
        LayerView m_Layer[2];
        std::string m_DumpPath;
        bool m_LoggedFlippedBounds = false;
        bool m_LoggedNoFormat = false;

        uint64_t m_EndCalls = 0, m_ProjectionEnds = 0, m_EmptyEnds = 0, m_EndErrors = 0;
        XrResult m_LastEndError = XR_SUCCESS;
        uint64_t m_SubmittedEyes = 0, m_DroppedSubmits = 0, m_JoinedWaits = 0;
        uint64_t m_StatsPrevWaits = 0;
        double m_WaitMsSum = 0.0, m_WaitMsMax = 0.0;

        XrInputRuntime m_Input;
        XrSystemFacade m_SystemFacade;
        XrCompositorFacade m_CompositorFacade;
        XrOverlayFacade m_OverlayFacade;
        XrRenderModelsFacade m_RenderModelsFacade;
        XrApplicationsFacade m_ApplicationsFacade;
    };

    OpenXrRuntime& Runtime()
    {
        static OpenXrRuntime* runtime = new OpenXrRuntime();  // lives until the process exits
        return *runtime;
    }

    // ---- Facade forwarding ---------------------------------------------------
    void XrSystemFacade::GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) { m_Rt.RecommendedSize(pnWidth, pnHeight); }
    void XrSystemFacade::GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom)
    { m_Rt.ProjectionRaw(eEye, pfLeft, pfRight, pfTop, pfBottom); }
    vr::HmdMatrix34_t XrSystemFacade::GetEyeToHeadTransform(vr::EVREye eEye) { return m_Rt.EyeToHead(eEye); }
    void XrSystemFacade::GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin, float fPredictedSecondsToPhotonsFromNow,
        vr::TrackedDevicePose_t* pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount)
    { m_Rt.PredictPoses(fPredictedSecondsToPhotonsFromNow, pTrackedDevicePoseArray, unTrackedDevicePoseArrayCount); }
    bool XrSystemFacade::IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t unDeviceIndex)
    {
        if (unDeviceIndex == kHmd)
            return m_Rt.HmdConnected();
        return m_Rt.Input().Connected(XrInputRuntime::HandOfDevice(unDeviceIndex));
    }
    vr::TrackedDeviceIndex_t XrSystemFacade::GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole role)
    {
        const int hand = role == vr::TrackedControllerRole_LeftHand ? 0 : role == vr::TrackedControllerRole_RightHand ? 1 : -1;
        if (hand < 0 || !m_Rt.Input().Connected(hand))
            return vr::k_unTrackedDeviceIndexInvalid;
        return hand == 0 ? kLeftHand : kRightHand;
    }
    vr::ETrackedDeviceClass XrSystemFacade::GetTrackedDeviceClass(vr::TrackedDeviceIndex_t unDeviceIndex)
    {
        if (unDeviceIndex == kHmd)
            return vr::TrackedDeviceClass_HMD;
        return m_Rt.Input().Connected(XrInputRuntime::HandOfDevice(unDeviceIndex)) ? vr::TrackedDeviceClass_Controller : vr::TrackedDeviceClass_Invalid;
    }
    int32_t XrSystemFacade::GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop,
        vr::ETrackedPropertyError* pError)
    {
        const int hand = XrInputRuntime::HandOfDevice(unDeviceIndex);
        if (hand >= 0 && m_Rt.Input().Connected(hand))
        {
            if (prop == vr::Prop_ControllerRoleHint_Int32)
            {
                if (pError) *pError = vr::TrackedProp_Success;
                return hand == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
            }
            if (prop == vr::Prop_DeviceClass_Int32)
            {
                if (pError) *pError = vr::TrackedProp_Success;
                return vr::TrackedDeviceClass_Controller;
            }
        }
        if (pError) *pError = vr::TrackedProp_UnknownProperty;
        return 0;
    }
    uint32_t XrSystemFacade::GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop,
        char* pchValue, uint32_t unBufferSize, vr::ETrackedPropertyError* pError)
    {
        if (pchValue && unBufferSize)
            pchValue[0] = '\0';
        const int hand = XrInputRuntime::HandOfDevice(unDeviceIndex);
        std::string value;
        if (hand >= 0 && m_Rt.Input().Connected(hand))
        {
            const l4d2vr_openxr_input::Profile* p = m_Rt.Input().RuntimeProfile(hand);
            if (prop == vr::Prop_RenderModelName_String)
                value = p ? p->renderModel[hand] : "generic_controller";
            else if (prop == vr::Prop_ControllerType_String)
                value = p ? p->controllerType : "generic";
            else if (prop == vr::Prop_ModelNumber_String)
                value = p ? p->path : "unknown";
            else if (prop == vr::Prop_TrackingSystemName_String)
                value = "openxr";
        }
        else if (unDeviceIndex == kHmd && prop == vr::Prop_TrackingSystemName_String)
            value = "openxr";
        if (value.empty())
        {
            if (pError) *pError = vr::TrackedProp_UnknownProperty;
            return 0;
        }
        const uint32_t needed = static_cast<uint32_t>(value.size() + 1);
        if (!pchValue || unBufferSize < needed)
        {
            if (pError) *pError = vr::TrackedProp_BufferTooSmall;
            return needed;
        }
        std::memcpy(pchValue, value.c_str(), needed);
        if (pError) *pError = vr::TrackedProp_Success;
        return needed;
    }
    void XrSystemFacade::TriggerHapticPulse(vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t, unsigned short usDurationMicroSec)
    {
        // Legacy width-only pulse: SteamVR controllers buzz harder for longer
        // pulses (0.5-4 ms in the mod); keep that feel with a short vibration.
        const float strength = std::clamp(static_cast<float>(usDurationMicroSec) / 4000.0f, 0.0f, 1.0f);
        m_Rt.Input().Vibrate(XrInputRuntime::HandOfDevice(unControllerDeviceIndex), 0.01f + 0.02f * strength, 0.0f, 0.25f + 0.75f * strength);
    }
    bool XrSystemFacade::TriggerHapticVibration(vr::TrackedDeviceIndex_t unControllerDeviceIndex, float durationSeconds, float frequency,
        float amplitude)
    {
        return m_Rt.Input().Vibrate(XrInputRuntime::HandOfDevice(unControllerDeviceIndex), durationSeconds, frequency, amplitude);
    }
    float XrSystemFacade::GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop,
        vr::ETrackedPropertyError* pError)
    {
        if (unDeviceIndex == kHmd && prop == vr::Prop_DisplayFrequency_Float)
        {
            if (pError) *pError = vr::TrackedProp_Success;
            return m_Rt.DisplayHz();
        }
        if (pError) *pError = vr::TrackedProp_UnknownProperty;
        return 0.0f;
    }

    vr::ETrackingUniverseOrigin XrCompositorFacade::GetTrackingSpace() { return m_Rt.TrackingOrigin(); }
    vr::EVRCompositorError XrCompositorFacade::WaitGetPoses(vr::TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount,
        vr::TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount)
    { return m_Rt.WaitGetPoses(pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount); }
    vr::EVRCompositorError XrCompositorFacade::Submit(vr::EVREye eEye, const vr::Texture_t* pTexture,
        const vr::VRTextureBounds_t* pBounds, vr::EVRSubmitFlags nSubmitFlags)
    { return m_Rt.Submit(eEye, pTexture, pBounds, nSubmitFlags); }
    void XrCompositorFacade::ClearLastSubmittedFrame() { m_Rt.ClearLastSubmittedFrame(); }
    void XrCompositorFacade::PostPresentHandoff() { m_Rt.PostPresentHandoff(); }
    bool XrCompositorFacade::GetFrameTiming(vr::Compositor_FrameTiming* pTiming, uint32_t) { return m_Rt.GetFrameTiming(pTiming); }
    float XrCompositorFacade::GetFrameTimeRemaining() { return m_Rt.FrameTimeRemaining(); }
    void XrCompositorFacade::SetExplicitTimingMode(vr::EVRCompositorTimingMode eTimingMode) { m_Rt.SetExplicitTimingMode(eTimingMode); }
    vr::EVRCompositorError XrCompositorFacade::SubmitExplicitTimingData() { return m_Rt.SubmitExplicitTimingData(); }

    class OpenXrBackend final : public IVrBackend
    {
    public:
        RuntimeKind Kind() const override { return RuntimeKind::OpenXR; }
        const char* Name() const override { return "OpenXR"; }

        Caps GetCaps() const override
        {
            Caps caps;  // no overlay mouse, intersection or keyboard (M4), no skeletal bones (M5), no runtime assets
            caps.maxOverlayLayers = Runtime().MaxLayers();
            return caps;
        }

        bool IsRuntimeInstalled() override
        {
            std::string reason;
            return Runtime().EnsureInstance(reason);
        }

        bool Init(const char*& errorText) override
        {
            std::string reason;
            if (Runtime().EnsureInstance(reason))
            {
                errorText = nullptr;
                return true;
            }
            errorText = Runtime().InitFailureText();
            return false;
        }

        bool GetRuntimePath(char*, uint32_t, uint32_t* punRequiredBufferSize) override
        {
            if (punRequiredBufferSize) *punRequiredBufferSize = 0;
            return false;
        }

        IVrSystem* System() override { return Runtime().InstanceReady() ? Runtime().SystemFacade() : nullptr; }
        IVrCompositor* Compositor() override { return Runtime().InstanceReady() ? Runtime().CompositorFacade() : nullptr; }
        IVrInput* Input() override { return Runtime().InstanceReady() ? Runtime().InputFacade() : nullptr; }
        IVrOverlay* Overlay() override { return Runtime().InstanceReady() ? Runtime().OverlayFacade() : nullptr; }
        IVrRenderModels* RenderModels() override { return Runtime().InstanceReady() ? Runtime().RenderModelsFacade() : nullptr; }
        IVrApplications* Applications() override { return Runtime().InstanceReady() ? Runtime().ApplicationsFacade() : nullptr; }
        bool RequestSubmittedEyeDump(const char* path) override { return Runtime().RequestEyeDump(path); }
        bool SetFakeInput(const char* spec, std::string& error) override
        {
            if (!Runtime().InstanceReady())
            {
                error = "OpenXR is not running";
                return false;
            }
            return Runtime().Input().SetFake(spec, error);
        }
        std::string DescribeControllers() override { return Runtime().InstanceReady() ? Runtime().Input().Describe() : std::string(); }
    };
}

IVrBackend& OpenXrBackendInstance()
{
    static OpenXrBackend backend;
    return backend;
}

bool OpenXrProbeRuntime(std::string& reason)
{
    return Runtime().EnsureInstance(reason);
}
}

namespace
{
    bool CopyExtensionString(const std::string& text, char* buffer, uint32_t capacity, uint32_t* length)
    {
        if (length)
            *length = static_cast<uint32_t>(text.size() + 1);
        if (!buffer)
            return true;
        if (capacity < text.size() + 1)
            return false;
        std::memcpy(buffer, text.c_str(), text.size() + 1);
        return true;
    }
}

extern "C" bool L4D2VR_XrGetVulkanInstanceExtensions(char* buffer, uint32_t capacity, uint32_t* length)
{
    std::string text;
    if (!l4d2vr_backend::Runtime().VulkanExtensions(false, text))
        return false;
    return CopyExtensionString(text, buffer, capacity, length);
}

extern "C" bool L4D2VR_XrGetVulkanDeviceExtensions(char* buffer, uint32_t capacity, uint32_t* length)
{
    std::string text;
    if (!l4d2vr_backend::Runtime().VulkanExtensions(true, text))
        return false;
    return CopyExtensionString(text, buffer, capacity, length);
}

extern "C" bool L4D2VR_XrGetVulkanGraphicsDevice(void* vkInstance, void** physicalDevice)
{
    return l4d2vr_backend::Runtime().GraphicsDevice(vkInstance, physicalDevice);
}

extern "C" void L4D2VR_VrBackendDeviceCreated(const L4D2VR_VkDeviceInfo* info)
{
    if (!info || info->size < sizeof(L4D2VR_VkDeviceInfo))
        return;
    if (L4D2VR_SelectedVrRuntime() != static_cast<int>(l4d2vr_backend::RuntimeKind::OpenXR))
        return;
    l4d2vr_backend::Runtime().DeviceCreated(*info);
}

#pragma once

// VR runtime seam (OpenXR port, milestone M1).
//
// The mod talks to the VR runtime only through IVrBackend and the
// sub-interfaces below. Each sub-interface is the subset of the matching
// OpenVR interface the mod actually calls, with the same method names and
// signatures, so call sites and the templates over System/Input/Models
// (l4d2vr_controller_identity, l4d2vr_controller_tip, l4d2vr_hand_input)
// stay unchanged. The OpenVR value types (vr::HmdMatrix34_t,
// vr::TrackedDevicePose_t, handles, error enums) remain the mod's vocabulary:
// they are header-only and need no runtime.
//
// Backends:
//   vr_backend_openvr.cpp  pass-through to openvr_api (today's behaviour).
//   vr_backend_openxr.cpp  OpenXR on DXVK's Vulkan device: HMD pose and eye
//                          frames (M2), controllers and input (M3,
//                          openxr_input_runtime.inl); overlays follow in M4.
//                          Selected by VrRuntime=openxr or -l4d2vr_openxr
//                          (vr_runtime_select.h).
//
// Every accessor returns nullptr when the runtime has no such interface (for
// OpenVR: before VR_Init, or in desktop mode), exactly like the vr::VRxxx()
// free functions it replaces.

#include "openvr.h"

#include <cstdint>
#include <string>

namespace l4d2vr_backend
{
    // Values match l4d2vr_runtime_select::Runtime and L4D2VR_SelectedVrRuntime().
    enum class RuntimeKind : int
    {
        OpenVR = 1,
        OpenXR = 2,
    };

    // IVRSystem subset: display geometry, device identity, raw poses, haptics.
    class IVrSystem
    {
    public:
        virtual ~IVrSystem() = default;
        virtual void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) = 0;
        virtual void GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) = 0;
        virtual vr::HmdMatrix34_t GetEyeToHeadTransform(vr::EVREye eEye) = 0;
        virtual void GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow,
            vr::TrackedDevicePose_t* pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount) = 0;
        virtual vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole unDeviceType) = 0;
        virtual vr::ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t unDeviceIndex) = 0;
        virtual bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t unDeviceIndex) = 0;
        virtual float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop,
            vr::ETrackedPropertyError* pError = nullptr) = 0;
        virtual int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop,
            vr::ETrackedPropertyError* pError = nullptr) = 0;
        virtual uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop,
            char* pchValue, uint32_t unBufferSize, vr::ETrackedPropertyError* pError = nullptr) = 0;
        virtual void TriggerHapticPulse(vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId, unsigned short usDurationMicroSec) = 0;
        // A haptic pulse with its real duration, frequency (Hz, 0 = runtime
        // default) and amplitude (0..1). False when the runtime only has the
        // legacy width-only pulse (OpenVR); the caller then folds the profile
        // into TriggerHapticPulse.
        virtual bool TriggerHapticVibration(vr::TrackedDeviceIndex_t unControllerDeviceIndex, float durationSeconds, float frequency,
            float amplitude)
        {
            (void)unControllerDeviceIndex; (void)durationSeconds; (void)frequency; (void)amplitude;
            return false;
        }
    };

    // IVRCompositor subset: frame pacing and eye submission.
    class IVrCompositor
    {
    public:
        virtual ~IVrCompositor() = default;
        virtual vr::ETrackingUniverseOrigin GetTrackingSpace() = 0;
        virtual vr::EVRCompositorError WaitGetPoses(vr::TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount,
            vr::TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) = 0;
        virtual vr::EVRCompositorError Submit(vr::EVREye eEye, const vr::Texture_t* pTexture,
            const vr::VRTextureBounds_t* pBounds = nullptr, vr::EVRSubmitFlags nSubmitFlags = vr::Submit_Default) = 0;
        virtual void ClearLastSubmittedFrame() = 0;
        virtual void PostPresentHandoff() = 0;
        virtual bool GetFrameTiming(vr::Compositor_FrameTiming* pTiming, uint32_t unFramesAgo = 0) = 0;
        virtual float GetFrameTimeRemaining() = 0;
        virtual void SetExplicitTimingMode(vr::EVRCompositorTimingMode eTimingMode) = 0;
        virtual vr::EVRCompositorError SubmitExplicitTimingData() = 0;
    };

    // IVRInput subset: action manifest, action reads, origins, skeletal data.
    class IVrInput
    {
    public:
        virtual ~IVrInput() = default;
        virtual vr::EVRInputError SetActionManifestPath(const char* pchActionManifestPath) = 0;
        virtual vr::EVRInputError GetActionSetHandle(const char* pchActionSetName, vr::VRActionSetHandle_t* pHandle) = 0;
        virtual vr::EVRInputError GetActionHandle(const char* pchActionName, vr::VRActionHandle_t* pHandle) = 0;
        virtual vr::EVRInputError GetInputSourceHandle(const char* pchInputSourcePath, vr::VRInputValueHandle_t* pHandle) = 0;
        virtual vr::EVRInputError UpdateActionState(vr::VRActiveActionSet_t* pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount) = 0;
        virtual vr::EVRInputError GetDigitalActionData(vr::VRActionHandle_t action, vr::InputDigitalActionData_t* pActionData,
            uint32_t unActionDataSize, vr::VRInputValueHandle_t ulRestrictToDevice) = 0;
        virtual vr::EVRInputError GetAnalogActionData(vr::VRActionHandle_t action, vr::InputAnalogActionData_t* pActionData,
            uint32_t unActionDataSize, vr::VRInputValueHandle_t ulRestrictToDevice) = 0;
        virtual vr::EVRInputError GetSkeletalActionData(vr::VRActionHandle_t action, vr::InputSkeletalActionData_t* pActionData,
            uint32_t unActionDataSize) = 0;
        virtual vr::EVRInputError GetBoneCount(vr::VRActionHandle_t action, uint32_t* pBoneCount) = 0;
        virtual vr::EVRInputError GetBoneHierarchy(vr::VRActionHandle_t action, vr::BoneIndex_t* pParentIndices, uint32_t unIndexArayCount) = 0;
        virtual vr::EVRInputError GetBoneName(vr::VRActionHandle_t action, vr::BoneIndex_t nBoneIndex, char* pchBoneName, uint32_t unNameBufferSize) = 0;
        virtual vr::EVRInputError GetSkeletalReferenceTransforms(vr::VRActionHandle_t action, vr::EVRSkeletalTransformSpace eTransformSpace,
            vr::EVRSkeletalReferencePose eReferencePose, vr::VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount) = 0;
        virtual vr::EVRInputError GetSkeletalBoneData(vr::VRActionHandle_t action, vr::EVRSkeletalTransformSpace eTransformSpace,
            vr::EVRSkeletalMotionRange eMotionRange, vr::VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount) = 0;
        virtual vr::EVRInputError GetSkeletalSummaryData(vr::VRActionHandle_t action, vr::EVRSummaryType eSummaryType,
            vr::VRSkeletalSummaryData_t* pSkeletalSummaryData) = 0;
        virtual vr::EVRInputError GetOriginTrackedDeviceInfo(vr::VRInputValueHandle_t origin, vr::InputOriginInfo_t* pOriginInfo,
            uint32_t unOriginInfoSize) = 0;
        virtual vr::EVRInputError GetActionBindingInfo(vr::VRActionHandle_t action, vr::InputBindingInfo_t* pOriginInfo,
            uint32_t unBindingInfoSize, uint32_t unBindingInfoCount, uint32_t* punReturnedBindingInfoCount) = 0;
    };

    // IVROverlay subset: every overlay panel, the overlay mouse and the VR keyboard.
    class IVrOverlay
    {
    public:
        virtual ~IVrOverlay() = default;
        virtual vr::EVROverlayError FindOverlay(const char* pchOverlayKey, vr::VROverlayHandle_t* pOverlayHandle) = 0;
        virtual vr::EVROverlayError CreateOverlay(const char* pchOverlayKey, const char* pchOverlayName, vr::VROverlayHandle_t* pOverlayHandle) = 0;
        virtual vr::EVROverlayError DestroyOverlay(vr::VROverlayHandle_t ulOverlayHandle) = 0;
        virtual vr::EVROverlayError SetOverlayFlag(vr::VROverlayHandle_t ulOverlayHandle, vr::VROverlayFlags eOverlayFlag, bool bEnabled) = 0;
        virtual vr::EVROverlayError SetOverlayAlpha(vr::VROverlayHandle_t ulOverlayHandle, float fAlpha) = 0;
        virtual vr::EVROverlayError SetOverlayTexelAspect(vr::VROverlayHandle_t ulOverlayHandle, float fTexelAspect) = 0;
        virtual vr::EVROverlayError SetOverlaySortOrder(vr::VROverlayHandle_t ulOverlayHandle, uint32_t unSortOrder) = 0;
        virtual vr::EVROverlayError SetOverlayWidthInMeters(vr::VROverlayHandle_t ulOverlayHandle, float fWidthInMeters) = 0;
        virtual vr::EVROverlayError GetOverlayWidthInMeters(vr::VROverlayHandle_t ulOverlayHandle, float* pfWidthInMeters) = 0;
        virtual vr::EVROverlayError SetOverlayCurvature(vr::VROverlayHandle_t ulOverlayHandle, float fCurvature) = 0;
        virtual vr::EVROverlayError GetOverlayCurvature(vr::VROverlayHandle_t ulOverlayHandle, float* pfCurvature) = 0;
        virtual vr::EVROverlayError SetOverlayTextureBounds(vr::VROverlayHandle_t ulOverlayHandle, const vr::VRTextureBounds_t* pOverlayTextureBounds) = 0;
        virtual vr::EVROverlayError GetOverlayTransformType(vr::VROverlayHandle_t ulOverlayHandle, vr::VROverlayTransformType* peTransformType) = 0;
        virtual vr::EVROverlayError SetOverlayTransformAbsolute(vr::VROverlayHandle_t ulOverlayHandle, vr::ETrackingUniverseOrigin eTrackingOrigin,
            const vr::HmdMatrix34_t* pmatTrackingOriginToOverlayTransform) = 0;
        virtual vr::EVROverlayError GetOverlayTransformAbsolute(vr::VROverlayHandle_t ulOverlayHandle, vr::ETrackingUniverseOrigin* peTrackingOrigin,
            vr::HmdMatrix34_t* pmatTrackingOriginToOverlayTransform) = 0;
        virtual vr::EVROverlayError GetOverlayTransformTrackedDeviceRelative(vr::VROverlayHandle_t ulOverlayHandle,
            vr::TrackedDeviceIndex_t* punTrackedDevice, vr::HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform) = 0;
        virtual vr::EVROverlayError ShowOverlay(vr::VROverlayHandle_t ulOverlayHandle) = 0;
        virtual vr::EVROverlayError HideOverlay(vr::VROverlayHandle_t ulOverlayHandle) = 0;
        virtual bool IsOverlayVisible(vr::VROverlayHandle_t ulOverlayHandle) = 0;
        virtual bool PollNextOverlayEvent(vr::VROverlayHandle_t ulOverlayHandle, vr::VREvent_t* pEvent, uint32_t uncbVREvent) = 0;
        virtual vr::EVROverlayError SetOverlayInputMethod(vr::VROverlayHandle_t ulOverlayHandle, vr::VROverlayInputMethod eInputMethod) = 0;
        virtual vr::EVROverlayError SetOverlayMouseScale(vr::VROverlayHandle_t ulOverlayHandle, const vr::HmdVector2_t* pvecMouseScale) = 0;
        virtual bool ComputeOverlayIntersection(vr::VROverlayHandle_t ulOverlayHandle, const vr::VROverlayIntersectionParams_t* pParams,
            vr::VROverlayIntersectionResults_t* pResults) = 0;
        virtual vr::EVROverlayError SetOverlayTexture(vr::VROverlayHandle_t ulOverlayHandle, const vr::Texture_t* pTexture) = 0;
        virtual vr::EVROverlayError SetOverlayRaw(vr::VROverlayHandle_t ulOverlayHandle, void* pvBuffer, uint32_t unWidth, uint32_t unHeight,
            uint32_t unBytesPerPixel) = 0;
        virtual vr::EVROverlayError ShowKeyboardForOverlay(vr::VROverlayHandle_t ulOverlayHandle, vr::EGamepadTextInputMode eInputMode,
            vr::EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char* pchDescription, uint32_t unCharMax,
            const char* pchExistingText, uint64_t uUserValue) = 0;
        virtual uint32_t GetKeyboardText(char* pchText, uint32_t cchText) = 0;
        virtual void HideKeyboard() = 0;
    };

    // IVRRenderModels subset: the controller tip component (vr_controller_tip.h).
    class IVrRenderModels
    {
    public:
        virtual ~IVrRenderModels() = default;
        virtual bool GetComponentStateForDevicePath(const char* pchRenderModelName, const char* pchComponentName,
            vr::VRInputValueHandle_t devicePath, const vr::RenderModel_ControllerMode_State_t* pState,
            vr::RenderModel_ComponentState_t* pComponentState) = 0;
    };

    // IVRApplications subset: registering manifest.vrmanifest.
    class IVrApplications
    {
    public:
        virtual ~IVrApplications() = default;
        virtual vr::EVRApplicationError AddApplicationManifest(const char* pchApplicationManifestFullPath, bool bTemporary = false) = 0;
    };

    // What the runtime can do natively. Features without a native path use the
    // mod's own fallbacks (manual laser pointer, in-mod keyboard, ...). Nothing
    // reads this yet in M1; the OpenVR backend reports everything available.
    struct Caps
    {
        bool nativeOverlayMouse = false;   // SetOverlayInputMethod(Mouse) + PollNextOverlayEvent mouse events
        bool overlayIntersection = false;  // ComputeOverlayIntersection
        bool vrKeyboard = false;           // ShowKeyboardForOverlay
        bool skeletalBones = false;        // GetSkeletalBoneData and friends
        bool runtimeAssets = false;        // GetRuntimePath (vr_glove meshes)
        uint32_t maxOverlayLayers = 0;     // 0 = no practical limit
    };

    class IVrBackend
    {
    public:
        virtual ~IVrBackend() = default;

        virtual RuntimeKind Kind() const = 0;
        virtual const char* Name() const = 0;
        virtual Caps GetCaps() const = 0;

        // Whether this runtime is installed at all (desktop auto-mode probe).
        virtual bool IsRuntimeInstalled() = 0;
        // Starts (or joins) the runtime session. On failure errorText is the
        // runtime's English description of the error.
        virtual bool Init(const char*& errorText) = 0;
        // Install path of the runtime (OpenVR: VR_GetRuntimePath). False when
        // the runtime has no such path.
        virtual bool GetRuntimePath(char* pchPathBuffer, uint32_t unBufferSize, uint32_t* punRequiredBufferSize) = 0;

        virtual IVrSystem* System() = 0;
        virtual IVrCompositor* Compositor() = 0;
        virtual IVrInput* Input() = 0;
        virtual IVrOverlay* Overlay() = 0;
        virtual IVrRenderModels* RenderModels() = 0;
        virtual IVrApplications* Applications() = 0;

        // Debug: saves the next left-eye image the runtime receives (after the
        // backend's own copy) as a BMP at `path`. False when unsupported
        // (OpenVR: the runtime owns that copy).
        virtual bool RequestSubmittedEyeDump(const char* path) { (void)path; return false; }

        // Test only (self-test step xrinput=): replaces the controller input the
        // runtime reports at the action-state boundary, so the backend's binding
        // table, thresholds, dpad and per-hand facade can be exercised without
        // hardware. Spec syntax: openxr_input_logic.h FakeInput. False with a
        // reason when unsupported (OpenVR) or the spec is invalid.
        virtual bool SetFakeInput(const char* spec, std::string& error) { (void)spec; error = "not supported by this runtime"; return false; }
        // Short description of each hand's controller ("left=wmr right=wmr"),
        // for logs and self-test reports.
        virtual std::string DescribeControllers() { return std::string(); }
    };

    // The OpenXR backend (vr_backend_openxr.cpp).
    IVrBackend& OpenXrBackendInstance();
    // Loads openxr_loader.dll and creates the OpenXR instance and HMD system
    // (once; the result is cached). False with a player-readable reason when
    // OpenXR is unusable. Runs only when OpenXR was explicitly selected.
    bool OpenXrProbeRuntime(std::string& reason);

    // The backend chosen for this process (L4D2VR_SelectedVrRuntime()). It is
    // created on first use and lives until the process exits.
    IVrBackend& Active();

    // Shorthands for the free-function style the mod used with OpenVR
    // (vr::VROverlay() -> l4d2vr_backend::Overlay()).
    inline IVrSystem* System() { return Active().System(); }
    inline IVrCompositor* Compositor() { return Active().Compositor(); }
    inline IVrInput* Input() { return Active().Input(); }
    inline IVrOverlay* Overlay() { return Active().Overlay(); }
    inline IVrRenderModels* RenderModels() { return Active().RenderModels(); }
    inline IVrApplications* Applications() { return Active().Applications(); }
    inline bool GetRuntimePath(char* pchPathBuffer, uint32_t unBufferSize, uint32_t* punRequiredBufferSize)
    {
        return Active().GetRuntimePath(pchPathBuffer, unBufferSize, punRequiredBufferSize);
    }
}

// What DXVK hands the OpenXR backend once its device exists (plain C so the
// DXVK fork needs no mod headers; d3d9_interface.cpp has a matching copy).
// The lock callbacks are DxvkDevice::lockSubmission/unlockSubmission, the gate
// every VkQueue user (and every xr call that may use the queue) goes through.
struct L4D2VR_VkDeviceInfo
{
    uint32_t size;                 // sizeof(L4D2VR_VkDeviceInfo)
    void* instance;                // VkInstance
    void* physicalDevice;          // VkPhysicalDevice
    void* device;                  // VkDevice
    void* queue;                   // VkQueue (DXVK's graphics queue)
    uint32_t queueFamilyIndex;
    uint32_t queueIndex;
    void* getInstanceProcAddr;     // PFN_vkGetInstanceProcAddr of DXVK's loader
    void (*lockQueue)(void* context);
    void (*unlockQueue)(void* context);
    void* queueContext;
};

// C entry points for the DXVK fork (d3d9_interface.cpp, dxvk_instance.cpp),
// which does not include the mod's headers.
extern "C"
{
    // 1 = OpenVR, 2 = OpenXR (l4d2vr_backend::RuntimeKind). Decided once, before
    // Direct3DCreate9 needs it; defined in dllmain.cpp.
    int L4D2VR_SelectedVrRuntime();
    // Starts the selected runtime for D3D9 device creation and reports the
    // recommended per-eye render size. On failure writes "VR_Init failed: ..."
    // into errorString and returns false.
    bool L4D2VR_VrBackendInitForDevice(uint32_t* renderWidth, uint32_t* renderHeight, char* errorString, uint32_t errorStringSize);

    // OpenXR only (DXVK's native OpenXR extension provider and adapter pin).
    // Space-separated Vulkan extension lists from xrGetVulkan*ExtensionsKHR;
    // `length` receives the size including the terminator. False when OpenXR
    // is unavailable or the buffer is too small.
    bool L4D2VR_XrGetVulkanInstanceExtensions(char* buffer, uint32_t capacity, uint32_t* length);
    bool L4D2VR_XrGetVulkanDeviceExtensions(char* buffer, uint32_t capacity, uint32_t* length);
    // xrGetVulkanGraphicsDeviceKHR for DXVK's VkInstance (void* = VkInstance,
    // VkPhysicalDevice*).
    bool L4D2VR_XrGetVulkanGraphicsDevice(void* vkInstance, void** physicalDevice);
    // DXVK created the D3D9 device: the OpenXR backend creates its session on it.
    // No-op for OpenVR.
    void L4D2VR_VrBackendDeviceCreated(const L4D2VR_VkDeviceInfo* info);
}

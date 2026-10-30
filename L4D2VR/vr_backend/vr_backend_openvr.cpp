// OpenVR backend: a pass-through to openvr_api with exactly the behaviour the
// mod had before the backend seam (OpenXR port M1).
//
// Each facade forwards to the OpenVR interface pointer it was last bound to.
// The accessors bind from the same vr::VRxxx() lookups the mod used to call
// directly and return nullptr whenever OpenVR returns nullptr, so null checks
// and the VR members' publication points behave as before.

#include "vr_backend.h"

#include <atomic>
#include <cstdio>

namespace l4d2vr_backend
{
namespace
{
    template<class Raw>
    class OpenVrBinding
    {
    public:
        // Binds a non-null interface and returns true; a null lookup leaves the
        // previous binding alone (OpenVR interfaces never change once valid).
        bool Bind(Raw* raw)
        {
            if (!raw)
                return false;
            m_Raw.store(raw, std::memory_order_relaxed);
            return true;
        }

    protected:
        Raw* R() const { return m_Raw.load(std::memory_order_relaxed); }

    private:
        std::atomic<Raw*> m_Raw{ nullptr };
    };

    class OpenVrSystem final : public IVrSystem, public OpenVrBinding<vr::IVRSystem>
    {
    public:
        void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) override
        { R()->GetRecommendedRenderTargetSize(pnWidth, pnHeight); }
        void GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) override
        { R()->GetProjectionRaw(eEye, pfLeft, pfRight, pfTop, pfBottom); }
        vr::HmdMatrix34_t GetEyeToHeadTransform(vr::EVREye eEye) override
        { return R()->GetEyeToHeadTransform(eEye); }
        void GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow,
            vr::TrackedDevicePose_t* pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount) override
        { R()->GetDeviceToAbsoluteTrackingPose(eOrigin, fPredictedSecondsToPhotonsFromNow, pTrackedDevicePoseArray, unTrackedDevicePoseArrayCount); }
        vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole unDeviceType) override
        { return R()->GetTrackedDeviceIndexForControllerRole(unDeviceType); }
        vr::ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t unDeviceIndex) override
        { return R()->GetTrackedDeviceClass(unDeviceIndex); }
        bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t unDeviceIndex) override
        { return R()->IsTrackedDeviceConnected(unDeviceIndex); }
        float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop,
            vr::ETrackedPropertyError* pError) override
        { return R()->GetFloatTrackedDeviceProperty(unDeviceIndex, prop, pError); }
        int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop,
            vr::ETrackedPropertyError* pError) override
        { return R()->GetInt32TrackedDeviceProperty(unDeviceIndex, prop, pError); }
        uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop,
            char* pchValue, uint32_t unBufferSize, vr::ETrackedPropertyError* pError) override
        { return R()->GetStringTrackedDeviceProperty(unDeviceIndex, prop, pchValue, unBufferSize, pError); }
        void TriggerHapticPulse(vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId, unsigned short usDurationMicroSec) override
        { R()->TriggerHapticPulse(unControllerDeviceIndex, unAxisId, usDurationMicroSec); }
    };

    class OpenVrCompositor final : public IVrCompositor, public OpenVrBinding<vr::IVRCompositor>
    {
    public:
        vr::ETrackingUniverseOrigin GetTrackingSpace() override
        { return R()->GetTrackingSpace(); }
        vr::EVRCompositorError WaitGetPoses(vr::TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount,
            vr::TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) override
        { return R()->WaitGetPoses(pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount); }
        vr::EVRCompositorError Submit(vr::EVREye eEye, const vr::Texture_t* pTexture, const vr::VRTextureBounds_t* pBounds,
            vr::EVRSubmitFlags nSubmitFlags) override
        { return R()->Submit(eEye, pTexture, pBounds, nSubmitFlags); }
        void ClearLastSubmittedFrame() override
        { R()->ClearLastSubmittedFrame(); }
        void PostPresentHandoff() override
        { R()->PostPresentHandoff(); }
        bool GetFrameTiming(vr::Compositor_FrameTiming* pTiming, uint32_t unFramesAgo) override
        { return R()->GetFrameTiming(pTiming, unFramesAgo); }
        float GetFrameTimeRemaining() override
        { return R()->GetFrameTimeRemaining(); }
        void SetExplicitTimingMode(vr::EVRCompositorTimingMode eTimingMode) override
        { R()->SetExplicitTimingMode(eTimingMode); }
        vr::EVRCompositorError SubmitExplicitTimingData() override
        { return R()->SubmitExplicitTimingData(); }
    };

    class OpenVrInput final : public IVrInput, public OpenVrBinding<vr::IVRInput>
    {
    public:
        vr::EVRInputError SetActionManifestPath(const char* pchActionManifestPath) override
        { return R()->SetActionManifestPath(pchActionManifestPath); }
        vr::EVRInputError GetActionSetHandle(const char* pchActionSetName, vr::VRActionSetHandle_t* pHandle) override
        { return R()->GetActionSetHandle(pchActionSetName, pHandle); }
        vr::EVRInputError GetActionHandle(const char* pchActionName, vr::VRActionHandle_t* pHandle) override
        { return R()->GetActionHandle(pchActionName, pHandle); }
        vr::EVRInputError GetInputSourceHandle(const char* pchInputSourcePath, vr::VRInputValueHandle_t* pHandle) override
        { return R()->GetInputSourceHandle(pchInputSourcePath, pHandle); }
        vr::EVRInputError UpdateActionState(vr::VRActiveActionSet_t* pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount) override
        { return R()->UpdateActionState(pSets, unSizeOfVRSelectedActionSet_t, unSetCount); }
        vr::EVRInputError GetDigitalActionData(vr::VRActionHandle_t action, vr::InputDigitalActionData_t* pActionData,
            uint32_t unActionDataSize, vr::VRInputValueHandle_t ulRestrictToDevice) override
        { return R()->GetDigitalActionData(action, pActionData, unActionDataSize, ulRestrictToDevice); }
        vr::EVRInputError GetAnalogActionData(vr::VRActionHandle_t action, vr::InputAnalogActionData_t* pActionData,
            uint32_t unActionDataSize, vr::VRInputValueHandle_t ulRestrictToDevice) override
        { return R()->GetAnalogActionData(action, pActionData, unActionDataSize, ulRestrictToDevice); }
        vr::EVRInputError GetSkeletalActionData(vr::VRActionHandle_t action, vr::InputSkeletalActionData_t* pActionData,
            uint32_t unActionDataSize) override
        { return R()->GetSkeletalActionData(action, pActionData, unActionDataSize); }
        vr::EVRInputError GetBoneCount(vr::VRActionHandle_t action, uint32_t* pBoneCount) override
        { return R()->GetBoneCount(action, pBoneCount); }
        vr::EVRInputError GetBoneHierarchy(vr::VRActionHandle_t action, vr::BoneIndex_t* pParentIndices, uint32_t unIndexArayCount) override
        { return R()->GetBoneHierarchy(action, pParentIndices, unIndexArayCount); }
        vr::EVRInputError GetBoneName(vr::VRActionHandle_t action, vr::BoneIndex_t nBoneIndex, char* pchBoneName, uint32_t unNameBufferSize) override
        { return R()->GetBoneName(action, nBoneIndex, pchBoneName, unNameBufferSize); }
        vr::EVRInputError GetSkeletalReferenceTransforms(vr::VRActionHandle_t action, vr::EVRSkeletalTransformSpace eTransformSpace,
            vr::EVRSkeletalReferencePose eReferencePose, vr::VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount) override
        { return R()->GetSkeletalReferenceTransforms(action, eTransformSpace, eReferencePose, pTransformArray, unTransformArrayCount); }
        vr::EVRInputError GetSkeletalBoneData(vr::VRActionHandle_t action, vr::EVRSkeletalTransformSpace eTransformSpace,
            vr::EVRSkeletalMotionRange eMotionRange, vr::VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount) override
        { return R()->GetSkeletalBoneData(action, eTransformSpace, eMotionRange, pTransformArray, unTransformArrayCount); }
        vr::EVRInputError GetSkeletalSummaryData(vr::VRActionHandle_t action, vr::EVRSummaryType eSummaryType,
            vr::VRSkeletalSummaryData_t* pSkeletalSummaryData) override
        { return R()->GetSkeletalSummaryData(action, eSummaryType, pSkeletalSummaryData); }
        vr::EVRInputError GetSkeletalTrackingLevel(vr::VRActionHandle_t action, vr::EVRSkeletalTrackingLevel* pSkeletalTrackingLevel) override
        { return R()->GetSkeletalTrackingLevel(action, pSkeletalTrackingLevel); }
        vr::EVRInputError GetOriginTrackedDeviceInfo(vr::VRInputValueHandle_t origin, vr::InputOriginInfo_t* pOriginInfo,
            uint32_t unOriginInfoSize) override
        { return R()->GetOriginTrackedDeviceInfo(origin, pOriginInfo, unOriginInfoSize); }
        vr::EVRInputError GetActionBindingInfo(vr::VRActionHandle_t action, vr::InputBindingInfo_t* pOriginInfo,
            uint32_t unBindingInfoSize, uint32_t unBindingInfoCount, uint32_t* punReturnedBindingInfoCount) override
        { return R()->GetActionBindingInfo(action, pOriginInfo, unBindingInfoSize, unBindingInfoCount, punReturnedBindingInfoCount); }
    };

    class OpenVrOverlay final : public IVrOverlay, public OpenVrBinding<vr::IVROverlay>
    {
    public:
        vr::EVROverlayError FindOverlay(const char* pchOverlayKey, vr::VROverlayHandle_t* pOverlayHandle) override
        { return R()->FindOverlay(pchOverlayKey, pOverlayHandle); }
        vr::EVROverlayError CreateOverlay(const char* pchOverlayKey, const char* pchOverlayName, vr::VROverlayHandle_t* pOverlayHandle) override
        { return R()->CreateOverlay(pchOverlayKey, pchOverlayName, pOverlayHandle); }
        vr::EVROverlayError DestroyOverlay(vr::VROverlayHandle_t ulOverlayHandle) override
        { return R()->DestroyOverlay(ulOverlayHandle); }
        vr::EVROverlayError SetOverlayFlag(vr::VROverlayHandle_t ulOverlayHandle, vr::VROverlayFlags eOverlayFlag, bool bEnabled) override
        { return R()->SetOverlayFlag(ulOverlayHandle, eOverlayFlag, bEnabled); }
        vr::EVROverlayError SetOverlayAlpha(vr::VROverlayHandle_t ulOverlayHandle, float fAlpha) override
        { return R()->SetOverlayAlpha(ulOverlayHandle, fAlpha); }
        vr::EVROverlayError SetOverlayTexelAspect(vr::VROverlayHandle_t ulOverlayHandle, float fTexelAspect) override
        { return R()->SetOverlayTexelAspect(ulOverlayHandle, fTexelAspect); }
        vr::EVROverlayError SetOverlaySortOrder(vr::VROverlayHandle_t ulOverlayHandle, uint32_t unSortOrder) override
        { return R()->SetOverlaySortOrder(ulOverlayHandle, unSortOrder); }
        vr::EVROverlayError SetOverlayWidthInMeters(vr::VROverlayHandle_t ulOverlayHandle, float fWidthInMeters) override
        { return R()->SetOverlayWidthInMeters(ulOverlayHandle, fWidthInMeters); }
        vr::EVROverlayError GetOverlayWidthInMeters(vr::VROverlayHandle_t ulOverlayHandle, float* pfWidthInMeters) override
        { return R()->GetOverlayWidthInMeters(ulOverlayHandle, pfWidthInMeters); }
        vr::EVROverlayError SetOverlayCurvature(vr::VROverlayHandle_t ulOverlayHandle, float fCurvature) override
        { return R()->SetOverlayCurvature(ulOverlayHandle, fCurvature); }
        vr::EVROverlayError GetOverlayCurvature(vr::VROverlayHandle_t ulOverlayHandle, float* pfCurvature) override
        { return R()->GetOverlayCurvature(ulOverlayHandle, pfCurvature); }
        vr::EVROverlayError SetOverlayTextureBounds(vr::VROverlayHandle_t ulOverlayHandle, const vr::VRTextureBounds_t* pOverlayTextureBounds) override
        { return R()->SetOverlayTextureBounds(ulOverlayHandle, pOverlayTextureBounds); }
        vr::EVROverlayError GetOverlayTransformType(vr::VROverlayHandle_t ulOverlayHandle, vr::VROverlayTransformType* peTransformType) override
        { return R()->GetOverlayTransformType(ulOverlayHandle, peTransformType); }
        vr::EVROverlayError SetOverlayTransformAbsolute(vr::VROverlayHandle_t ulOverlayHandle, vr::ETrackingUniverseOrigin eTrackingOrigin,
            const vr::HmdMatrix34_t* pmatTrackingOriginToOverlayTransform) override
        { return R()->SetOverlayTransformAbsolute(ulOverlayHandle, eTrackingOrigin, pmatTrackingOriginToOverlayTransform); }
        vr::EVROverlayError GetOverlayTransformAbsolute(vr::VROverlayHandle_t ulOverlayHandle, vr::ETrackingUniverseOrigin* peTrackingOrigin,
            vr::HmdMatrix34_t* pmatTrackingOriginToOverlayTransform) override
        { return R()->GetOverlayTransformAbsolute(ulOverlayHandle, peTrackingOrigin, pmatTrackingOriginToOverlayTransform); }
        vr::EVROverlayError GetOverlayTransformTrackedDeviceRelative(vr::VROverlayHandle_t ulOverlayHandle,
            vr::TrackedDeviceIndex_t* punTrackedDevice, vr::HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform) override
        { return R()->GetOverlayTransformTrackedDeviceRelative(ulOverlayHandle, punTrackedDevice, pmatTrackedDeviceToOverlayTransform); }
        vr::EVROverlayError ShowOverlay(vr::VROverlayHandle_t ulOverlayHandle) override
        { return R()->ShowOverlay(ulOverlayHandle); }
        vr::EVROverlayError HideOverlay(vr::VROverlayHandle_t ulOverlayHandle) override
        { return R()->HideOverlay(ulOverlayHandle); }
        bool IsOverlayVisible(vr::VROverlayHandle_t ulOverlayHandle) override
        { return R()->IsOverlayVisible(ulOverlayHandle); }
        bool PollNextOverlayEvent(vr::VROverlayHandle_t ulOverlayHandle, vr::VREvent_t* pEvent, uint32_t uncbVREvent) override
        { return R()->PollNextOverlayEvent(ulOverlayHandle, pEvent, uncbVREvent); }
        vr::EVROverlayError SetOverlayInputMethod(vr::VROverlayHandle_t ulOverlayHandle, vr::VROverlayInputMethod eInputMethod) override
        { return R()->SetOverlayInputMethod(ulOverlayHandle, eInputMethod); }
        vr::EVROverlayError SetOverlayMouseScale(vr::VROverlayHandle_t ulOverlayHandle, const vr::HmdVector2_t* pvecMouseScale) override
        { return R()->SetOverlayMouseScale(ulOverlayHandle, pvecMouseScale); }
        bool ComputeOverlayIntersection(vr::VROverlayHandle_t ulOverlayHandle, const vr::VROverlayIntersectionParams_t* pParams,
            vr::VROverlayIntersectionResults_t* pResults) override
        { return R()->ComputeOverlayIntersection(ulOverlayHandle, pParams, pResults); }
        vr::EVROverlayError SetOverlayTexture(vr::VROverlayHandle_t ulOverlayHandle, const vr::Texture_t* pTexture) override
        { return R()->SetOverlayTexture(ulOverlayHandle, pTexture); }
        vr::EVROverlayError SetOverlayRaw(vr::VROverlayHandle_t ulOverlayHandle, void* pvBuffer, uint32_t unWidth, uint32_t unHeight,
            uint32_t unBytesPerPixel) override
        { return R()->SetOverlayRaw(ulOverlayHandle, pvBuffer, unWidth, unHeight, unBytesPerPixel); }
        vr::EVROverlayError ShowKeyboardForOverlay(vr::VROverlayHandle_t ulOverlayHandle, vr::EGamepadTextInputMode eInputMode,
            vr::EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char* pchDescription, uint32_t unCharMax,
            const char* pchExistingText, uint64_t uUserValue) override
        { return R()->ShowKeyboardForOverlay(ulOverlayHandle, eInputMode, eLineInputMode, unFlags, pchDescription, unCharMax, pchExistingText, uUserValue); }
        uint32_t GetKeyboardText(char* pchText, uint32_t cchText) override
        { return R()->GetKeyboardText(pchText, cchText); }
        void HideKeyboard() override
        { R()->HideKeyboard(); }
    };

    class OpenVrRenderModels final : public IVrRenderModels, public OpenVrBinding<vr::IVRRenderModels>
    {
    public:
        bool GetComponentStateForDevicePath(const char* pchRenderModelName, const char* pchComponentName,
            vr::VRInputValueHandle_t devicePath, const vr::RenderModel_ControllerMode_State_t* pState,
            vr::RenderModel_ComponentState_t* pComponentState) override
        { return R()->GetComponentStateForDevicePath(pchRenderModelName, pchComponentName, devicePath, pState, pComponentState); }
    };

    class OpenVrApplications final : public IVrApplications, public OpenVrBinding<vr::IVRApplications>
    {
    public:
        vr::EVRApplicationError AddApplicationManifest(const char* pchApplicationManifestFullPath, bool bTemporary) override
        { return R()->AddApplicationManifest(pchApplicationManifestFullPath, bTemporary); }
    };

    template<class Facade, class Raw>
    Facade* BindOrNull(Facade& facade, Raw* raw)
    {
        return facade.Bind(raw) ? &facade : nullptr;
    }

    class OpenVrBackend final : public IVrBackend
    {
    public:
        RuntimeKind Kind() const override { return RuntimeKind::OpenVR; }
        const char* Name() const override { return "OpenVR"; }

        Caps GetCaps() const override
        {
            Caps caps;
            caps.nativeOverlayMouse = true;
            caps.overlayIntersection = true;
            caps.vrKeyboard = true;
            caps.skeletalBones = true;
            caps.runtimeAssets = true;
            caps.maxOverlayLayers = vr::k_unMaxOverlayCount;
            return caps;
        }

        bool IsRuntimeInstalled() override
        {
            return vr::VR_IsRuntimeInstalled();
        }

        bool Init(const char*& errorText) override
        {
            errorText = nullptr;
            vr::HmdError error = vr::VRInitError_None;
            vr::IVRSystem* system = vr::VR_Init(&error, vr::VRApplication_Scene);
            if (error != vr::VRInitError_None)
            {
                errorText = vr::VR_GetVRInitErrorAsEnglishDescription(error);
                return false;
            }
            m_System.Bind(system);
            return true;
        }

        bool GetRuntimePath(char* pchPathBuffer, uint32_t unBufferSize, uint32_t* punRequiredBufferSize) override
        {
            return vr::VR_GetRuntimePath(pchPathBuffer, unBufferSize, punRequiredBufferSize);
        }

        IVrSystem* System() override { return BindOrNull(m_System, vr::VRSystem()); }
        IVrCompositor* Compositor() override { return BindOrNull(m_Compositor, vr::VRCompositor()); }
        IVrInput* Input() override { return BindOrNull(m_Input, vr::VRInput()); }
        IVrOverlay* Overlay() override { return BindOrNull(m_Overlay, vr::VROverlay()); }
        IVrRenderModels* RenderModels() override { return BindOrNull(m_RenderModels, vr::VRRenderModels()); }
        IVrApplications* Applications() override { return BindOrNull(m_Applications, vr::VRApplications()); }

    private:
        OpenVrSystem m_System;
        OpenVrCompositor m_Compositor;
        OpenVrInput m_Input;
        OpenVrOverlay m_Overlay;
        OpenVrRenderModels m_RenderModels;
        OpenVrApplications m_Applications;
    };

    IVrBackend& CreateBackend(int selected)
    {
        if (selected == static_cast<int>(RuntimeKind::OpenXR))
            return OpenXrBackendInstance();
        static OpenVrBackend openVr;
        return openVr;
    }
}

IVrBackend& Active()
{
    static IVrBackend& backend = CreateBackend(L4D2VR_SelectedVrRuntime());
    return backend;
}
}

extern "C" bool L4D2VR_VrBackendInitForDevice(uint32_t* renderWidth, uint32_t* renderHeight, char* errorString, uint32_t errorStringSize)
{
    l4d2vr_backend::IVrBackend& backend = l4d2vr_backend::Active();
    const char* initError = nullptr;
    if (!backend.Init(initError))
    {
        if (errorString && errorStringSize)
            std::snprintf(errorString, errorStringSize, "VR_Init failed: %s", initError ? initError : "");
        return false;
    }
    l4d2vr_backend::IVrSystem* system = backend.System();
    if (!system)
    {
        if (errorString && errorStringSize)
            std::snprintf(errorString, errorStringSize, "VR_Init failed: %s runtime has no system interface", backend.Name());
        return false;
    }
    system->GetRecommendedRenderTargetSize(renderWidth, renderHeight);
    return true;
}

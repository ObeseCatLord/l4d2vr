#pragma once

#include "openvr.h"
#include "../vr_backend/vr_backend.h"
#include "vr_hand_types.h"
#include "../vr_input_recovery.h"

#include <memory>
#include <string>
#include <vector>

class VrHandSkeletonRuntime
{
public:
    VrHandSkeletonRuntime();
    ~VrHandSkeletonRuntime();

    VrHandSkeletonRuntime(const VrHandSkeletonRuntime&) = delete;
    VrHandSkeletonRuntime& operator=(const VrHandSkeletonRuntime&) = delete;

    bool Initialize(l4d2vr_backend::IVrInput* input, vr::VRActionHandle_t action, std::string& outError);
    // Curls only (a runtime without bone data, or a self-test curl override):
    // the glove palette comes from the skeletal summary, as it does under
    // SteamVR whenever the summary is valid.
    void InitializeSummaryOnly(vr::VRActionHandle_t action);
    bool Update(l4d2vr_backend::IVrInput* input, const l4d2vr_input::ActionFrame& actionFrame,
        l4d2vr_backend::IVrSystem* system, vr::VRInputValueHandle_t expectedSource,
        vr::TrackedDeviceIndex_t expectedDevice,
        vr::EVRSkeletalMotionRange motionRange, std::string& outError);
    // A pose from given curls (self-test fingercurls=), without runtime reads.
    bool UpdateFromCurls(const l4d2vr_input::ActionFrame& actionFrame, const float curls[5]);
    bool BuildSkinningPalette(
        const VrHandMeshAsset& asset,
        std::vector<VrHandMatrixRows3x4>& outPalette,
        std::string& outError,
        const VrHandFingerCurlOverride* fingerCurlOverride = nullptr,
        const std::array<float, 5>* fingerMaxCurl = nullptr) const;

    bool IsInitialized() const;
    bool IsSummaryOnly() const;
    bool HasPose() const;
    std::uint64_t PoseFrameToken() const;
    int JointCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

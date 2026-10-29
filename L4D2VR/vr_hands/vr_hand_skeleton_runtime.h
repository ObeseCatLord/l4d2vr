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
    bool Update(l4d2vr_backend::IVrInput* input, const l4d2vr_input::ActionFrame& actionFrame,
        l4d2vr_backend::IVrSystem* system, vr::VRInputValueHandle_t expectedSource,
        vr::TrackedDeviceIndex_t expectedDevice,
        vr::EVRSkeletalMotionRange motionRange, std::string& outError);
    bool BuildSkinningPalette(
        const VrHandMeshAsset& asset,
        std::vector<VrHandMatrixRows3x4>& outPalette,
        std::string& outError,
        const VrHandFingerCurlOverride* fingerCurlOverride = nullptr,
        const std::array<float, 5>* fingerMaxCurl = nullptr) const;

    bool IsInitialized() const;
    bool HasPose() const;
    std::uint64_t PoseFrameToken() const;
    int JointCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

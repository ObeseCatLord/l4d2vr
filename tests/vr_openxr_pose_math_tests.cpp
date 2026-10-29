// OpenXR <-> OpenVR value conversions (L4D2VR/vr_backend/openxr_pose_math.h).
#include "../L4D2VR/vr_backend/openxr_pose_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int checks = 0;
static void Check(bool passed, const char* contract)
{
    ++checks;
    if (!passed)
    {
        std::fprintf(stderr, "FAIL: %s\n", contract);
        std::exit(1);
    }
}

static bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

namespace xrm = l4d2vr_openxr;

static xrm::Quat AxisAngle(float x, float y, float z, float radians)
{
    const float s = std::sin(radians * 0.5f);
    return xrm::Quat{ x * s, y * s, z * s, std::cos(radians * 0.5f) };
}

int main()
{
    // Identity and translation.
    {
        xrm::Pose p;
        p.position = { 0.1f, 1.7f, -0.3f };
        const vr::HmdMatrix34_t m = xrm::ToMatrix(p);
        Check(Near(m.m[0][0], 1) && Near(m.m[1][1], 1) && Near(m.m[2][2], 1) && Near(m.m[0][1], 0), "identity rotation");
        Check(Near(m.m[0][3], 0.1f) && Near(m.m[1][3], 1.7f) && Near(m.m[2][3], -0.3f), "translation in column 3");
    }

    // A yaw of +90 degrees about +Y turns -Z (forward) into -X.
    {
        xrm::Pose p;
        p.orientation = AxisAngle(0, 1, 0, 3.14159265f * 0.5f);
        const vr::HmdMatrix34_t m = xrm::ToMatrix(p);
        // forward = R * (0,0,-1) = -(third column)
        Check(Near(-m.m[0][2], -1.0f) && Near(-m.m[1][2], 0.0f) && Near(-m.m[2][2], 0.0f, 1e-5f), "yaw +90 looks along -X");
    }

    // Round trips through the matrix keep the pose (several orientations).
    {
        const xrm::Quat qs[] = {
            AxisAngle(1, 0, 0, 0.7f), AxisAngle(0, 0, 1, -2.9f), AxisAngle(0.267f, 0.535f, 0.802f, 2.2f),
            AxisAngle(0, 1, 0, 3.14159f), AxisAngle(1, 0, 0, 3.1f) };
        for (const xrm::Quat& q0 : qs)
        {
            xrm::Pose p;
            p.orientation = xrm::Normalized(q0);
            p.position = { -0.5f, 1.2f, 2.0f };
            const xrm::Pose back = xrm::FromMatrix(xrm::ToMatrix(p));
            const float dot = std::fabs(back.orientation.x * p.orientation.x + back.orientation.y * p.orientation.y +
                back.orientation.z * p.orientation.z + back.orientation.w * p.orientation.w);
            Check(Near(dot, 1.0f, 1e-4f), "matrix -> quaternion round trip keeps the rotation");
            Check(Near(back.position.x, -0.5f) && Near(back.position.y, 1.2f) && Near(back.position.z, 2.0f), "round trip keeps the position");
            Check(back.orientation.w >= 0.0f, "quaternion is canonical (w >= 0)");
        }
    }

    // Eye pose = head * eye offset (translation only, like the mod renders).
    {
        xrm::Pose head;
        head.orientation = AxisAngle(0, 1, 0, 3.14159265f * 0.5f);  // facing -X
        head.position = { 1.0f, 1.6f, 0.0f };
        xrm::Pose eyeOffset;
        eyeOffset.orientation = AxisAngle(0, 1, 0, 0.1f);  // a canted display
        eyeOffset.position = { 0.032f, 0.0f, 0.0f };        // right eye
        const xrm::Pose eye = xrm::EyePoseFromHead(xrm::ToMatrix(head), xrm::ToMatrix(eyeOffset), true);
        // Head-right (+X local) maps to world -Z... for yaw +90: local +X -> world -Z.
        Check(Near(eye.position.x, 1.0f, 1e-5f) && Near(eye.position.y, 1.6f) && Near(eye.position.z, -0.032f, 1e-5f),
            "right eye sits to the head's right");
        const float dot = std::fabs(eye.orientation.x * head.orientation.x + eye.orientation.y * head.orientation.y +
            eye.orientation.z * head.orientation.z + eye.orientation.w * head.orientation.w);
        Check(Near(dot, 1.0f, 1e-4f), "translation-only eye keeps the head orientation (cant ignored like the renderer)");
        const xrm::Pose canted = xrm::EyePoseFromHead(xrm::ToMatrix(head), xrm::ToMatrix(eyeOffset), false);
        const float cdot = std::fabs(canted.orientation.x * head.orientation.x + canted.orientation.y * head.orientation.y +
            canted.orientation.z * head.orientation.z + canted.orientation.w * head.orientation.w);
        Check(cdot < 0.9999f, "full eye offset applies the cant");
    }

    // Raw projection <-> FOV follows XRizer's convention (bottom = tan(up)).
    {
        const xrm::Fov fov{ -0.9f, 0.8f, 0.85f, -0.95f };
        const xrm::RawProjection raw = xrm::ToRawProjection(fov);
        Check(Near(raw.left, std::tan(-0.9f)) && Near(raw.right, std::tan(0.8f)), "left/right are tan(angleLeft/Right)");
        Check(Near(raw.bottom, std::tan(0.85f)) && Near(raw.top, std::tan(-0.95f)), "bottom = tan(angleUp), top = tan(angleDown)");
        const xrm::Fov back = xrm::FromRawProjection(raw);
        Check(Near(back.angleLeft, fov.angleLeft) && Near(back.angleRight, fov.angleRight) &&
            Near(back.angleUp, fov.angleUp) && Near(back.angleDown, fov.angleDown), "raw projection round trip");
        // The mod's texture bounds from that raw projection (vr_lifecycle_init.inl):
        const float tanX = std::fmax(-raw.left, raw.right);
        const float tanY = std::fmax(-raw.top, raw.bottom);
        const float vMin = 0.5f - 0.5f * raw.bottom / tanY;
        const float vMax = 0.5f - 0.5f * raw.top / tanY;
        Check(vMin >= -1e-6f && vMin < vMax && vMax <= 1.0f + 1e-6f, "mod bounds are top-down and inside the image");
        Check(Near(0.5f + 0.5f * raw.left / tanX, 0.0f), "the wider side spans to the image edge");
        Check(xrm::FovUsable(fov) && !xrm::FovUsable(xrm::Fov{}), "usable FOV check");
    }

    // Image rect from OpenVR bounds.
    {
        vr::VRTextureBounds_t b{ 0.0f, 0.1f, 1.0f, 0.9f };
        bool flipped = true;
        xrm::Rect r = xrm::ImageRectFromBounds(&b, 1000, 800, &flipped);
        Check(!flipped && r.x == 0 && r.y == 80 && r.width == 1000 && r.height == 640, "bounds select the sub-rect");
        r = xrm::ImageRectFromBounds(nullptr, 640, 720);
        Check(r.x == 0 && r.y == 0 && r.width == 640 && r.height == 720, "no bounds = whole image");
        vr::VRTextureBounds_t f{ 0.0f, 1.0f, 1.0f, 0.0f };
        r = xrm::ImageRectFromBounds(&f, 100, 100, &flipped);
        Check(flipped && r.width == 100 && r.height == 100, "a flipped axis is reported and used unflipped");
        vr::VRTextureBounds_t odd{ -0.2f, 0.5f, 1.4f, 0.5f };
        r = xrm::ImageRectFromBounds(&odd, 100, 100);
        Check(r.x == 0 && r.width == 100 && r.height >= 1 && r.y + r.height <= 100, "bounds are clamped and never empty");
    }

    // Tracked pose flags.
    {
        xrm::Pose p;
        p.position = { 0, 1.5f, 0 };
        vr::TrackedDevicePose_t t = xrm::ToTrackedPose(p, 0xF, { 1, 0, 0 }, { 0, 2, 0 }, true);
        Check(t.bPoseIsValid && t.bDeviceIsConnected && t.eTrackingResult == vr::TrackingResult_Running_OK, "tracked pose is valid");
        Check(Near(t.mDeviceToAbsoluteTracking.m[1][3], 1.5f) && Near(t.vVelocity.v[0], 1.0f) && Near(t.vAngularVelocity.v[1], 2.0f),
            "pose and velocities copied");
        t = xrm::ToTrackedPose(p, 0x3, {}, {}, false);
        Check(t.bPoseIsValid && t.eTrackingResult == vr::TrackingResult_Fallback_RotationOnly, "valid but untracked pose");
        t = xrm::ToTrackedPose(p, 0x1, {}, {}, false);
        Check(!t.bPoseIsValid && t.eTrackingResult == vr::TrackingResult_Running_OutOfRange, "orientation only is not a valid pose");
    }

    // Display rate.
    Check(Near(xrm::DisplayHzFromPeriod(16666667, 90.0f), 60.0f, 0.01f), "60 Hz period");
    Check(Near(xrm::DisplayHzFromPeriod(11111111, 0.0f), 90.0f, 0.01f), "90 Hz period");
    Check(Near(xrm::DisplayHzFromPeriod(0, 90.0f), 90.0f), "no period uses the fallback");

    std::printf("vr_openxr_pose_math: %d checks passed\n", checks);
    return 0;
}

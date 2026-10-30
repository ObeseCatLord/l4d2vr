#pragma once

// App-side input logic for the OpenXR backend (OpenXR port M3). Std-only and
// runtime-free so it is unit-tested (tests/vr_openxr_input_tests.cpp):
//   * click thresholds with hysteresis (OpenXR bindings carry no parameters),
//   * radial stick deadzone, the menu dpad derived from a stick,
//   * the grip pose -> OpenVR raw pose transform (with velocity),
//   * the test-only fake action source's spec parser.

#include "openxr_pose_math.h"
#include "openxr_input_profiles.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>

namespace l4d2vr_openxr_input
{
    namespace xrm = l4d2vr_openxr;

    // ---- Thresholds -------------------------------------------------------------
    // Press at >= press; while down, stay down until the value drops below
    // release (SteamVR click_activate/deactivate_threshold semantics).
    inline bool NextClick(bool wasDown, float value, float press, float release)
    {
        if (!std::isfinite(value))
            return false;
        return wasDown ? value >= release : value >= press;
    }

    // ---- Sticks -----------------------------------------------------------------
    struct Stick2 { float x = 0.0f, y = 0.0f; };

    // Radial deadzone with the remaining range rescaled to 0..1, so the output
    // grows continuously from the deadzone edge. Inputs outside the unit circle
    // are clamped to it.
    inline Stick2 RadialDeadzone(Stick2 in, float deadzone)
    {
        if (!std::isfinite(in.x) || !std::isfinite(in.y))
            return {};
        const float r = std::sqrt(in.x * in.x + in.y * in.y);
        if (r <= deadzone || r <= 0.0f)
            return {};
        const float clamped = (std::min)(r, 1.0f);
        const float scaled = (deadzone < 1.0f) ? (clamped - deadzone) / (1.0f - deadzone) : 0.0f;
        return Stick2{ in.x / r * scaled, in.y / r * scaled };
    }

    // Which 90 degree wedge a stick points into (no overlap, like SteamVR's
    // overlap_pct 0 and XRizer's dpad).
    inline DpadDir Wedge(Stick2 s)
    {
        const float ax = std::fabs(s.x), ay = std::fabs(s.y);
        if (ax == 0.0f && ay == 0.0f)
            return DpadDir::None;
        if (ay >= ax)
            return s.y >= 0.0f ? DpadDir::North : DpadDir::South;
        return s.x >= 0.0f ? DpadDir::East : DpadDir::West;
    }

    // One dpad direction: down while the stick is touched (or the controller
    // has no touch sensor), inside the direction's wedge and beyond the press
    // radius; once down it stays down until the radius falls below release or
    // the stick leaves the wedge.
    inline bool NextDpad(bool wasDown, DpadDir dir, Stick2 s, bool touched, float press = kDpadPress, float release = kDpadRelease)
    {
        if (!touched || dir == DpadDir::None || !std::isfinite(s.x) || !std::isfinite(s.y))
            return false;
        const float r = std::sqrt(s.x * s.x + s.y * s.y);
        if (Wedge(s) != dir)
            return false;
        return wasDown ? r >= release : r >= press;
    }

    // ---- Grip -> raw pose ---------------------------------------------------------
    inline xrm::Quat AxisAngle(float ax, float ay, float az, float radians)
    {
        const float s = std::sin(radians * 0.5f);
        return xrm::Quat{ ax * s, ay * s, az * s, std::cos(radians * 0.5f) };
    }

    inline xrm::Quat Mul(const xrm::Quat& a, const xrm::Quat& b)
    {
        return xrm::Quat{
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    }

    inline xrm::Quat Conjugate(const xrm::Quat& q) { return xrm::Quat{ -q.x, -q.y, -q.z, q.w }; }

    inline xrm::Vec3 Rotate(const xrm::Quat& q, const xrm::Vec3& v)
    {
        // v' = q v q*
        const xrm::Quat p{ v.x, v.y, v.z, 0.0f };
        const xrm::Quat r = Mul(Mul(q, p), Conjugate(q));
        return xrm::Vec3{ r.x, r.y, r.z };
    }

    inline xrm::Vec3 Cross(const xrm::Vec3& a, const xrm::Vec3& b)
    {
        return xrm::Vec3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    inline xrm::Quat EulerToQuat(const float deg[3], EulerOrder order)
    {
        constexpr float kDegToRad = 3.14159265358979f / 180.0f;
        const xrm::Quat qx = AxisAngle(1.0f, 0.0f, 0.0f, deg[0] * kDegToRad);
        const xrm::Quat qy = AxisAngle(0.0f, 1.0f, 0.0f, deg[1] * kDegToRad);
        const xrm::Quat qz = AxisAngle(0.0f, 0.0f, 1.0f, deg[2] * kDegToRad);
        return order == EulerOrder::XYZ ? Mul(Mul(qx, qy), qz) : Mul(Mul(qz, qy), qx);
    }

    // The grip pose in the raw frame as a pose (grip = raw * this).
    inline xrm::Pose GripInRawPose(const GripInRaw& g)
    {
        xrm::Pose p;
        p.orientation = xrm::Normalized(EulerToQuat(g.eulerDeg, g.order));
        p.position = xrm::Vec3{ g.position[0], g.position[1], g.position[2] };
        return p;
    }

    inline xrm::Pose Compose(const xrm::Pose& a, const xrm::Pose& b)
    {
        xrm::Pose r;
        r.orientation = xrm::Normalized(Mul(a.orientation, b.orientation));
        const xrm::Vec3 t = Rotate(a.orientation, b.position);
        r.position = xrm::Vec3{ a.position.x + t.x, a.position.y + t.y, a.position.z + t.z };
        return r;
    }

    inline xrm::Pose Inverse(const xrm::Pose& a)
    {
        xrm::Pose r;
        r.orientation = Conjugate(xrm::Normalized(a.orientation));
        const xrm::Vec3 t = Rotate(r.orientation, a.position);
        r.position = xrm::Vec3{ -t.x, -t.y, -t.z };
        return r;
    }

    // raw = grip * inverse(gripInRaw).
    inline xrm::Pose RawFromGrip(const xrm::Pose& grip, const GripInRaw& offset)
    {
        return Compose(grip, Inverse(GripInRawPose(offset)));
    }

    // Velocity of the raw origin from the grip's (both in the base space):
    // v_raw = v_grip + w x (p_raw - p_grip); the angular velocity is shared.
    inline xrm::Vec3 RawLinearVelocity(const xrm::Vec3& gripLinear, const xrm::Vec3& angular,
        const xrm::Pose& grip, const xrm::Pose& raw)
    {
        const xrm::Vec3 r{ raw.position.x - grip.position.x, raw.position.y - grip.position.y, raw.position.z - grip.position.z };
        const xrm::Vec3 c = Cross(angular, r);
        return xrm::Vec3{ gripLinear.x + c.x, gripLinear.y + c.y, gripLinear.z + c.z };
    }

    // ---- Fake action source (self-test / unit test only) ------------------------
    // Replaces what xrGetActionState* returns, keyed by full input path. Specs:
    //   "profile=<key>"                       interpret bindings as this profile (index, touch, ...)
    //   "/user/hand/right/input/squeeze/value=0.9"
    //   "/user/hand/left/input/thumbstick=0:0.8"   (x:y)
    //   "clear"                               leave fake mode
    struct FakeInput
    {
        bool enabled = false;
        const Profile* profile = nullptr;  // nullptr = the runtime's current profile
        std::map<std::string, Stick2> values;  // scalars use x

        bool Apply(const std::string& spec, std::string& error)
        {
            error.clear();
            if (spec == "clear" || spec == "off")
            {
                enabled = false;
                profile = nullptr;
                values.clear();
                return true;
            }
            const size_t eq = spec.find('=');
            if (eq == std::string::npos || eq == 0)
            {
                error = "expected key=value";
                return false;
            }
            const std::string key = spec.substr(0, eq), value = spec.substr(eq + 1);
            if (key == "profile")
            {
                if (value == "runtime")
                {
                    profile = nullptr;
                    enabled = true;
                    return true;
                }
                const Profile* p = FindProfileByKey(value.c_str());
                if (!p)
                {
                    error = "unknown profile " + value;
                    return false;
                }
                profile = p;
                enabled = true;
                return true;
            }
            if (key.rfind("/user/hand/left/", 0) != 0 && key.rfind("/user/hand/right/", 0) != 0)
            {
                error = "path must start with /user/hand/left/ or /user/hand/right/";
                return false;
            }
            Stick2 v;
            char* end = nullptr;
            v.x = std::strtof(value.c_str(), &end);
            if (end == value.c_str())
            {
                error = "bad number " + value;
                return false;
            }
            if (end && *end == ':')
            {
                const char* ys = end + 1;
                v.y = std::strtof(ys, &end);
                if (end == ys)
                {
                    error = "bad y in " + value;
                    return false;
                }
            }
            values[key] = v;
            enabled = true;
            return true;
        }

        Stick2 Get(const std::string& path) const
        {
            auto it = values.find(path);
            return it == values.end() ? Stick2{} : it->second;
        }
    };
}

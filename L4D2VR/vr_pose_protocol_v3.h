#pragma once

// VR pose relay protocol version 3.
//
// Version 3 sits alongside version 2 (vr_pose_protocol.h, which is unchanged
// and stays byte-identical). It carries enough of the sender's tracking for a
// receiver to solve a full body locally (see vr_vrik_solver.h): finger pose
// kinds, trigger curl, crouch ratio and calibrated standing height, the body
// yaw relative to the player's view yaw, the eye offset with the full head
// basis, and both hand frames as compact quaternions.
//
// Reference frame ("view frame"): origin at the player origin (the feet), axes
// aligned with the player's view yaw and Source model axes: +X forward, +Y
// left, +Z up. Positions are game units * 8 (the v2 step of 0.125 units).
// Orientations map local Source axes (forward, left, up) into the view frame.
//
// Wire layout (72 bytes, explicit little-endian packing, no struct punning):
//   off size field
//    0   1   magic 0xB7 (same as v2)
//    1   1   version << 4 (0x30); low nibble reserved, must be zero
//    2   2   u16 sequence
//    4   1   u8 flags (kFlag*)
//    5   7   left hand fingers: u8 kind, u8 trigger curl, u8 curls[5]
//   12   7   right hand fingers
//   19   1   u8 crouch ratio: (HMD height / standing height) * 255 / 1.5
//   20   2   u16 calibrated standing eye height in millimetres (0 = unknown)
//   22   2   s16 body yaw relative to the view yaw, degrees * 100
//   24   6   s16[3] eye offset, view frame, game units * 8
//   30   6   s16[3] head forward, unit * 32767
//   36   6   s16[3] head up, unit * 32767
//   42  14   left hand: s16[3] position (units * 8), s16[4] quaternion
//            (x, y, z, w) * 32767, canonical w >= 0
//   56  14   right hand
//   70   2   u16 CRC-16/CCITT over bytes 0..69 (same CRC as v2)
//
// Text encoding: the v2 client-command alphabet (A-Z a-z 0-9 - _), MSB-first
// 6-bit groups; 72 bytes encode to exactly 96 characters.
//
// Strictness: unknown flag bits, out-of-range enums, the value -32768 in any
// signed field, non-zero data in sections whose valid flag is clear, finger
// data with kind None, non-unit head axes / quaternions, non-canonical
// quaternion sign and CRC mismatches are all rejected.

#include "vr_pose_protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace l4d2vr_pose_v3
{
    constexpr std::uint8_t kMagic = l4d2vr_pose::kMagic;
    constexpr std::uint8_t kVersion = 3u;
    constexpr std::uint8_t kVersionByte = static_cast<std::uint8_t>(kVersion << 4);

    constexpr std::uint8_t kFlagHeadValid = 1u << 0;
    constexpr std::uint8_t kFlagLeftHandValid = 1u << 1;
    constexpr std::uint8_t kFlagRightHandValid = 1u << 2;
    constexpr std::uint8_t kFlagWeaponInLeftHand = 1u << 3;   // akimbo / left-handed weapon
    constexpr std::uint8_t kFlagMeleeInertialPose = 1u << 4;
    constexpr std::uint8_t kFlagHolsterGrabActive = 1u << 5;
    constexpr std::uint8_t kFlagMask =
        kFlagHeadValid | kFlagLeftHandValid | kFlagRightHandValid |
        kFlagWeaponInLeftHand | kFlagMeleeInertialPose | kFlagHolsterGrabActive;

    enum class FingerPose : std::uint8_t
    {
        None = 0,      // no finger data (receiver uses native animation)
        Relaxed,
        Trigger,       // weapon grip with the index finger on the trigger
        Point,
        Fist,
        Hold,          // holding an object (curls describe the grip)
        Support,       // off hand supporting a two-handed weapon
        Count
    };

    constexpr int kLeft = 0;
    constexpr int kRight = 1;

    constexpr float kPositionStepsPerUnit = 8.0f;          // == 1 / l4d2vr_pose::kPositionUnitsPerStep
    constexpr float kUnitScale = 32767.0f;
    constexpr float kBodyYawStepsPerDegree = 100.0f;
    constexpr std::int16_t kBodyYawLimit = 18000;
    constexpr float kCrouchRatioMaximum = 1.5f;
    constexpr float kFingerCurlMaximum = l4d2vr_pose::kFingerCurlMaximum;   // same quantisation as v2
    constexpr std::uint16_t kStandingHeightMinMm = 500u;
    constexpr std::uint16_t kStandingHeightMaxMm = 3000u;

    // Receive-store policy.
    constexpr double kStaleSeconds = 0.5;
    constexpr double kRestartSilenceSeconds = 1.0;

    struct HandFingers
    {
        FingerPose kind = FingerPose::None;
        std::uint8_t trigger = 0u;          // 0..255 -> 0..1
        std::uint8_t curls[5]{};            // thumb..little, 0..255 -> 0..kFingerCurlMaximum
    };

    struct HandFrame
    {
        std::int16_t position[3]{};         // view frame, units * 8
        std::int16_t rotation[4]{};         // quaternion x, y, z, w * 32767
    };

    struct PacketV3
    {
        std::uint16_t sequence = 0u;
        std::uint8_t flags = 0u;
        HandFingers fingers[2]{};           // [kLeft], [kRight]
        std::uint8_t crouchRatio = 0u;
        std::uint16_t standingHeightMm = 0u;
        std::int16_t bodyYaw = 0;           // degrees * 100, relative to view yaw
        std::int16_t eyeOffset[3]{};
        std::int16_t headForward[3]{};
        std::int16_t headUp[3]{};
        HandFrame hands[2]{};               // [kLeft], [kRight]
    };

    constexpr std::size_t kWireBytes = 72u;
    constexpr std::size_t kCrcOffset = 70u;
    constexpr std::size_t kEncodedChars = (kWireBytes * 8u + 5u) / 6u;
    static_assert(kEncodedChars == 96u, "Unexpected protocol 3 payload length.");
    static_assert(kEncodedChars != l4d2vr_pose::kEncodedPayloadChars,
        "Protocol 2 and 3 payloads must be distinguishable by length.");

    using WireBytes = std::array<std::uint8_t, kWireBytes>;

    // ---- Quantisation helpers ------------------------------------------------

    inline std::int16_t ClampToS16(float scaled)
    {
        if (!std::isfinite(scaled))
            return 0;
        return static_cast<std::int16_t>(std::lround(std::clamp(scaled, -32767.0f, 32767.0f)));
    }

    inline std::int16_t PackPosition(float units) { return ClampToS16(units * kPositionStepsPerUnit); }
    inline float UnpackPosition(std::int16_t value) { return static_cast<float>(value) / kPositionStepsPerUnit; }

    inline std::int16_t PackUnit(float value) { return ClampToS16(value * kUnitScale); }
    inline float UnpackUnit(std::int16_t value) { return static_cast<float>(value) / kUnitScale; }

    inline float NormalizeDegrees(float degrees)
    {
        if (!std::isfinite(degrees))
            return 0.0f;
        return degrees - 360.0f * std::floor((degrees + 180.0f) / 360.0f);
    }

    inline std::int16_t PackBodyYaw(float relativeDegrees)
    {
        const long value = std::lround(NormalizeDegrees(relativeDegrees) * kBodyYawStepsPerDegree);
        return static_cast<std::int16_t>(std::clamp<long>(value, -kBodyYawLimit, kBodyYawLimit));
    }
    inline float UnpackBodyYaw(std::int16_t value) { return static_cast<float>(value) / kBodyYawStepsPerDegree; }

    inline std::uint8_t PackCrouchRatio(float hmdHeight, float standingHeight)
    {
        if (!std::isfinite(hmdHeight) || !std::isfinite(standingHeight) || standingHeight <= 0.0f)
            return 0u;
        const float ratio = std::clamp(hmdHeight / standingHeight, 0.0f, kCrouchRatioMaximum);
        return static_cast<std::uint8_t>(std::lround(ratio * 255.0f / kCrouchRatioMaximum));
    }
    inline float UnpackCrouchRatio(std::uint8_t value) { return static_cast<float>(value) * kCrouchRatioMaximum / 255.0f; }

    // 0 (unknown) for non-finite or out-of-range heights.
    inline std::uint16_t PackStandingHeightMm(float metres)
    {
        if (!std::isfinite(metres))
            return 0u;
        const long mm = std::lround(metres * 1000.0f);
        if (mm < kStandingHeightMinMm || mm > kStandingHeightMaxMm)
            return 0u;
        return static_cast<std::uint16_t>(mm);
    }

    inline std::uint8_t PackFingerCurl(float curl)
    {
        if (!std::isfinite(curl))
            return 0u;
        return static_cast<std::uint8_t>(std::lround(std::clamp(curl / kFingerCurlMaximum, 0.0f, 1.0f) * 255.0f));
    }
    inline float UnpackFingerCurl(std::uint8_t value) { return static_cast<float>(value) * (kFingerCurlMaximum / 255.0f); }

    inline std::uint8_t PackTrigger(float curl)
    {
        if (!std::isfinite(curl))
            return 0u;
        return static_cast<std::uint8_t>(std::lround(std::clamp(curl, 0.0f, 1.0f) * 255.0f));
    }
    inline float UnpackTrigger(std::uint8_t value) { return static_cast<float>(value) / 255.0f; }

    // Normalised, canonical (w >= 0) quaternion. Returns false for a degenerate input.
    inline bool PackQuaternion(float x, float y, float z, float w, std::int16_t out[4])
    {
        const float length = std::sqrt(x * x + y * y + z * z + w * w);
        if (!std::isfinite(length) || length < 1e-6f)
        {
            out[0] = out[1] = out[2] = out[3] = 0;
            return false;
        }
        const float sign = w < 0.0f ? -1.0f : 1.0f;
        const float k = sign / length;
        out[0] = PackUnit(x * k);
        out[1] = PackUnit(y * k);
        out[2] = PackUnit(z * k);
        out[3] = PackUnit(w * k);
        return true;
    }

    inline void UnpackQuaternion(const std::int16_t in[4], float& x, float& y, float& z, float& w)
    {
        x = UnpackUnit(in[0]); y = UnpackUnit(in[1]); z = UnpackUnit(in[2]); w = UnpackUnit(in[3]);
        const float length = std::sqrt(x * x + y * y + z * z + w * w);
        if (length > 1e-6f) { x /= length; y /= length; z /= length; w /= length; }
        else { x = y = z = 0.0f; w = 1.0f; }
    }

    // ---- Source angle conventions (pitch, yaw, roll in degrees) ---------------
    // Matrices are column-major axes: m[row][col], column 0 forward, 1 left, 2 up.

    inline void AngleMatrix(float pitch, float yaw, float roll, float m[3][3])
    {
        constexpr float kDeg = 3.14159265358979323846f / 180.0f;
        const float sp = std::sin(pitch * kDeg), cp = std::cos(pitch * kDeg);
        const float sy = std::sin(yaw * kDeg), cy = std::cos(yaw * kDeg);
        const float sr = std::sin(roll * kDeg), cr = std::cos(roll * kDeg);
        m[0][0] = cp * cy; m[1][0] = cp * sy; m[2][0] = -sp;
        m[0][1] = sp * sr * cy - cr * sy; m[1][1] = sp * sr * sy + cr * cy; m[2][1] = sr * cp;
        m[0][2] = sp * cr * cy + sr * sy; m[1][2] = sp * cr * sy - sr * cy; m[2][2] = cr * cp;
    }

    inline void MatrixAngles(const float m[3][3], float& pitch, float& yaw, float& roll)
    {
        constexpr float kRad = 180.0f / 3.14159265358979323846f;
        const float xyDist = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0]);
        if (xyDist > 0.001f)
        {
            yaw = std::atan2(m[1][0], m[0][0]) * kRad;
            pitch = std::atan2(-m[2][0], xyDist) * kRad;
            roll = std::atan2(m[2][1], m[2][2]) * kRad;
        }
        else
        {
            yaw = std::atan2(-m[0][1], m[1][1]) * kRad;
            pitch = std::atan2(-m[2][0], xyDist) * kRad;
            roll = 0.0f;
        }
    }

    inline void QuaternionMatrix(float x, float y, float z, float w, float m[3][3])
    {
        m[0][0] = 1.0f - 2.0f * (y * y + z * z); m[0][1] = 2.0f * (x * y - w * z); m[0][2] = 2.0f * (x * z + w * y);
        m[1][0] = 2.0f * (x * y + w * z); m[1][1] = 1.0f - 2.0f * (x * x + z * z); m[1][2] = 2.0f * (y * z - w * x);
        m[2][0] = 2.0f * (x * z - w * y); m[2][1] = 2.0f * (y * z + w * x); m[2][2] = 1.0f - 2.0f * (x * x + y * y);
    }

    inline void MatrixQuaternion(const float m[3][3], float& x, float& y, float& z, float& w)
    {
        const float trace = m[0][0] + m[1][1] + m[2][2];
        if (trace > 0.0f)
        {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            w = 0.25f * s; x = (m[2][1] - m[1][2]) / s; y = (m[0][2] - m[2][0]) / s; z = (m[1][0] - m[0][1]) / s;
        }
        else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
        {
            const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
            w = (m[2][1] - m[1][2]) / s; x = 0.25f * s; y = (m[0][1] + m[1][0]) / s; z = (m[0][2] + m[2][0]) / s;
        }
        else if (m[1][1] > m[2][2])
        {
            const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
            w = (m[0][2] - m[2][0]) / s; x = (m[0][1] + m[1][0]) / s; y = 0.25f * s; z = (m[1][2] + m[2][1]) / s;
        }
        else
        {
            const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
            w = (m[1][0] - m[0][1]) / s; x = (m[0][2] + m[2][0]) / s; y = (m[1][2] + m[2][1]) / s; z = 0.25f * s;
        }
    }

    // Packs a Source QAngle-style orientation (view-frame relative) as a hand quaternion.
    inline bool PackAnglesAsQuaternion(float pitch, float yaw, float roll, std::int16_t out[4])
    {
        float m[3][3];
        AngleMatrix(pitch, yaw, roll, m);
        float x, y, z, w;
        MatrixQuaternion(m, x, y, z, w);
        return PackQuaternion(x, y, z, w, out);
    }

    // ---- Validation -----------------------------------------------------------

    namespace detail
    {
        inline bool AllZero(const std::int16_t* values, int count)
        {
            for (int i = 0; i < count; ++i)
                if (values[i] != 0) return false;
            return true;
        }

        inline bool NoMinimum(const std::int16_t* values, int count)
        {
            for (int i = 0; i < count; ++i)
                if (values[i] == -32768) return false;
            return true;
        }

        inline std::int64_t Dot3(const std::int16_t* a, const std::int16_t* b)
        {
            return std::int64_t(a[0]) * b[0] + std::int64_t(a[1]) * b[1] + std::int64_t(a[2]) * b[2];
        }

        // Within roughly +-5% of unit length (squared bounds on the *32767 scale).
        inline bool UnitLength(std::int64_t lengthSquared)
        {
            return lengthSquared >= std::int64_t(31000) * 31000 && lengthSquared <= std::int64_t(34400) * 34400;
        }

        inline bool FingersEmpty(const HandFingers& fingers)
        {
            if (fingers.trigger != 0u) return false;
            for (std::uint8_t curl : fingers.curls)
                if (curl != 0u) return false;
            return true;
        }

        inline std::uint8_t HandValidFlag(int side) { return side == kLeft ? kFlagLeftHandValid : kFlagRightHandValid; }

        inline void PutU8(std::uint8_t*& at, std::uint8_t v) { *at++ = v; }
        inline void PutU16(std::uint8_t*& at, std::uint16_t v)
        {
            *at++ = static_cast<std::uint8_t>(v & 0xFFu);
            *at++ = static_cast<std::uint8_t>(v >> 8);
        }
        inline void PutS16(std::uint8_t*& at, std::int16_t v) { PutU16(at, static_cast<std::uint16_t>(v)); }
        inline std::uint8_t GetU8(const std::uint8_t*& at) { return *at++; }
        inline std::uint16_t GetU16(const std::uint8_t*& at)
        {
            const std::uint16_t v = static_cast<std::uint16_t>(at[0] | (at[1] << 8));
            at += 2;
            return v;
        }
        inline std::int16_t GetS16(const std::uint8_t*& at) { return static_cast<std::int16_t>(GetU16(at)); }
    }

    // Semantic validation of a logical packet (everything except magic/version/CRC).
    inline bool ValidateFields(const PacketV3& packet)
    {
        if ((packet.flags & ~kFlagMask) != 0u)
            return false;
        if (packet.bodyYaw < -kBodyYawLimit || packet.bodyYaw > kBodyYawLimit)
            return false;
        if (packet.standingHeightMm != 0u &&
            (packet.standingHeightMm < kStandingHeightMinMm || packet.standingHeightMm > kStandingHeightMaxMm))
            return false;
        if (!detail::NoMinimum(packet.eyeOffset, 3) || !detail::NoMinimum(packet.headForward, 3) ||
            !detail::NoMinimum(packet.headUp, 3))
            return false;

        if ((packet.flags & kFlagHeadValid) != 0u)
        {
            if (!detail::UnitLength(detail::Dot3(packet.headForward, packet.headForward)) ||
                !detail::UnitLength(detail::Dot3(packet.headUp, packet.headUp)))
                return false;
            // Orthogonal to within about 3 degrees.
            const std::int64_t dot = detail::Dot3(packet.headForward, packet.headUp);
            const std::int64_t limit = std::int64_t(1700) * 32767;
            if (dot > limit || dot < -limit)
                return false;
        }
        else if (!detail::AllZero(packet.eyeOffset, 3) || !detail::AllZero(packet.headForward, 3) ||
                 !detail::AllZero(packet.headUp, 3) || packet.crouchRatio != 0u)
        {
            return false;
        }

        for (int side = 0; side < 2; ++side)
        {
            const HandFingers& fingers = packet.fingers[side];
            const HandFrame& hand = packet.hands[side];
            if (static_cast<std::uint8_t>(fingers.kind) >= static_cast<std::uint8_t>(FingerPose::Count))
                return false;
            if (fingers.kind == FingerPose::None && !detail::FingersEmpty(fingers))
                return false;
            if (!detail::NoMinimum(hand.position, 3) || !detail::NoMinimum(hand.rotation, 4))
                return false;
            if ((packet.flags & detail::HandValidFlag(side)) != 0u)
            {
                const std::int64_t lengthSquared =
                    detail::Dot3(hand.rotation, hand.rotation) + std::int64_t(hand.rotation[3]) * hand.rotation[3];
                if (!detail::UnitLength(lengthSquared) || hand.rotation[3] < 0)
                    return false;
            }
            else if (!detail::AllZero(hand.position, 3) || !detail::AllZero(hand.rotation, 4) ||
                     fingers.kind != FingerPose::None || !detail::FingersEmpty(fingers))
            {
                return false;
            }
        }
        return true;
    }

    // Clears every section whose valid flag is not set, unknown flags, and
    // finger values of kind None, so a sender's packet passes ValidateFields.
    inline void Normalize(PacketV3& packet)
    {
        packet.flags = static_cast<std::uint8_t>(packet.flags & kFlagMask);
        packet.bodyYaw = static_cast<std::int16_t>(std::clamp<int>(packet.bodyYaw, -kBodyYawLimit, kBodyYawLimit));
        if (packet.standingHeightMm != 0u &&
            (packet.standingHeightMm < kStandingHeightMinMm || packet.standingHeightMm > kStandingHeightMaxMm))
            packet.standingHeightMm = 0u;
        if ((packet.flags & kFlagHeadValid) == 0u)
        {
            std::memset(packet.eyeOffset, 0, sizeof(packet.eyeOffset));
            std::memset(packet.headForward, 0, sizeof(packet.headForward));
            std::memset(packet.headUp, 0, sizeof(packet.headUp));
            packet.crouchRatio = 0u;
        }
        auto fixMinimum = [](std::int16_t* values, int count)
        {
            for (int i = 0; i < count; ++i)
                if (values[i] == -32768) values[i] = -32767;
        };
        fixMinimum(packet.eyeOffset, 3);
        fixMinimum(packet.headForward, 3);
        fixMinimum(packet.headUp, 3);
        for (int side = 0; side < 2; ++side)
        {
            HandFingers& fingers = packet.fingers[side];
            HandFrame& hand = packet.hands[side];
            if (static_cast<std::uint8_t>(fingers.kind) >= static_cast<std::uint8_t>(FingerPose::Count))
                fingers.kind = FingerPose::None;
            if ((packet.flags & detail::HandValidFlag(side)) == 0u)
            {
                hand = HandFrame{};
                fingers = HandFingers{};
            }
            if (fingers.kind == FingerPose::None)
                fingers = HandFingers{};
            fixMinimum(hand.position, 3);
            fixMinimum(hand.rotation, 4);
            if (hand.rotation[3] < 0)
                for (std::int16_t& value : hand.rotation)
                    value = static_cast<std::int16_t>(-value);
        }
    }

    // ---- Binary codec ---------------------------------------------------------

    // Fails (leaving `out` unspecified) when the packet does not pass ValidateFields.
    inline bool Encode(const PacketV3& packet, WireBytes& out)
    {
        if (!ValidateFields(packet))
            return false;
        std::uint8_t* at = out.data();
        detail::PutU8(at, kMagic);
        detail::PutU8(at, kVersionByte);
        detail::PutU16(at, packet.sequence);
        detail::PutU8(at, packet.flags);
        for (const HandFingers& fingers : packet.fingers)
        {
            detail::PutU8(at, static_cast<std::uint8_t>(fingers.kind));
            detail::PutU8(at, fingers.trigger);
            for (std::uint8_t curl : fingers.curls)
                detail::PutU8(at, curl);
        }
        detail::PutU8(at, packet.crouchRatio);
        detail::PutU16(at, packet.standingHeightMm);
        detail::PutS16(at, packet.bodyYaw);
        for (std::int16_t v : packet.eyeOffset) detail::PutS16(at, v);
        for (std::int16_t v : packet.headForward) detail::PutS16(at, v);
        for (std::int16_t v : packet.headUp) detail::PutS16(at, v);
        for (const HandFrame& hand : packet.hands)
        {
            for (std::int16_t v : hand.position) detail::PutS16(at, v);
            for (std::int16_t v : hand.rotation) detail::PutS16(at, v);
        }
        if (static_cast<std::size_t>(at - out.data()) != kCrcOffset)
            return false;
        detail::PutU16(at, l4d2vr_pose::Crc16Ccitt(out.data(), kCrcOffset));
        return true;
    }

    inline bool Decode(const std::uint8_t* bytes, std::size_t size, PacketV3& out)
    {
        if (!bytes || size != kWireBytes || bytes[0] != kMagic || bytes[1] != kVersionByte)
            return false;
        const std::uint16_t crc = static_cast<std::uint16_t>(bytes[kCrcOffset] | (bytes[kCrcOffset + 1] << 8));
        if (crc != l4d2vr_pose::Crc16Ccitt(bytes, kCrcOffset))
            return false;

        PacketV3 packet{};
        const std::uint8_t* at = bytes + 2;
        packet.sequence = detail::GetU16(at);
        packet.flags = detail::GetU8(at);
        for (HandFingers& fingers : packet.fingers)
        {
            const std::uint8_t kind = detail::GetU8(at);
            if (kind >= static_cast<std::uint8_t>(FingerPose::Count))
                return false;
            fingers.kind = static_cast<FingerPose>(kind);
            fingers.trigger = detail::GetU8(at);
            for (std::uint8_t& curl : fingers.curls)
                curl = detail::GetU8(at);
        }
        packet.crouchRatio = detail::GetU8(at);
        packet.standingHeightMm = detail::GetU16(at);
        packet.bodyYaw = detail::GetS16(at);
        for (std::int16_t& v : packet.eyeOffset) v = detail::GetS16(at);
        for (std::int16_t& v : packet.headForward) v = detail::GetS16(at);
        for (std::int16_t& v : packet.headUp) v = detail::GetS16(at);
        for (HandFrame& hand : packet.hands)
        {
            for (std::int16_t& v : hand.position) v = detail::GetS16(at);
            for (std::int16_t& v : hand.rotation) v = detail::GetS16(at);
        }
        if (static_cast<std::size_t>(at - bytes) != kCrcOffset || !ValidateFields(packet))
            return false;
        out = packet;
        return true;
    }

    // ---- Text codec (v2 client-command alphabet) -------------------------------

    inline void EncodeBytesToText(const std::uint8_t* bytes, std::size_t size, std::string& out)
    {
        static constexpr char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789-_";
        out.clear();
        out.reserve((size * 8u + 5u) / 6u);
        std::uint32_t accumulator = 0u;
        int bits = 0;
        for (std::size_t i = 0; i < size; ++i)
        {
            accumulator = ((accumulator << 8) | bytes[i]) & 0xFFFFu;
            bits += 8;
            while (bits >= 6)
            {
                bits -= 6;
                out.push_back(kAlphabet[(accumulator >> bits) & 0x3Fu]);
            }
        }
        if (bits > 0)
            out.push_back(kAlphabet[(accumulator << (6 - bits)) & 0x3Fu]);
    }

    // Strict: exact length, alphabet only, zero padding bits.
    inline bool DecodeTextToBytes(const char* text, std::uint8_t* bytes, std::size_t size)
    {
        const std::size_t chars = (size * 8u + 5u) / 6u;
        if (!text || std::strlen(text) != chars)
            return false;
        std::size_t written = 0;
        std::uint32_t accumulator = 0u;
        int bits = 0;
        for (std::size_t i = 0; i < chars; ++i)
        {
            const int value = l4d2vr_pose::DecodePayloadCharacter(static_cast<unsigned char>(text[i]));
            if (value < 0)
                return false;
            accumulator = ((accumulator << 6) | static_cast<std::uint32_t>(value)) & 0xFFFFu;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                if (written >= size)
                    return false;
                bytes[written++] = static_cast<std::uint8_t>((accumulator >> bits) & 0xFFu);
            }
        }
        if (written != size || (bits > 0 && (accumulator & ((1u << bits) - 1u)) != 0u))
            return false;
        return true;
    }

    inline bool EncodeText(const PacketV3& packet, std::string& out)
    {
        WireBytes bytes{};
        if (!Encode(packet, bytes))
        {
            out.clear();
            return false;
        }
        EncodeBytesToText(bytes.data(), bytes.size(), out);
        return out.size() == kEncodedChars;
    }

    inline bool DecodeText(const char* text, PacketV3& out)
    {
        WireBytes bytes{};
        return DecodeTextToBytes(text, bytes.data(), bytes.size()) && Decode(bytes.data(), bytes.size(), out);
    }

    // 2 or 3 from the encoded length (the two payloads never share a length), 0 otherwise.
    inline int PayloadVersionForText(const char* text)
    {
        if (!text)
            return 0;
        const std::size_t length = std::strlen(text);
        if (length == l4d2vr_pose::kEncodedPayloadChars) return 2;
        if (length == kEncodedChars) return 3;
        return 0;
    }

    // ---- Sequencing and receive store -----------------------------------------

    // 16-bit wrap-safe "a is newer than b".
    inline bool SequenceIsNewer(std::uint16_t a, std::uint16_t b)
    {
        return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b)) > 0;
    }

    // Per-player receive slot: keeps the newest sequence; a stream that was
    // silent for kRestartSilenceSeconds accepts any sequence (sender restart);
    // samples older than kStaleSeconds are not returned.
    struct ReceiveStore
    {
        PacketV3 packet{};
        double receivedAt = 0.0;
        bool valid = false;

        bool Offer(const PacketV3& candidate, double now)
        {
            if (!std::isfinite(now))
                return false;
            if (valid && now - receivedAt < kRestartSilenceSeconds &&
                !SequenceIsNewer(candidate.sequence, packet.sequence))
                return false;
            packet = candidate;
            receivedAt = now;
            valid = true;
            return true;
        }

        bool IsFresh(double now) const
        {
            return valid && std::isfinite(now) && now >= receivedAt && now - receivedAt <= kStaleSeconds;
        }

        const PacketV3* Latest(double now) const { return IsFresh(now) ? &packet : nullptr; }

        void Reset() { *this = ReceiveStore{}; }
    };

    // ---- Downconversion for protocol-2 peers ------------------------------------

    namespace detail
    {
        inline std::int16_t PackV2Angle(float degrees)
        {
            return static_cast<std::int16_t>(std::lround(std::clamp(
                NormalizeDegrees(degrees) * (32767.0f / 180.0f), -32767.0f, 32767.0f)));
        }

        inline std::int16_t RequantizeV2Position(float units)
        {
            return ClampToS16(units / l4d2vr_pose::kPositionUnitsPerStep);
        }

        // View-frame position (forward, left, up) -> v2 body-local (forward, right, up).
        inline void ToV2Position(const std::int16_t in[3], float cosRel, float sinRel, std::int16_t out[3])
        {
            const float px = UnpackPosition(in[0]), py = UnpackPosition(in[1]), pz = UnpackPosition(in[2]);
            const float bodyForward = cosRel * px + sinRel * py;
            const float bodyLeft = -sinRel * px + cosRel * py;
            out[0] = RequantizeV2Position(bodyForward);
            out[1] = RequantizeV2Position(-bodyLeft);
            out[2] = RequantizeV2Position(pz);
        }

        // View-frame rotation matrix -> v2 body-relative angles (yaw relative to the body).
        inline void ToV2Angles(const float view[3][3], float cosRel, float sinRel, std::int16_t out[3])
        {
            float body[3][3];
            for (int col = 0; col < 3; ++col)
            {
                body[0][col] = cosRel * view[0][col] + sinRel * view[1][col];
                body[1][col] = -sinRel * view[0][col] + cosRel * view[1][col];
                body[2][col] = view[2][col];
            }
            float pitch, yaw, roll;
            MatrixAngles(body, pitch, yaw, roll);
            out[0] = PackV2Angle(pitch);
            out[1] = PackV2Angle(yaw);
            out[2] = PackV2Angle(roll);
        }

        inline bool NativeFingerKind(FingerPose kind)
        {
            return kind == FingerPose::None || kind == FingerPose::Trigger || kind == FingerPose::Support;
        }
    }

    // Produces a valid protocol-2 packet for peers without v3 support.
    // `playerViewYawDegrees` is the sender's view yaw (v2 carries the absolute
    // body yaw). Finger mapping: None/Trigger/Support hands use native
    // animation; Relaxed/Point/Fist/Hold send their curls (same quantisation);
    // a Support hand on either side is a two-handed grip (both native); two
    // curl-driven hands are "empty hands". Trigger curl, crouch, standing
    // height and the v3-only flags have no v2 representation and are dropped.
    inline bool DownconvertToV2(const PacketV3& in, float playerViewYawDegrees, l4d2vr_pose::WirePacket& out)
    {
        out = l4d2vr_pose::WirePacket{};
        if (!ValidateFields(in) || !std::isfinite(playerViewYawDegrees))
            return false;

        constexpr float kDeg = 3.14159265358979323846f / 180.0f;
        const float relative = UnpackBodyYaw(in.bodyYaw);
        const float cosRel = std::cos(relative * kDeg), sinRel = std::sin(relative * kDeg);

        std::uint8_t validMask = 0u;
        if ((in.flags & kFlagHeadValid) != 0u) validMask |= l4d2vr_pose::kValidHmd;
        if ((in.flags & kFlagLeftHandValid) != 0u) validMask |= l4d2vr_pose::kValidLeftHand;
        if ((in.flags & kFlagRightHandValid) != 0u) validMask |= l4d2vr_pose::kValidRightHand;
        out.versionAndValidMask = static_cast<std::uint8_t>((l4d2vr_pose::kVersion << 4) | validMask);
        out.featureMask = l4d2vr_pose::kFeatureBodyYaw;
        out.bodyYaw = detail::PackV2Angle(playerViewYawDegrees + relative);

        if ((in.flags & kFlagHeadValid) != 0u)
        {
            detail::ToV2Position(in.eyeOffset, cosRel, sinRel, out.hmd.position);
            float f[3], u[3];
            for (int i = 0; i < 3; ++i) { f[i] = UnpackUnit(in.headForward[i]); u[i] = UnpackUnit(in.headUp[i]); }
            const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            for (float& v : f) v /= fl;
            const float fu = f[0] * u[0] + f[1] * u[1] + f[2] * u[2];
            for (int i = 0; i < 3; ++i) u[i] -= f[i] * fu;   // Gram-Schmidt
            const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            for (float& v : u) v /= ul;
            const float l[3] = { u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0] };
            float view[3][3];
            for (int r = 0; r < 3; ++r) { view[r][0] = f[r]; view[r][1] = l[r]; view[r][2] = u[r]; }
            detail::ToV2Angles(view, cosRel, sinRel, out.hmd.rotation);
        }

        l4d2vr_pose::PackedTrackedPose* v2Hands[2] = { &out.leftHand, &out.rightHand };
        for (int side = 0; side < 2; ++side)
        {
            if ((in.flags & detail::HandValidFlag(side)) == 0u)
                continue;
            detail::ToV2Position(in.hands[side].position, cosRel, sinRel, v2Hands[side]->position);
            float x, y, z, w, view[3][3];
            UnpackQuaternion(in.hands[side].rotation, x, y, z, w);
            QuaternionMatrix(x, y, z, w, view);
            detail::ToV2Angles(view, cosRel, sinRel, v2Hands[side]->rotation);
        }

        const bool leftValid = (in.flags & kFlagLeftHandValid) != 0u;
        const bool rightValid = (in.flags & kFlagRightHandValid) != 0u;
        const FingerPose leftKind = in.fingers[kLeft].kind, rightKind = in.fingers[kRight].kind;
        const bool twoHanded = leftKind == FingerPose::Support || rightKind == FingerPose::Support;
        const bool leftNative = twoHanded || !leftValid || detail::NativeFingerKind(leftKind);
        const bool rightNative = twoHanded || !rightValid || detail::NativeFingerKind(rightKind);

        std::uint8_t handState = 0u;
        if (leftNative) handState |= l4d2vr_pose::kHandStateLeftNativeFingerAnimation;
        if (rightNative) handState |= l4d2vr_pose::kHandStateRightNativeFingerAnimation;
        if (twoHanded) handState |= l4d2vr_pose::kHandStateTwoHandedGrip;
        if (!leftNative && !rightNative) handState |= l4d2vr_pose::kHandStateEmptyHands;
        out.handStateFlags = handState;

        if (!leftNative)
        {
            out.featureMask |= l4d2vr_pose::kFeatureLeftFingerCurls;
            std::memcpy(out.leftFingerCurls, in.fingers[kLeft].curls, sizeof(out.leftFingerCurls));
        }
        if (!rightNative)
        {
            out.featureMask |= l4d2vr_pose::kFeatureRightFingerCurls;
            std::memcpy(out.rightFingerCurls, in.fingers[kRight].curls, sizeof(out.rightFingerCurls));
        }

        l4d2vr_pose::FinalizePacket(out);
        return l4d2vr_pose::ValidatePacket(out);
    }
}

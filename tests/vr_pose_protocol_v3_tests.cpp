#include "../L4D2VR/vr_pose_protocol_v3.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    int checks = 0;
    void Require(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            std::fprintf(stderr, "FAILED: %s\n", message);
            std::exit(1);
        }
    }

    bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

    float AngleNear(float a, float b, float tolerance)
    {
        return std::fabs(l4d2vr_pose_v3::NormalizeDegrees(a - b)) <= tolerance;
    }

    float V2Position(std::int16_t value) { return static_cast<float>(value) * l4d2vr_pose::kPositionUnitsPerStep; }
    float V2Angle(std::int16_t value) { return static_cast<float>(value) * (180.0f / 32767.0f); }

    l4d2vr_pose_v3::PacketV3 SamplePacket()
    {
        using namespace l4d2vr_pose_v3;
        PacketV3 packet{};
        packet.sequence = 4321u;
        packet.flags = kFlagHeadValid | kFlagLeftHandValid | kFlagRightHandValid | kFlagHolsterGrabActive;
        packet.fingers[kLeft].kind = FingerPose::Fist;
        packet.fingers[kLeft].trigger = 12u;
        for (int i = 0; i < 5; ++i)
            packet.fingers[kLeft].curls[i] = static_cast<std::uint8_t>(40 + i * 30);
        packet.fingers[kRight].kind = FingerPose::Trigger;
        packet.fingers[kRight].trigger = 200u;
        packet.fingers[kRight].curls[1] = 90u;
        packet.crouchRatio = PackCrouchRatio(52.0f, 64.0f);
        packet.standingHeightMm = PackStandingHeightMm(1.65f);
        packet.bodyYaw = PackBodyYaw(-25.5f);
        packet.eyeOffset[0] = PackPosition(2.5f);
        packet.eyeOffset[1] = PackPosition(-1.25f);
        packet.eyeOffset[2] = PackPosition(52.0f);
        // Level gaze 30 degrees left of the view yaw.
        packet.headForward[0] = PackUnit(std::cos(30.0f * 3.14159265f / 180.0f));
        packet.headForward[1] = PackUnit(std::sin(30.0f * 3.14159265f / 180.0f));
        packet.headUp[2] = PackUnit(1.0f);
        packet.hands[kLeft].position[0] = PackPosition(14.0f);
        packet.hands[kLeft].position[1] = PackPosition(9.0f);
        packet.hands[kLeft].position[2] = PackPosition(40.0f);
        PackAnglesAsQuaternion(10.0f, 20.0f, -5.0f, packet.hands[kLeft].rotation);
        packet.hands[kRight].position[0] = PackPosition(18.0f);
        packet.hands[kRight].position[1] = PackPosition(-8.0f);
        packet.hands[kRight].position[2] = PackPosition(44.0f);
        PackAnglesAsQuaternion(-15.0f, -10.0f, 30.0f, packet.hands[kRight].rotation);
        return packet;
    }

    bool SamePacket(const l4d2vr_pose_v3::PacketV3& a, const l4d2vr_pose_v3::PacketV3& b)
    {
        l4d2vr_pose_v3::WireBytes wa{}, wb{};
        return l4d2vr_pose_v3::Encode(a, wa) && l4d2vr_pose_v3::Encode(b, wb) && wa == wb;
    }

    void TestRoundTrip()
    {
        using namespace l4d2vr_pose_v3;
        const PacketV3 packet = SamplePacket();
        Require(ValidateFields(packet), "Sample packet is valid");

        WireBytes bytes{};
        Require(Encode(packet, bytes), "Binary encode succeeds");
        Require(bytes[0] == 0xB7u && bytes[1] == 0x30u, "Magic and version byte lead the packet");
        Require(bytes[2] == (4321u & 0xFFu) && bytes[3] == (4321u >> 8), "Sequence is little-endian");
        PacketV3 decoded{};
        Require(Decode(bytes.data(), bytes.size(), decoded), "Binary decode succeeds");
        Require(SamePacket(packet, decoded), "Binary round trip is lossless");
        Require(decoded.fingers[kLeft].kind == FingerPose::Fist && decoded.fingers[kRight].trigger == 200u,
            "Finger data survives the round trip");

        std::string text;
        Require(EncodeText(packet, text) && text.size() == kEncodedChars, "Text encode produces 96 characters");
        for (char c : text)
            Require(l4d2vr_pose::DecodePayloadCharacter(static_cast<unsigned char>(c)) >= 0,
                "Text uses the v2 client-command alphabet");
        PacketV3 fromText{};
        Require(DecodeText(text.c_str(), fromText) && SamePacket(packet, fromText), "Text round trip is lossless");
        Require(PayloadVersionForText(text.c_str()) == 3, "Payload length identifies protocol 3");

        Require(Near(UnpackCrouchRatio(packet.crouchRatio), 52.0f / 64.0f, 0.01f), "Crouch ratio quantisation");
        Require(packet.standingHeightMm == 1650u, "Standing height in millimetres");
        Require(Near(UnpackBodyYaw(packet.bodyYaw), -25.5f, 0.006f), "Body yaw centidegrees");
        Require(PackPosition(1e9f) == 32767 && PackPosition(-1e9f) == -32767 && PackPosition(NAN) == 0,
            "Positions clamp symmetrically and reject NaN");

        float x, y, z, w;
        UnpackQuaternion(packet.hands[kLeft].rotation, x, y, z, w);
        float m[3][3], pitch, yaw, roll;
        QuaternionMatrix(x, y, z, w, m);
        MatrixAngles(m, pitch, yaw, roll);
        Require(Near(pitch, 10.0f, 0.05f) && Near(yaw, 20.0f, 0.05f) && Near(roll, -5.0f, 0.05f),
            "Hand quaternion reproduces the Source angles");

        // The generic text codec is byte-compatible with the v2 encoder.
        l4d2vr_pose::WirePacket v2{};
        v2.bodyYaw = 1234;
        v2.hmd.position[2] = 400;
        l4d2vr_pose::FinalizePacket(v2);
        std::string v2Text, genericText;
        Require(l4d2vr_pose::EncodePayload(v2, v2Text), "v2 encode");
        EncodeBytesToText(reinterpret_cast<const std::uint8_t*>(&v2), sizeof(v2), genericText);
        Require(v2Text == genericText, "Shared alphabet encoder matches v2 byte for byte");
        Require(PayloadVersionForText(v2Text.c_str()) == 2, "Payload length identifies protocol 2");

        // Normalize produces a valid packet from a sloppy one.
        PacketV3 sloppy = packet;
        sloppy.flags = static_cast<std::uint8_t>(sloppy.flags & ~kFlagRightHandValid);
        sloppy.flags = static_cast<std::uint8_t>(sloppy.flags | 0x80u);
        sloppy.hands[kLeft].rotation[0] = static_cast<std::int16_t>(-sloppy.hands[kLeft].rotation[0]);
        sloppy.hands[kLeft].rotation[1] = static_cast<std::int16_t>(-sloppy.hands[kLeft].rotation[1]);
        sloppy.hands[kLeft].rotation[2] = static_cast<std::int16_t>(-sloppy.hands[kLeft].rotation[2]);
        sloppy.hands[kLeft].rotation[3] = static_cast<std::int16_t>(-sloppy.hands[kLeft].rotation[3]);
        Require(!ValidateFields(sloppy), "Sloppy packet is invalid before normalising");
        Normalize(sloppy);
        Require(ValidateFields(sloppy) && sloppy.fingers[kRight].kind == FingerPose::None &&
            sloppy.hands[kRight].position[0] == 0 && sloppy.hands[kLeft].rotation[3] > 0,
            "Normalize clears invalid sections and canonicalises the quaternion sign");
    }

    void TestCorruption()
    {
        using namespace l4d2vr_pose_v3;
        const PacketV3 packet = SamplePacket();
        WireBytes good{};
        Require(Encode(packet, good), "Encode for corruption tests");
        PacketV3 out{};

        for (std::size_t index = 0; index < good.size(); ++index)
        {
            WireBytes bad = good;
            bad[index] ^= 0x01u;
            Require(!Decode(bad.data(), bad.size(), out), "Any single-bit corruption is rejected");
        }

        // Re-sign a modified packet so only the targeted check can reject it.
        auto resign = [](WireBytes& bytes)
        {
            const std::uint16_t crc = l4d2vr_pose::Crc16Ccitt(bytes.data(), kCrcOffset);
            bytes[kCrcOffset] = static_cast<std::uint8_t>(crc & 0xFFu);
            bytes[kCrcOffset + 1] = static_cast<std::uint8_t>(crc >> 8);
        };
        WireBytes bad = good;
        bad[1] = 0x20u; resign(bad);
        Require(!Decode(bad.data(), bad.size(), out), "Version 2 byte is rejected");
        bad = good; bad[1] = 0x31u; resign(bad);
        Require(!Decode(bad.data(), bad.size(), out), "Reserved version nibble is rejected");
        bad = good; bad[0] = 0xB6u; resign(bad);
        Require(!Decode(bad.data(), bad.size(), out), "Wrong magic is rejected");
        bad = good; bad[4] |= 0x40u; resign(bad);
        Require(!Decode(bad.data(), bad.size(), out), "Unknown flag bit is rejected");
        bad = good; bad[5] = static_cast<std::uint8_t>(FingerPose::Count); resign(bad);
        Require(!Decode(bad.data(), bad.size(), out), "Out-of-range finger kind is rejected");
        bad = good; bad[4] = static_cast<std::uint8_t>(bad[4] & ~kFlagRightHandValid); resign(bad);
        Require(!Decode(bad.data(), bad.size(), out), "Data in an invalid hand section is rejected");
        bad = good; bad[22] = 0x51u; bad[23] = 0x46u; resign(bad);   // 18000 + 1
        Require(!Decode(bad.data(), bad.size(), out), "Body yaw beyond 180 degrees is rejected");
        bad = good; bad[30] = 0x00u; bad[31] = 0x40u; resign(bad);   // head forward x -> 16384
        Require(!Decode(bad.data(), bad.size(), out), "Non-unit head forward is rejected");
        bad = good; bad[24] = 0x00u; bad[25] = 0x80u; resign(bad);   // -32768
        Require(!Decode(bad.data(), bad.size(), out), "Non-canonical -32768 is rejected");
        Require(!Decode(good.data(), good.size() - 1, out), "Short buffer is rejected");
        Require(Decode(good.data(), good.size(), out), "Untouched packet still decodes");

        PacketV3 fields = packet;
        fields.hands[kLeft].rotation[3] = static_cast<std::int16_t>(-fields.hands[kLeft].rotation[3]);
        Require(!ValidateFields(fields), "Non-canonical quaternion sign is rejected");
        fields = packet;
        fields.hands[kRight].rotation[0] = 0; fields.hands[kRight].rotation[1] = 0;
        fields.hands[kRight].rotation[2] = 0; fields.hands[kRight].rotation[3] = 20000;
        Require(!ValidateFields(fields), "Non-unit quaternion is rejected");
        fields = packet;
        fields.headUp[0] = PackUnit(0.5f); fields.headUp[2] = PackUnit(0.866f);
        Require(!ValidateFields(fields), "Non-orthogonal head basis is rejected");
        fields = packet;
        fields.fingers[kLeft] = HandFingers{};
        fields.fingers[kLeft].curls[2] = 4u;
        Require(!ValidateFields(fields), "Finger curls with kind None are rejected");
        fields = packet;
        fields.standingHeightMm = 200u;
        Require(!ValidateFields(fields), "Implausible standing height is rejected");
        WireBytes unused{};
        Require(!Encode(fields, unused), "Encode refuses invalid packets");

        std::string text;
        Require(EncodeText(packet, text), "Text encode for corruption tests");
        std::string badText = text;
        badText[10] = '+';
        Require(!DecodeText(badText.c_str(), out), "Characters outside the alphabet are rejected");
        badText = text;
        badText[10] = badText[10] == 'A' ? 'B' : 'A';
        Require(!DecodeText(badText.c_str(), out), "Altered text fails the CRC");
        Require(!DecodeText(text.substr(0, 95).c_str(), out) && !DecodeText((text + "A").c_str(), out) &&
            !DecodeText(nullptr, out), "Wrong text length is rejected");
    }

    void TestDownconvert()
    {
        using namespace l4d2vr_pose_v3;
        PacketV3 packet = SamplePacket();
        packet.bodyYaw = PackBodyYaw(90.0f);
        // Left hand straight ahead of the view: with the body turned 90 degrees
        // left it is on the body's right.
        packet.hands[kLeft].position[0] = PackPosition(10.0f);
        packet.hands[kLeft].position[1] = 0;
        packet.hands[kLeft].position[2] = PackPosition(40.0f);
        PackAnglesAsQuaternion(0.0f, 0.0f, 0.0f, packet.hands[kLeft].rotation);
        Require(ValidateFields(packet), "Downconvert input is valid");

        l4d2vr_pose::WirePacket v2{};
        Require(DownconvertToV2(packet, 170.0f, v2), "Downconvert succeeds");
        Require(l4d2vr_pose::ValidatePacket(v2), "Downconverted packet passes v2 ValidatePacket");
        Require(l4d2vr_pose::PacketValidMask(v2) == l4d2vr_pose::kValidMask, "All components valid in v2");
        Require(AngleNear(V2Angle(v2.bodyYaw), 260.0f, 0.02f), "v2 body yaw is absolute (view + relative)");
        Require(Near(V2Position(v2.leftHand.position[0]), 0.0f, 0.13f) &&
            Near(V2Position(v2.leftHand.position[1]), 10.0f, 0.13f) &&
            Near(V2Position(v2.leftHand.position[2]), 40.0f, 0.13f), "Hand position rebased to the body frame");
        Require(AngleNear(V2Angle(v2.leftHand.rotation[1]), -90.0f, 0.05f) &&
            AngleNear(V2Angle(v2.leftHand.rotation[0]), 0.0f, 0.05f), "Hand yaw relative to the body");
        Require(AngleNear(V2Angle(v2.hmd.rotation[1]), 30.0f - 90.0f, 0.1f), "Head yaw relative to the body");
        Require((v2.featureMask & l4d2vr_pose::kFeatureLeftFingerCurls) != 0u &&
            (v2.handStateFlags & l4d2vr_pose::kHandStateRightNativeFingerAnimation) != 0u &&
            (v2.handStateFlags & l4d2vr_pose::kHandStateLeftNativeFingerAnimation) == 0u &&
            std::memcmp(v2.leftFingerCurls, packet.fingers[kLeft].curls, 5) == 0,
            "Curl hand sends curls, trigger hand stays native");

        std::string text;
        l4d2vr_pose::WirePacket reparsed{};
        Require(l4d2vr_pose::EncodePayload(v2, text) && l4d2vr_pose::DecodePayload(text.c_str(), reparsed),
            "Downconverted packet survives the v2 text codec");

        // Two-handed support grip.
        PacketV3 support = SamplePacket();
        support.fingers[kLeft] = HandFingers{};
        support.fingers[kLeft].kind = FingerPose::Support;
        Require(DownconvertToV2(support, 0.0f, v2) && l4d2vr_pose::ValidatePacket(v2) &&
            (v2.handStateFlags & l4d2vr_pose::kHandStateTwoHandedGrip) != 0u &&
            (v2.featureMask & (l4d2vr_pose::kFeatureLeftFingerCurls | l4d2vr_pose::kFeatureRightFingerCurls)) == 0u,
            "Support hand maps to the v2 two-handed grip");

        // Empty hands.
        PacketV3 empty = SamplePacket();
        empty.fingers[kRight].kind = FingerPose::Point;
        Require(DownconvertToV2(empty, 0.0f, v2) && l4d2vr_pose::ValidatePacket(v2) &&
            (v2.handStateFlags & l4d2vr_pose::kHandStateEmptyHands) != 0u &&
            (v2.featureMask & l4d2vr_pose::kFeatureRightFingerCurls) != 0u, "Two curl hands map to empty hands");

        // Missing hands and head.
        PacketV3 sparse{};
        sparse.flags = kFlagRightHandValid;
        PackAnglesAsQuaternion(0.0f, 45.0f, 0.0f, sparse.hands[kRight].rotation);
        sparse.fingers[kRight].kind = FingerPose::Relaxed;
        Require(DownconvertToV2(sparse, -30.0f, v2) && l4d2vr_pose::ValidatePacket(v2) &&
            l4d2vr_pose::PacketValidMask(v2) == l4d2vr_pose::kValidRightHand, "Sparse packets downconvert");

        PacketV3 invalid = SamplePacket();
        invalid.flags = static_cast<std::uint8_t>(invalid.flags | 0x80u);
        Require(!DownconvertToV2(invalid, 0.0f, v2), "Invalid v3 packets are not downconverted");
    }

    void TestSequenceAndStore()
    {
        using namespace l4d2vr_pose_v3;
        Require(SequenceIsNewer(1u, 0u) && !SequenceIsNewer(0u, 1u) && !SequenceIsNewer(7u, 7u), "Basic ordering");
        Require(SequenceIsNewer(0u, 65535u) && SequenceIsNewer(5u, 65530u) && !SequenceIsNewer(65530u, 5u),
            "Ordering is wrap-safe");
        Require(SequenceIsNewer(32767u, 0u) && !SequenceIsNewer(32768u, 0u), "Half-range boundary");

        ReceiveStore store;
        PacketV3 packet = SamplePacket();
        Require(store.Latest(0.0) == nullptr, "Empty store has nothing");
        packet.sequence = 65534u;
        Require(store.Offer(packet, 10.0), "First packet accepted");
        packet.sequence = 65533u;
        Require(!store.Offer(packet, 10.1), "Older packet dropped");
        packet.sequence = 65534u;
        Require(!store.Offer(packet, 10.1), "Duplicate dropped");
        packet.sequence = 2u;
        Require(store.Offer(packet, 10.2) && store.packet.sequence == 2u, "Wrapped newer packet accepted");
        Require(store.Latest(10.69) != nullptr && store.Latest(10.71) == nullptr, "Stale after 0.5 s");
        packet.sequence = 1u;
        Require(!store.Offer(packet, 11.1), "Older packet within the silence window is dropped");
        Require(store.Offer(packet, 11.3) && store.packet.sequence == 1u, "Any sequence after 1 s of silence (restart)");
        store.Reset();
        Require(!store.valid, "Reset clears the slot");
    }
}

int main()
{
    TestRoundTrip();
    TestCorruption();
    TestDownconvert();
    TestSequenceAndStore();
    std::printf("vr_pose_protocol_v3_tests: %d checks passed\n", checks);
    return 0;
}

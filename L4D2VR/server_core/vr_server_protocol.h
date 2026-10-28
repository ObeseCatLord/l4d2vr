#pragma once

// Wire constants shared by the dedicated-server plugin and VR clients.
//
// Handshake (server -> client unless noted):
//   l4d2vr_pose_ack 2            sent to every connecting client exactly like the
//                                proven pose relay (ClientPutInServer/ClientActive,
//                                up to 3 attempts, 1 s apart, until a hello).
//   client: l4d2vr_pose_hello 2  existing clients answer pose_ack / server_ack>=3.
//   client: l4d2vr_pose_hello 3  (new) opt into pose protocol v3 relay.
//   client: l4d2vr_feature_hello <hexmask> <version>
//                                (new, optional) restrict the server-side
//                                features applied to this player.
// After the first hello from a client, and never before, the server sends the
// capability bundle in this order:
//   l4d2vr_server_ack 4          dedicated full-feature plugin; clients that
//                                check >= 3 treat it as VR-aware. Only sent when
//                                the usercmd decoder is installed.
//   l4d2vr_interaction_ack 1     physical interaction (grip pickup/release,
//                                native reload gate) is supported.
//   l4d2vr_shell_capability 1 <token>   (PR409 shell transactions)
//   l4d2vr_mag_capability 3 <token>     (PR409 magazine transactions; v3
//                                returns ejected rounds to the reserve)
//   l4d2vr_feature_ack <hexmask> 1      resolved feature bits, lowercase hex
//                                without a 0x prefix.
//
// Usercmd intents carried in CUserCmd::impulse by VR clients:
//   234 grip pickup (with IN_USE), 235 grip release, 236 block native reload
//   237 holster drop: drop the active weapon after this command
//   238 shove: this command shoves (IN_ATTACK2 is added) aimed by the hand
//   239 pill give: give pills/adrenaline to the teammate the hand points at
//   240..243 object pull (stripped; not implemented by the dedicated plugin)
//   244 hand throw: throw the held grenade from the hand in this command, with
//       the hand velocity packed into forwardmove/sidemove/weaponselect/
//       weaponsubtype (vr_hand_throw.h); only sent when HandThrow (0x400) is
//       advertised

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace l4d2vr_server
{
    // ---- Feature bits (l4d2vr_feature_ack) ----------------------------------
    enum FeatureBit : std::uint32_t
    {
        kFeatureMeleeSweepV2 = 0x1u,
        kFeatureShoveAim = 0x2u,
        kFeatureAkimboRays = 0x4u,
        kFeatureMagTxn = 0x8u,
        kFeatureShellTxn = 0x10u,
        kFeatureHolsterDrop = 0x20u,
        kFeatureThrowVelocity = 0x40u,
        kFeaturePillGive = 0x80u,
        kFeaturePoseV3 = 0x100u,
        kFeatureInstantDeploy = 0x200u,
        kFeatureHandThrow = 0x400u,
    };
    constexpr std::uint32_t kFeatureAllMask = 0x7FFu;
    constexpr unsigned kFeatureAckVersion = 1u;

    inline const char* FeatureName(std::uint32_t bit)
    {
        switch (bit)
        {
        case kFeatureMeleeSweepV2: return "MeleeSweepV2";
        case kFeatureShoveAim: return "ShoveAim";
        case kFeatureAkimboRays: return "AkimboRays";
        case kFeatureMagTxn: return "MagTxn";
        case kFeatureShellTxn: return "ShellTxn";
        case kFeatureHolsterDrop: return "HolsterDrop";
        case kFeatureThrowVelocity: return "ThrowVelocity";
        case kFeaturePillGive: return "PillGive";
        case kFeaturePoseV3: return "PoseV3";
        case kFeatureInstantDeploy: return "InstantDeploy";
        case kFeatureHandThrow: return "HandThrow";
        default: return "?";
        }
    }

    // ---- Versions -----------------------------------------------------------
    constexpr unsigned kServerAckVersion = 4u;      // dedicated full-feature plugin
    constexpr unsigned kInteractionVersion = 1u;    // l4d2vr_wire::kPhysicalVersion
    constexpr unsigned kPoseV2 = 2u;
    constexpr unsigned kPoseV3 = 3u;

    // ---- Impulses -----------------------------------------------------------
    constexpr std::uint8_t kImpulseGripPickup = 234u;
    constexpr std::uint8_t kImpulseGripRelease = 235u;
    constexpr std::uint8_t kImpulseBlockNativeReload = 236u;
    constexpr std::uint8_t kImpulseHolsterDrop = 237u;
    constexpr std::uint8_t kImpulseShove = 238u;
    constexpr std::uint8_t kImpulsePillGive = 239u;
    constexpr std::uint8_t kImpulseObjectPullBegin = 240u;
    constexpr std::uint8_t kImpulseObjectPullEnd = 243u;
    constexpr std::uint8_t kImpulseHandThrow = 244u;      // l4d2vr_hand_throw::kImpulse

    // ---- buttons bits 26..31 ("carry word") -----------------------------------
    constexpr std::uint32_t kCarryWordShift = 26u;
    constexpr std::uint32_t kCarryWordMask = 0x3Fu << kCarryWordShift;
    constexpr unsigned kCarryRightDropMarker = 60u;   // l4d2vr_pistol::kRightDropMarker
    constexpr unsigned kCarryLeftDropMarker = 61u;    // l4d2vr_pistol::kLeftDropMarker
    constexpr unsigned kCarryRightShotMarker = 62u;   // l4d2vr_dual::kRightShotMarker
    constexpr unsigned kCarryLeftShotMarker = 63u;    // l4d2vr_dual::kLeftShotMarker

    // ---- Source button bits -------------------------------------------------
    constexpr int kInAttack = 1 << 0;
    constexpr int kInJump = 1 << 1;
    constexpr int kInUse = 1 << 5;
    constexpr int kInAttack2 = 1 << 11;
    constexpr int kInReload = 1 << 13;

    // ---- L4D2 weapon ids (CSWeaponID) used by the server --------------------
    namespace weapon
    {
        constexpr int kNone = 0, kPistol = 1, kMolotov = 13, kPipeBomb = 14, kPainPills = 15,
            kGascan = 16, kPropane = 17, kOxygen = 18, kMelee = 19, kChainsaw = 20,
            kAdrenaline = 23, kVomitJar = 25, kGnome = 27, kCola = 28, kFireworks = 29,
            kMagnum = 32;
        inline bool IsThrowable(int id) { return id == kMolotov || id == kPipeBomb || id == kVomitJar; }
        inline bool IsCarryable(int id)
        {
            return id == kGascan || id == kPropane || id == kOxygen || id == kGnome || id == kCola || id == kFireworks;
        }
        // Same set as l4d2vr_calibration::IsFirearm (vr_weapon_calibration.h).
        constexpr bool IsFirearm(int id)
        {
            return (id >= 1 && id <= 11) || id == 21 || id == 26 || (id >= 32 && id <= 37) || id == 45;
        }
        inline bool IsGiveable(int id) { return id == kPainPills || id == kAdrenaline; }
        // Matches the client's ManualInventoryThrowWeaponIdIsSupported minus the
        // carry props, i.e. ManualInventoryThrowWeaponIdRequiresCustomDrop.
        inline bool IsInventoryDroppable(int id)
        {
            switch (id)
            {
            case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11:
            case 19: case 20: case 21: case 26: case 32: case 33: case 34: case 35: case 36: case 37:
            case 13: case 14: case 25: case 12: case 15: case 22: case 23: case 24: case 30: case 31:
                return true;
            default:
                return false;
            }
        }
    }

    // ---- Command text ---------------------------------------------------------
    constexpr char kPoseHelloCommand[] = "l4d2vr_pose_hello";
    constexpr char kPoseUploadCommand[] = "l4d2vr_pose_upload";
    constexpr char kPoseReceiveCommand[] = "l4d2vr_pose_receive";
    constexpr char kFeatureHelloCommand[] = "l4d2vr_feature_hello";
    constexpr char kShellInsertCommand[] = "l4d2vr_shell_insert_v1";
    constexpr char kMagActionCommand[] = "l4d2vr_mag_action_v2";
    constexpr char kPoseAckText[] = "l4d2vr_pose_ack 2\n";   // byte-identical to the proven relay

    inline bool FormatServerAck(char* out, std::size_t size)
    {
        const int n = std::snprintf(out, size, "l4d2vr_server_ack %u\n", kServerAckVersion);
        return n > 0 && static_cast<std::size_t>(n) < size;
    }

    inline bool FormatInteractionAck(char* out, std::size_t size)
    {
        const int n = std::snprintf(out, size, "l4d2vr_interaction_ack %u\n", kInteractionVersion);
        return n > 0 && static_cast<std::size_t>(n) < size;
    }

    inline bool FormatFeatureAck(std::uint32_t mask, char* out, std::size_t size)
    {
        const int n = std::snprintf(out, size, "l4d2vr_feature_ack %x %u\n",
            static_cast<unsigned>(mask & kFeatureAllMask), kFeatureAckVersion);
        return n > 0 && static_cast<std::size_t>(n) < size;
    }

    // Strict hex parser for "<hexmask>" (optional 0x prefix, at most 8 digits).
    inline bool ParseHexMask(const char* text, std::uint32_t& out)
    {
        out = 0u;
        if (!text || !*text) return false;
        if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
        if (!*text) return false;
        std::uint32_t value = 0u;
        int digits = 0;
        for (; *text; ++text)
        {
            const char c = *text;
            unsigned d = 0u;
            if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
            else return false;
            if (++digits > 8) return false;
            value = (value << 4) | d;
        }
        out = value;
        return true;
    }

    // Strict decimal parser, identical contract to the relay's ParseSequence.
    inline bool ParseDecimal(const char* text, std::uint32_t maximum, std::uint32_t& out)
    {
        out = 0u;
        if (!text || !*text) return false;
        std::uint32_t value = 0u;
        for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c)
        {
            if (*c < '0' || *c > '9') return false;
            const std::uint32_t digit = static_cast<std::uint32_t>(*c - '0');
            if (value > (maximum - digit) / 10u) return false;
            value = value * 10u + digit;
        }
        out = value;
        return true;
    }

    // Client-side helper: parses "l4d2vr_feature_ack <hexmask> <version>" args.
    inline bool ParseFeatureAck(const char* maskText, const char* versionText,
        std::uint32_t& mask, unsigned& version)
    {
        std::uint32_t parsedVersion = 0u;
        if (!ParseHexMask(maskText, mask) || !ParseDecimal(versionText, 255u, parsedVersion))
            return false;
        version = parsedVersion;
        mask &= kFeatureAllMask;
        return version >= 1u;
    }
}

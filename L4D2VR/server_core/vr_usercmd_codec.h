#pragma once

// VR usercmd codec shared by the dedicated-server plugin (and, later, the
// listen host). It mirrors, bit for bit, the client's Hooks::dWriteUsercmd
// encoding and the listen host's Hooks::dReadUsercmd decoding/stripping
// (L4D2VR/hooks/hooks_combat_network.inl). std-only.
//
// Encoding summary (client -> server, only when the server acknowledged VR):
//   tick_count      negated: a negative tick_count flags a VR command
//   command_number  += ((int)roll + 180) / 2 * 10000000; negated while a melee
//                   swing is in motion
//   mousedx/mousedy controller pitch/yaw * 10 (int16)
//   viewangles.z    controller position x
//   upmove          controller position y
//   viewangles.x    (int)(pos.z * 10) * 10000 +- (int)((pitch + 360) * 10)
//   impulse         234..243 interaction intents (vr_server_protocol.h)
//   weaponselect    object-pull entity index (impulse 240..243 only)
//   weaponsubtype   object-pull target hint (impulse 240..243 only)
//   impulse 244     hand throw: forwardmove/sidemove/weaponselect/weaponsubtype
//                   carry the hand velocity (vr_hand_throw.h)
//   buttons 26..31  "carry word": weapon id + 1 of a released carry/inventory
//                   item, or pistol markers 60..63
// Commands without the VR flag are never modified.

#include <climits>
#include <cmath>
#include <cstdint>

#include "../vr_dual_pistols.h"
#include "../vr_hand_throw.h"
#include "../vr_pistol_detachment.h"
#include "../vr_interaction_protocol.h"
#include "../vr_server_pistol_commands.h"
#include "vr_server_math.h"
#include "vr_server_protocol.h"

namespace l4d2vr_server
{
    // Portable mirror of the gameplay fields of Source's CUserCmd. Shells copy
    // their native CUserCmd into this, decode, and copy the result back.
    struct UsercmdData
    {
        int command_number = 0;
        int tick_count = 0;
        float viewangles[3]{};
        float forwardmove = 0.0f;
        float sidemove = 0.0f;
        float upmove = 0.0f;
        int buttons = 0;
        std::uint8_t impulse = 0u;
        int weaponselect = 0;
        int weaponsubtype = 0;
        int random_seed = 0;
        std::int16_t mousedx = 0;
        std::int16_t mousedy = 0;
    };

    enum class Intent : std::uint8_t
    {
        None = 0,
        GripPickup,
        GripRelease,
        BlockNativeReload,
        HolsterDrop,
        Shove,
        PillGive,
        ObjectPull,
        HandThrow,
    };

    struct DecodedUsercmd
    {
        bool vr = false;              // the command carried the VR flag (and was stripped)
        bool poseValid = false;       // controller pose decoded to finite values
        bool meleeSwing = false;      // negative command_number
        Vec3 controllerPos{};
        Angles controllerAngles{};    // as decoded (pitch, yaw, roll); not clamped
        std::uint8_t incomingImpulse = 0u;
        Intent intent = Intent::None;
        std::uint8_t objectPullCommand = 0u;
        int objectPullEntity = 0;
        int objectPullHint = 0;
        unsigned carryWord = 0u;      // buttons bits 26..31 before stripping
        int carryWeaponId = weapon::kNone;   // weapon id + 1 decoded from the carry word
        l4d2vr_dual::Hand shotHand = l4d2vr_dual::Hand::None;
        l4d2vr_pistol::Hand dropHand = l4d2vr_pistol::Hand::None;
        l4d2vr_pistol::Hand pickupHand = l4d2vr_pistol::Hand::None;
        l4d2vr_server_pistol::ReloadPolicy reload = l4d2vr_server_pistol::ReloadPolicy::Native;
        bool attackDown = false;
        bool useDown = false;
        l4d2vr_hand_throw::Payload handThrow{};   // intent HandThrow with a valid payload
    };

    namespace codec_detail
    {
        // Negation that cannot overflow: INT_MIN maps to INT_MAX.
        inline int SafeNegate(int value)
        {
            return value == INT_MIN ? INT_MAX : -value;
        }

        inline bool FloatFitsInt(float value)
        {
            return std::isfinite(value) && value > -2147483520.0f && value < 2147483520.0f;
        }
    }

    // Client encoder (test oracle and future shared use). Mirrors the arithmetic
    // in Hooks::dWriteUsercmd after the pose was chosen. `cmd` must hold the
    // outgoing gameplay command with impulse/weaponselect/weaponsubtype/carry
    // word already set by the caller.
    inline void EncodeVRUsercmd(UsercmdData& cmd, const Vec3& controllerPos,
        const Angles& controllerAngles, bool meleeSwing)
    {
        cmd.tick_count *= -1;
        cmd.mousedx = static_cast<std::int16_t>(static_cast<int>(controllerAngles.x * 10.0f));
        cmd.mousedy = static_cast<std::int16_t>(static_cast<int>(controllerAngles.y * 10.0f));
        const int rollEncoding = ((static_cast<int>(controllerAngles.z) + 180) / 2 * 10000000);
        cmd.command_number += rollEncoding;
        if (meleeSwing)
            cmd.command_number *= -1;

        const float xAngleOrig = cmd.viewangles[0];
        cmd.viewangles[2] = controllerPos.x;
        cmd.upmove = controllerPos.y;
        const int encodedAngle = static_cast<int>((xAngleOrig + 360.0f) * 10.0f);
        int encoding = static_cast<int>(controllerPos.z * 10.0f) * 10000;
        encoding += (encoding < 0) ? -encodedAngle : encodedAngle;
        cmd.viewangles[0] = static_cast<float>(encoding);
    }

    // Server decoder. Mirrors dReadUsercmd: un-flags the command, extracts the
    // controller pose and the interaction intents and strips every VR-only field
    // so the game sees an ordinary command. A command without the VR flag is
    // returned untouched with vr == false.
    inline DecodedUsercmd DecodeVRUsercmd(UsercmdData& cmd)
    {
        DecodedUsercmd out{};
        if (cmd.tick_count >= 0)
            return out;

        out.vr = true;
        cmd.tick_count = codec_detail::SafeNegate(cmd.tick_count);
        if (cmd.command_number < 0)
        {
            cmd.command_number = codec_detail::SafeNegate(cmd.command_number);
            out.meleeSwing = true;
        }

        out.controllerAngles.x = static_cast<float>(cmd.mousedx) / 10.0f;
        out.controllerAngles.y = static_cast<float>(cmd.mousedy) / 10.0f;
        out.controllerPos.x = cmd.viewangles[2];
        out.controllerPos.y = cmd.upmove;

        const int rollEncoding = cmd.command_number / 10000000;
        cmd.command_number -= rollEncoding * 10000000;
        out.controllerAngles.z = static_cast<float>((rollEncoding * 2) - 180);

        // viewangles.x carries pos.z and the real pitch. Guard the float->int
        // conversions the client performs; a malformed value must neither invoke
        // undefined behaviour nor leak a NaN pitch into the game.
        const float packed = cmd.viewangles[0];
        float decodedAngle = 0.0f;
        float decodedZ = 0.0f;
        bool packedValid = false;
        if (codec_detail::FloatFitsInt(packed / 10000.0f))
        {
            const int decodedZInt = static_cast<int>(packed / 10000);
            if (decodedZInt >= -214748 && decodedZInt <= 214748)
            {
                decodedAngle = std::fabs(static_cast<float>(packed - static_cast<float>(decodedZInt * 10000)) / 10);
                decodedAngle -= 360.0f;
                decodedZ = static_cast<float>(decodedZInt) / 10.0f;
                packedValid = std::isfinite(decodedAngle);
            }
        }
        out.controllerPos.z = decodedZ;
        cmd.viewangles[0] = packedValid ? decodedAngle : 0.0f;
        cmd.viewangles[2] = 0.0f;
        cmd.upmove = 0.0f;
        out.poseValid = packedValid && out.controllerPos.Finite() && out.controllerAngles.Finite();

        // Interaction impulses.
        const std::uint8_t incoming = cmd.impulse;
        out.incomingImpulse = incoming;
        switch (incoming)
        {
        case kImpulseGripPickup: out.intent = Intent::GripPickup; cmd.impulse = 0u; break;
        case kImpulseGripRelease: out.intent = Intent::GripRelease; cmd.impulse = 0u; break;
        case kImpulseBlockNativeReload: out.intent = Intent::BlockNativeReload; cmd.impulse = 0u; break;
        case kImpulseHolsterDrop: out.intent = Intent::HolsterDrop; cmd.impulse = 0u; break;
        case kImpulseShove: out.intent = Intent::Shove; cmd.impulse = 0u; break;
        case kImpulsePillGive: out.intent = Intent::PillGive; cmd.impulse = 0u; break;
        case kImpulseHandThrow:
            // Strip the payload whatever it holds; a malformed one throws nothing.
            out.handThrow = l4d2vr_hand_throw::DecodeFields(cmd.forwardmove, cmd.sidemove,
                cmd.weaponselect, cmd.weaponsubtype);
            out.intent = out.handThrow.valid ? Intent::HandThrow : Intent::None;
            cmd.impulse = 0u;
            break;
        default:
            if (incoming >= kImpulseObjectPullBegin && incoming <= kImpulseObjectPullEnd)
            {
                out.intent = Intent::ObjectPull;
                out.objectPullCommand = incoming;
                out.objectPullEntity = cmd.weaponselect;
                out.objectPullHint = cmd.weaponsubtype;
                cmd.impulse = 0u;
                cmd.weaponselect = 0;
                cmd.weaponsubtype = 0;
            }
            break;
        }
        out.reload = incoming == kImpulseBlockNativeReload ? l4d2vr_server_pistol::ReloadPolicy::Block :
            (l4d2vr_wire::KeepNativeReloadGate(incoming) ? l4d2vr_server_pistol::ReloadPolicy::Preserve
                                                        : l4d2vr_server_pistol::ReloadPolicy::Native);

        // Carry word in buttons bits 26..31.
        const std::uint32_t buttons = static_cast<std::uint32_t>(cmd.buttons);
        out.carryWord = (buttons & kCarryWordMask) >> kCarryWordShift;
        cmd.buttons = static_cast<int>(buttons & ~kCarryWordMask);
        out.attackDown = (cmd.buttons & kInAttack) != 0;
        out.useDown = (cmd.buttons & kInUse) != 0;
        out.dropHand = l4d2vr_pistol::DecodeDrop(out.carryWord, out.attackDown);
        out.pickupHand = l4d2vr_pistol::DecodePickup(out.carryWord, incoming, out.useDown, out.attackDown);
        out.shotHand = l4d2vr_dual::DecodeShotHand(out.carryWord, out.attackDown);
        out.carryWeaponId = out.dropHand != l4d2vr_pistol::Hand::None ? weapon::kPistol
            : out.carryWord > 0u ? static_cast<int>(out.carryWord - 1u) : weapon::kNone;
        return out;
    }

    // True when the decoded carry word marks a released inventory item
    // (ManualInventoryThrow on the listen host): no attack, droppable id.
    inline bool IsInventoryRelease(const DecodedUsercmd& d)
    {
        return d.vr && !d.attackDown && weapon::IsInventoryDroppable(d.carryWeaponId);
    }

    // True when the carry word marks a released carried prop (gascan etc.).
    inline bool IsCarryRelease(const DecodedUsercmd& d)
    {
        return d.vr && d.attackDown && weapon::IsCarryable(d.carryWeaponId);
    }
}

#pragma once
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

// In VR the viewmodel follows the controller, so the game's own strike
// animations (the gun shove "melee" sequence, melee weapon swings) must never
// play on it: the shove's sound and hit still happen, the hand just stays where
// the controller put it. This classifies the viewmodel's current sequence from
// the model's studio header (activity name, e.g. ACT_VM_MELEE).
namespace l4d2vr_vm_anim
{
    enum class Kind : int
    {
        Unknown = 0, // sequence not resolvable (included model, bad header)
        Idle = 1,    // ACT_VM_IDLE*: safe to capture as the held pose
        Strike = 2,  // shove / swing: must not be shown
        Other = 3,   // fire, reload, deploy, pump...: shown as is
    };

    // studiohdr_t (MDL v44-49, 32-bit): the in-memory header is the .mdl file.
    constexpr std::size_t kVersionOffset = 4;
    constexpr std::size_t kLengthOffset = 76;
    constexpr std::size_t kNumLocalSeqOffset = 188;
    constexpr std::size_t kLocalSeqIndexOffset = 192;
    constexpr std::size_t kSeqDescSize = 212;      // mstudioseqdesc_t
    constexpr std::size_t kSeqLabelOffset = 4;     // szlabelindex
    constexpr std::size_t kSeqActivityOffset = 8;  // szactivitynameindex

    inline std::int32_t ReadI32(const unsigned char* p)
    {
        std::int32_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }

    // A NUL-terminated name at base+offset that lies inside [hdr, hdr+length).
    inline const char* NameAt(const unsigned char* hdr, std::size_t length, std::size_t base, std::int32_t offset)
    {
        if (offset <= 0)
            return "";
        const std::size_t at = base + static_cast<std::size_t>(offset);
        if (at >= length)
            return nullptr;
        const std::size_t limit = length - at < 128u ? length - at : 128u;
        if (!std::memchr(hdr + at, 0, limit))
            return nullptr;
        return reinterpret_cast<const char*>(hdr + at);
    }

    // Label and activity name of a local sequence. False for anything outside
    // the header (sequences of included animation models, corrupt data).
    inline bool SequenceNames(const unsigned char* hdr, int sequence, const char*& label, const char*& activity)
    {
        label = activity = nullptr;
        if (!hdr || sequence < 0)
            return false;
        const std::int32_t version = ReadI32(hdr + kVersionOffset);
        const std::int32_t length = ReadI32(hdr + kLengthOffset);
        if (version < 44 || version > 49 || length < static_cast<std::int32_t>(kLocalSeqIndexOffset + 4) ||
            length > (64 << 20))
            return false;
        const std::int32_t count = ReadI32(hdr + kNumLocalSeqOffset);
        const std::int32_t index = ReadI32(hdr + kLocalSeqIndexOffset);
        if (count <= 0 || count > 4096 || sequence >= count || index <= 0)
            return false;
        const std::size_t base = static_cast<std::size_t>(index) + static_cast<std::size_t>(sequence) * kSeqDescSize;
        if (base + kSeqDescSize > static_cast<std::size_t>(length))
            return false;
        label = NameAt(hdr, static_cast<std::size_t>(length), base, ReadI32(hdr + base + kSeqLabelOffset));
        activity = NameAt(hdr, static_cast<std::size_t>(length), base, ReadI32(hdr + base + kSeqActivityOffset));
        return label && activity;
    }

    inline bool ContainsNoCase(const char* text, const char* token)
    {
        if (!text || !token || !*token)
            return false;
        const std::size_t n = std::strlen(token);
        for (const char* p = text; *p; ++p)
        {
            std::size_t i = 0;
            while (i < n && p[i] &&
                std::toupper(static_cast<unsigned char>(p[i])) == std::toupper(static_cast<unsigned char>(token[i])))
                ++i;
            if (i == n)
                return true;
        }
        return false;
    }

    // meleeWeapon: a swung melee weapon (not the chainsaw), whose primary
    // attack is a swing rather than a shot.
    inline Kind Classify(const char* activity, const char* label, bool meleeWeapon)
    {
        if (!activity && !label)
            return Kind::Unknown;
        const char* const a = activity ? activity : "";
        const char* const l = label ? label : "";
        static const char* const kStrike[] = {
            "ACT_VM_MELEE", "SHOVE", "SWINGHARD", "SWINGHIT", "SWINGMISS", "HITCENTER", "HITLEFT", "HITRIGHT",
            "HITKILL", "MISSCENTER", "MISSLEFT", "MISSRIGHT", "ACT_VM_SECONDARYATTACK" };
        for (const char* token : kStrike)
            if (ContainsNoCase(a, token))
                return Kind::Strike;
        if (meleeWeapon && (ContainsNoCase(a, "ACT_VM_PRIMARYATTACK") || ContainsNoCase(a, "ACT_VM_SHOOT")))
            return Kind::Strike;
        if (ContainsNoCase(a, "IDLE") && !ContainsNoCase(a, "HELPINGHAND") && !ContainsNoCase(a, "ITEMPICKUP"))
            return Kind::Idle;
        if (*a)
            return Kind::Other;
        // No activity: go by the sequence label.
        if (ContainsNoCase(l, "melee") || ContainsNoCase(l, "shove") || ContainsNoCase(l, "bash") ||
            (meleeWeapon && (ContainsNoCase(l, "swing") || ContainsNoCase(l, "attack"))))
            return Kind::Strike;
        return Kind::Other;
    }

    // Firing sequences of a gun (L4D2 layers its recoil animation on them).
    inline bool IsFireActivity(const char* activity)
    {
        const char* const a = activity ? activity : "";
        return ContainsNoCase(a, "PRIMARYATTACK") || ContainsNoCase(a, "ACT_VM_SHOOT") || ContainsNoCase(a, "FIRE");
    }

    // Weapon switches: the draw / deploy sequence and the item pickup reach. In
    // VR the item is simply in the hand, so none of these may move it.
    inline bool IsSwitchActivity(const char* activity, const char* label)
    {
        const char* const a = activity ? activity : "";
        const char* const l = label ? label : "";
        if (*a)
            return ContainsNoCase(a, "ACT_VM_DEPLOY") || ContainsNoCase(a, "ACT_VM_DRAW") ||
                ContainsNoCase(a, "ACT_VM_ITEMPICKUP");
        return ContainsNoCase(l, "deploy") || ContainsNoCase(l, "draw") || ContainsNoCase(l, "itempickup");
    }

    // Model path of a swung melee viewmodel ("models/weapons/melee/v_bat.mdl").
    inline bool IsSwungMeleeModel(const char* lowerModelPath)
    {
        return lowerModelPath && std::strstr(lowerModelPath, "/melee/v_") && !std::strstr(lowerModelPath, "chainsaw");
    }
}

#pragma once
#include <cstdint>

// Server capability bits advertised by l4d2vr_feature_ack <hexmask> <version>.
// A VR mechanic uses its full server-side path only when its bit is present;
// otherwise it falls back to a client-side approximation that works on any
// server. Desktop players never depend on these.
namespace l4d2vr_features
{
    constexpr std::uint32_t kVersion = 1u;
    constexpr std::uint32_t MeleeSweepV2 = 0x001u;
    constexpr std::uint32_t ShoveAim = 0x002u;
    constexpr std::uint32_t AkimboRays = 0x004u;
    constexpr std::uint32_t MagTxn = 0x008u;
    constexpr std::uint32_t ShellTxn = 0x010u;
    constexpr std::uint32_t HolsterDrop = 0x020u;
    constexpr std::uint32_t ThrowVelocity = 0x040u;
    constexpr std::uint32_t PillGive = 0x080u;
    constexpr std::uint32_t PoseV3 = 0x100u;
    constexpr std::uint32_t InstantDeploy = 0x200u;
    constexpr std::uint32_t kKnownMask = 0x3FFu;

    // What the mod's own listen-server hooks implement.
    constexpr std::uint32_t kListenHostMask =
        MeleeSweepV2 | ShoveAim | AkimboRays | MagTxn | ShellTxn | HolsterDrop | ThrowVelocity | PoseV3;

    inline bool ParseMask(const char* text, std::uint32_t& mask)
    {
        mask = 0u;
        if (!text || !*text)
            return false;
        std::uint32_t value = 0u;
        int digits = 0;
        for (const char* p = text; *p; ++p)
        {
            const char c = *p;
            std::uint32_t nibble;
            if (c >= '0' && c <= '9') nibble = static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nibble = static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
            if (++digits > 8)
                return false;
            value = (value << 4u) | nibble;
        }
        mask = value & kKnownMask;
        return true;
    }
}

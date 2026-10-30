#pragma once

// PR409 authoritative magazine/shell transactions for the dedicated server.
// Command parsing and reply formatting are byte-compatible with the listen
// host (L4D2VR/game_remote_shells.inl, game_remote_magazines.inl); the ledgers
// are the shared std-only l4d2vr_shell / l4d2vr_remote_mag classes. Native
// ammunition access is delegated to the shell through AmmoBackend.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../vr_dual_reload.h"
#include "../vr_remote_magazines.h"
#include "../vr_remote_shells.h"

namespace l4d2vr_server
{
    // Snapshot of the requesting player's active gun plus opaque write handles.
    struct AmmoState
    {
        bool eligible = false;
        std::uint32_t handle = 0u;          // client-side EHANDLE projection (12+10 bits)
        std::uint32_t latestCommand = 0u;   // newest fresh decoded VR command, else 0
        int weaponId = 0;
        int clip = 0;
        int reserve = 0;
        int capacity = 0;
        bool infinite = false;
        void* nativeWeapon = nullptr;       // shell-private
        void* nativePlayer = nullptr;       // shell-private
        int ammoType = -1;
    };

    class AmmoBackend
    {
    public:
        // Reads the player's active gun. `magazine`: detachable-magazine request
        // (else shotgun shells). Sets `unsupported` when the server cannot serve
        // the protocol at all (missing offsets etc.).
        virtual bool ReadAmmo(int playerIndex, int requestedWeaponId, bool magazine,
            AmmoState& state, bool& unsupported) = 0;
        // Writes clip/reserve if they still equal the snapshot, ends any native
        // reload and marks the entities changed. Returns false on any mismatch.
        virtual bool WriteAmmo(const AmmoState& state, int clip, int reserve, bool magazine) = 0;
        // Reads the player's active dual pistols for l4d2vr_mag_dual_v1
        // (capacity = the pair's clip, 2 x 15). Backends without it report
        // `unsupported`; WriteAmmo(..., true) writes the result.
        virtual bool ReadDualPistolAmmo(int playerIndex, AmmoState& state, bool& unsupported)
        {
            (void)playerIndex; state = AmmoState{}; unsupported = true; return false;
        }
    protected:
        ~AmmoBackend() = default;
    };

    inline bool ParseShellRequest(int argc, const char* const* argv, l4d2vr_shell::Request& request)
    {
        if (argc != 8) return false;
        std::uint32_t values[7]{};
        constexpr std::uint32_t limits[7] = {0xffffffffu, 0xffffffffu, (1u << 22) - 1u, 0x7fffffffu, 64u, 128u, 5000u};
        for (int arg = 1; arg <= 7; ++arg)
            if (!l4d2vr_shell::ParseNumber(argv[arg], limits[arg - 1], values[arg - 1])) return false;
        request = {values[0], values[1], values[2], values[3], static_cast<int>(values[4]),
            static_cast<int>(values[5]), static_cast<int>(values[6])};
        return true;
    }

    inline bool ParseMagRequest(int argc, const char* const* argv, l4d2vr_remote_mag::Request& request)
    {
        if (argc != 9) return false;
        std::uint32_t values[8]{};
        constexpr std::uint32_t limits[8] = {0xffffffffu, 0xffffffffu, (1u << 22) - 1u, 0x7fffffffu, 64u,
            static_cast<std::uint32_t>(l4d2vr_remote_mag::kMaxClip), 5000u, 2u};
        for (int arg = 1; arg <= 8; ++arg)
            if (!l4d2vr_shell::ParseNumber(argv[arg], limits[arg - 1], values[arg - 1])) return false;
        request = {values[0], values[1], values[2], values[3], static_cast<int>(values[4]),
            static_cast<int>(values[5]), static_cast<int>(values[6]), static_cast<l4d2vr_remote_mag::Action>(values[7])};
        return true;
    }

    inline bool FormatShellReply(const l4d2vr_shell::Request& r, const l4d2vr_shell::Reply& reply, char* out, std::size_t size)
    {
        const int n = std::snprintf(out, size, "l4d2vr_shell_result %u %u %u %u %d %d %d %u %d %d\n",
            r.token, r.sequence, r.handle, r.command, r.weaponId, r.clip, r.reserve,
            static_cast<unsigned>(reply.status), std::max(0, reply.clip), std::max(0, reply.reserve));
        return n > 0 && static_cast<std::size_t>(n) < size;
    }

    inline bool FormatMagReply(const l4d2vr_remote_mag::Request& r, const l4d2vr_remote_mag::Reply& reply, char* out, std::size_t size)
    {
        const int n = std::snprintf(out, size, "l4d2vr_mag_result %u %u %u %u %d %d %d %u %u %d %d\n",
            r.token, r.sequence, r.handle, r.command, r.weaponId, r.clip, r.reserve,
            static_cast<unsigned>(r.action), static_cast<unsigned>(reply.status),
            std::max(0, reply.clip), std::max(0, reply.reserve));
        return n > 0 && static_cast<std::size_t>(n) < size;
    }

    inline bool FormatShellCapability(std::uint32_t token, char* out, std::size_t size)
    {
        const int n = std::snprintf(out, size, "l4d2vr_shell_capability %u %u\n", l4d2vr_shell::kVersion, token);
        return n > 0 && static_cast<std::size_t>(n) < size;
    }

    inline bool FormatMagCapability(std::uint32_t token, char* out, std::size_t size)
    {
        const int n = std::snprintf(out, size, "l4d2vr_mag_capability %u %u\n", l4d2vr_remote_mag::kVersion, token);
        return n > 0 && static_cast<std::size_t>(n) < size;
    }

    // Per-client transaction ledgers bound to one connection (opaque entity).
    struct TransactionClient
    {
        const void* entity = nullptr;
        l4d2vr_shell::ServerLedger shell{};
        l4d2vr_remote_mag::ServerLedger mag{};
        l4d2vr_dual_reload::ServerLedger dual{};    // shares the magazine token
    };

    // Applies one shell insertion. Mirrors Game::HandleRemoteShellCommand.
    inline l4d2vr_shell::Reply ApplyShellRequest(TransactionClient& client, AmmoBackend& backend, int playerIndex,
        const l4d2vr_shell::Request& request, std::uint64_t nowMs)
    {
        AmmoState native{};
        bool unsupported = false;
        backend.ReadAmmo(playerIndex, request.weaponId, false, native, unsupported);
        if (unsupported)
            return client.shell.Unsupported(request);
        const l4d2vr_shell::Snapshot snapshot{native.eligible, native.handle, native.latestCommand,
            native.weaponId, native.clip, native.reserve, native.capacity};
        return client.shell.Apply(request, snapshot, nowMs,
            [&](int clip, int reserve) { return backend.WriteAmmo(native, clip, reserve, false); });
    }

    // Applies one magazine action. Mirrors Game::HandleRemoteMagazineCommand.
    inline l4d2vr_remote_mag::Reply ApplyMagRequest(TransactionClient& client, AmmoBackend& backend, int playerIndex,
        const l4d2vr_remote_mag::Request& request, std::uint64_t nowMs)
    {
        AmmoState native{};
        bool unsupported = false;
        backend.ReadAmmo(playerIndex, request.weaponId, true, native, unsupported);
        if (unsupported)
            return client.mag.Unsupported(request);
        const l4d2vr_remote_mag::Snapshot snapshot{native.eligible, native.handle, native.latestCommand,
            native.weaponId, native.clip, native.reserve, native.capacity, native.infinite};
        return client.mag.Apply(request, snapshot, nowMs,
            [&](int clip, int reserve) { return backend.WriteAmmo(native, clip, reserve, true); });
    }

    // Applies one physical dual-pistol reload (vr_dual_reload.h). Mirrors
    // Game::HandleDualMagazineCommand on the listen host.
    inline l4d2vr_dual_reload::Reply ApplyDualRequest(TransactionClient& client, AmmoBackend& backend, int playerIndex,
        const l4d2vr_dual_reload::Request& request, std::uint64_t nowMs)
    {
        if (client.dual.Token() != client.mag.Token()) client.dual.Reset(client.mag.Token());
        AmmoState native{};
        bool unsupported = false;
        backend.ReadDualPistolAmmo(playerIndex, native, unsupported);
        if (unsupported)
            return client.dual.Unsupported(request);
        const l4d2vr_dual_reload::Snapshot snapshot{native.eligible, native.handle, native.latestCommand,
            native.clip, native.reserve, native.capacity, native.infinite};
        return client.dual.Apply(request, snapshot, nowMs,
            [&](int clip, int reserve) { return backend.WriteAmmo(native, clip, reserve, true); });
    }
}

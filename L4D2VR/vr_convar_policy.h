#pragma once

// 2.0.13: the mod writes game cvars directly (ConVar::SetValue), which skips
// the console's sv_cheats check. It must never write a cheat-protected or a
// server-replicated cvar that way: those belong to the server (sv_cheats) and
// a client-side write would be a cheat or a prediction desync. Every
// Game::SetConVar* call goes through this rule. std-only (tested in
// tests/vr_gameplay_mode_tests.cpp).

namespace l4d2vr_convar_policy
{
    constexpr int kFcvarReplicated = 1 << 13;   // FCVAR_REPLICATED
    constexpr int kFcvarCheat = 1 << 14;        // FCVAR_CHEAT
    constexpr int kServerOwnedFlags = kFcvarReplicated | kFcvarCheat;

    // flags < 0: the flags could not be read; refuse.
    inline bool ModMayWrite(int flags)
    {
        return flags >= 0 && (flags & kServerOwnedFlags) == 0;
    }

    // SetConVarFlags may only drop flags such as FCVAR_ARCHIVE: the server-owned
    // bits always stay as they are.
    inline int KeepServerOwnedFlags(int currentFlags, int requestedFlags)
    {
        return (requestedFlags & ~kServerOwnedFlags) | (currentFlags & kServerOwnedFlags);
    }
}

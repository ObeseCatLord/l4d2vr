#pragma once

// Gameplay mode (2.0.13): Physical (the full L4D2VR mechanics) or Classic
// (keyou91-style simple controls: button reload with the native auto reload,
// button shove and melee, native spread with the spread crosshair, holsters as
// weapon selectors with the stock deploy, no akimbo, no recoil system, no
// gestures or hand throws).
//
// GameplayMode=auto|physical|classic (config.txt). Auto plays Physical on a
// server that acknowledged the L4D2VR plugin (l4d2vr_server_ack) and on a game
// this process hosts (listen server / local game), Classic everywhere else.
// Until the acknowledgement arrives the client plays Classic (PendingClassic),
// so a plain server only ever sees stock usercmds and stock commands.
// Physical forced on a server without the plugin still plays Classic: VR data
// never goes to a server that did not ask for it.
//
// The wire policy says what may leave the client in each mode. Classic sends
// stock usercmds (no VR encoding, no protocol impulses 234..246, no marker bits
// 26..31) and no l4d2vr_* command, except the pose hello that answers a
// server-initiated ack and the relay features the server itself offers
// (pose relay, Opus voice: the feature bits of a partial plugin).
//
// std-only, unit tested (tests/vr_gameplay_mode_tests.cpp).

#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>

namespace l4d2vr_gameplay_mode
{
    enum class Config : std::uint8_t { Auto, Physical, Classic };
    enum class Effective : std::uint8_t
    {
        Vanilla,         // no VR (desktop / receive-only): the game's own input, unchanged
        Physical,        // the full mechanics (server acknowledged, or this process hosts the game)
        Classic,         // simple controls; the server never acknowledged
        PendingClassic,  // in a game, waiting for an acknowledgement: plays Classic
    };
    enum class Reason : std::uint8_t
    {
        Desktop,
        ForcedClassic,
        ServerAck,
        ListenHost,
        AwaitingAck,
        NoAck,
        ForcedPhysicalWithoutAck,
        NotInGame,
    };

    inline const char* ConfigName(Config c)
    {
        switch (c)
        {
        case Config::Physical: return "physical";
        case Config::Classic: return "classic";
        default: return "auto";
        }
    }

    inline const char* EffectiveName(Effective e)
    {
        switch (e)
        {
        case Effective::Physical: return "physical";
        case Effective::Classic: return "classic";
        case Effective::PendingClassic: return "classic (waiting for a server acknowledgement)";
        default: return "vanilla (no VR)";
        }
    }

    inline const char* ReasonName(Reason r)
    {
        switch (r)
        {
        case Reason::Desktop: return "desktop";
        case Reason::ForcedClassic: return "GameplayMode=classic";
        case Reason::ServerAck: return "server has the L4D2VR plugin";
        case Reason::ListenHost: return "this game hosts the server";
        case Reason::AwaitingAck: return "waiting for the server";
        case Reason::NoAck: return "server without the L4D2VR plugin";
        case Reason::ForcedPhysicalWithoutAck: return "GameplayMode=physical but the server has no L4D2VR plugin";
        case Reason::NotInGame: return "not in a game";
        default: return "?";
        }
    }

    // "auto" / "physical" / "classic", any case, surrounding blanks ignored.
    inline bool ParseConfig(const std::string& text, Config& out)
    {
        std::string t;
        for (char ch : text)
            if (!std::isspace(static_cast<unsigned char>(ch)))
                t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        if (t == "auto") { out = Config::Auto; return true; }
        if (t == "physical") { out = Config::Physical; return true; }
        if (t == "classic") { out = Config::Classic; return true; }
        return false;
    }

    struct Inputs
    {
        Config config = Config::Auto;
        bool vrActive = false;      // VR::m_IsVREnabled (false: desktop, -nohmd, receive-only)
        bool inGame = false;
        bool listenHost = false;    // this process runs the server (listen / local game)
        bool serverAck = false;     // l4d2vr_server_ack received this session
        bool graceExpired = false;  // the acknowledgement window after joining has passed
    };

    struct Result
    {
        Effective effective = Effective::Vanilla;
        Reason reason = Reason::Desktop;
    };

    inline Result Resolve(const Inputs& in)
    {
        if (!in.vrActive) return { Effective::Vanilla, Reason::Desktop };
        if (in.config == Config::Classic) return { Effective::Classic, Reason::ForcedClassic };
        if (in.listenHost) return { Effective::Physical, Reason::ListenHost };
        if (in.serverAck) return { Effective::Physical, Reason::ServerAck };
        if (!in.inGame) return { Effective::PendingClassic, Reason::NotInGame };
        if (!in.graceExpired) return { Effective::PendingClassic, Reason::AwaitingAck };
        return { Effective::Classic,
            in.config == Config::Physical ? Reason::ForcedPhysicalWithoutAck : Reason::NoAck };
    }

    inline bool ClassicLike(Effective e) { return e == Effective::Classic || e == Effective::PendingClassic; }

    // What may leave the client.
    struct WirePolicy
    {
        bool encodeUsercmd = false;     // VR usercmd encoding (tick_count sign, packed pose)
        bool protocolImpulses = false;  // impulses 234..246 and the marker bits 26..31
        bool gameplayCommands = false;  // l4d2vr_* transactions (shell, magazine, pistol, feature hello, shotgun pattern)
        bool poseRelay = false;         // l4d2vr_pose_upload (server relays poses)
        bool opusVoice = false;         // l4d2vr_voice_* (server offers OpusVoice)
    };

    constexpr std::uint32_t kOpusVoiceFeature = 0x2000u;   // l4d2vr_features::OpusVoice

    inline WirePolicy PolicyFor(Effective e, std::uint32_t serverFeatures, bool poseServerCapable)
    {
        WirePolicy p{};
        // Relay features the server offers itself (also a partial plugin):
        // in every mode.
        p.poseRelay = poseServerCapable;
        p.opusVoice = (serverFeatures & kOpusVoiceFeature) != 0u;
        // Desktop clients keep their behaviour (they send no VR gameplay data;
        // the existing gates decide).
        const bool full = e == Effective::Physical || e == Effective::Vanilla;
        p.encodeUsercmd = full;
        p.protocolImpulses = full;
        p.gameplayCommands = full;
        return p;
    }

    // Impulses only the L4D2VR plugin understands (vr_server_protocol.h).
    constexpr int kFirstProtocolImpulse = 234;
    constexpr int kLastProtocolImpulse = 246;
    constexpr std::uint32_t kProtocolButtonBits = 0x3fu << 26;

    inline bool IsProtocolImpulse(int impulse)
    {
        const int value = impulse & 0xff;
        return value >= kFirstProtocolImpulse && value <= kLastProtocolImpulse;
    }

    // Final usercmd guard: with no protocol impulses allowed, a protocol impulse
    // becomes 0 and the marker bits are cleared. Returns true when it changed
    // anything (the caller logs it: the gate should never have to act).
    inline bool SanitizeStockUsercmd(const WirePolicy& policy, int& impulse, int& buttons)
    {
        if (policy.protocolImpulses) return false;
        bool changed = false;
        if (IsProtocolImpulse(impulse)) { impulse = 0; changed = true; }
        const std::uint32_t b = static_cast<std::uint32_t>(buttons);
        if ((b & kProtocolButtonBits) != 0u)
        {
            buttons = static_cast<int>(b & ~kProtocolButtonBits);
            changed = true;
        }
        return changed;
    }

    // A command for the server. Classic keeps everything except the l4d2vr_*
    // transactions; the pose hello is the answer to a server-initiated ack.
    inline bool AllowServerCommand(const WirePolicy& policy, const char* text)
    {
        if (!text) return false;
        while (*text == ' ' || *text == '\t') ++text;
        static const char kPrefix[] = "l4d2vr_";
        if (std::strncmp(text, kPrefix, sizeof(kPrefix) - 1) != 0) return true;
        auto is = [&](const char* name) {
            const std::size_t n = std::strlen(name);
            return std::strncmp(text, name, n) == 0 && (text[n] == '\0' || text[n] == ' ' || text[n] == '\t');
        };
        auto starts = [&](const char* prefix) { return std::strncmp(text, prefix, std::strlen(prefix)) == 0; };
        if (is("l4d2vr_pose_hello")) return true;
        if (is("l4d2vr_pose_upload")) return policy.poseRelay;
        if (starts("l4d2vr_voice_")) return policy.opusVoice;
        return policy.gameplayCommands;
    }
}

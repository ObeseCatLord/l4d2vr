#pragma once

// Opus voice relay (OpusVoice, 0x2000), std-only: used by the dedicated
// plugin's Session (Linux and Windows) and by the modded listen host
// (game.cpp). The server never decodes Opus; it validates each frame, applies
// a per-talker rate limit and forwards the frame text unchanged to every mod
// client that opted in and that the game lets hear the talker (team voice
// rules, alltalk and player mutes, read through IVoiceServer). See
// vr_opus_voice.h for the wire format.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../vr_opus_voice.h"

namespace l4d2vr_voice_server
{
    using namespace l4d2vr_voice;

    class RelayHost
    {
    public:
        // The recipient slot holds a connected client the relay may address.
        virtual bool Connected(int index) const = 0;
        // Reliable server -> client command; `text` ends with '\n'.
        virtual void Send(int index, const char* text) = 0;
        // IVoiceServer::GetClientListening(receiver, sender), 1-based.
        virtual bool CanHear(int receiver, int sender) const = 0;
        virtual std::uint64_t NowMs() const = 0;
    protected:
        ~RelayHost() = default;
    };

    struct RelayClient
    {
        bool hello = false;
        std::uint32_t flags = 0u;
        double tokens = 0.0;
        std::uint64_t refillMs = 0u;
        std::uint32_t relayedFrames = 0u;   // frames this talker sent that were forwarded
        std::uint32_t rejectedFrames = 0u;  // malformed or over the rate limit
    };

    class Relay
    {
    public:
        static constexpr double kFramesPerSecond = 60.0;   // 50 + slack for bursts after a hitch
        static constexpr double kBurstFrames = 30.0;

        static bool IsVoiceCommand(const char* name)
        {
            return name && std::strncmp(name, "l4d2vr_voice_", 13) == 0;
        }

        void SetEnabled(bool enabled) { m_Enabled = enabled; }
        bool Enabled() const { return m_Enabled; }
        void SetClientMax(int clientMax) { m_ClientMax = (clientMax >= 1 && clientMax <= kMaxPlayers) ? clientMax : kMaxPlayers; }

        void ResetAll() { m_Clients.fill(RelayClient{}); m_LastTalkers = 0u; }

        // A slot was reset (disconnect, reconnect, edict reuse). Tells the
        // remaining Opus clients when a talker left.
        void Reset(RelayHost& host, int index)
        {
            if (!Valid(index)) return;
            m_Clients[static_cast<std::size_t>(index)] = RelayClient{};
            BroadcastPeersIfChanged(host);
        }

        const RelayClient* Client(int index) const { return Valid(index) ? &m_Clients[static_cast<std::size_t>(index)] : nullptr; }

        // Players whose Opus voice reaches others (stock voice is dropped for them).
        std::uint64_t TalkerMask() const
        {
            std::uint64_t mask = 0u;
            for (int i = 1; i <= m_ClientMax; ++i)
            {
                const RelayClient& c = m_Clients[static_cast<std::size_t>(i)];
                if (c.hello && (c.flags & kHelloTransmit)) mask |= PlayerBit(i);
            }
            return mask;
        }

        // Handles every l4d2vr_voice_* command (returns false for others).
        bool ClientCommand(RelayHost& host, int index, int argc, const char* const* argv)
        {
            if (argc <= 0 || !argv || !IsVoiceCommand(argv[0])) return false;
            if (!m_Enabled || !Valid(index)) return true;
            if (std::strcmp(argv[0], kHelloCommand) == 0) { HandleHello(host, index, argc, argv); return true; }
            if (std::strcmp(argv[0], kUpCommand) == 0) { HandleUp(host, index, argc, argv); return true; }
            return true;
        }

        std::uint32_t RelayedTotal() const { return m_RelayedTotal; }
        std::uint32_t DeliveredTotal() const { return m_DeliveredTotal; }

    private:
        bool Valid(int index) const { return index >= 1 && index <= kMaxPlayers && index <= m_ClientMax; }

        void HandleHello(RelayHost& host, int index, int argc, const char* const* argv)
        {
            std::uint32_t version = 0u, flags = 0u;
            if (argc != 3 || !ParseUnsigned(argv[1], 255u, version) || version < kProtocolVersion ||
                !ParseUnsigned(argv[2], 255u, flags))
                return;
            RelayClient& c = m_Clients[static_cast<std::size_t>(index)];
            flags &= kHelloMask;
            if (flags == 0u)
            {
                c = RelayClient{};
                BroadcastPeersIfChanged(host);
                return;
            }
            const bool first = !c.hello;
            c.hello = true;
            c.flags = flags;
            if (first)
            {
                c.tokens = kBurstFrames;
                c.refillMs = host.NowMs();
            }
            char line[48];
            std::snprintf(line, sizeof(line), "%s %u\n", kAckCommand, kProtocolVersion);
            host.Send(index, line);
            // The new client learns the talkers; everyone else when they changed.
            if (!BroadcastPeersIfChanged(host) && (flags & kHelloReceive))
                SendPeers(host, index);
        }

        void HandleUp(RelayHost& host, int index, int argc, const char* const* argv)
        {
            RelayClient& c = m_Clients[static_cast<std::size_t>(index)];
            if (!c.hello || !(c.flags & (kHelloTransmit | kHelloLoopback)) || argc != 2) return;
            // Token bucket: kFramesPerSecond sustained, kBurstFrames burst.
            const std::uint64_t now = host.NowMs();
            if (now > c.refillMs)
            {
                c.tokens = std::min(kBurstFrames, c.tokens + double(now - c.refillMs) * kFramesPerSecond / 1000.0);
                c.refillMs = now;
            }
            Frame frame;
            if (c.tokens < 1.0 || !DecodeFrameText(argv[1], frame)) { ++c.rejectedFrames; return; }
            c.tokens -= 1.0;
            char line[48 + kMaxFrameTextChars];
            const int n = std::snprintf(line, sizeof(line), "%s %d %s\n", kRxCommand, index, argv[1]);
            if (n <= 0 || n >= static_cast<int>(sizeof(line))) { ++c.rejectedFrames; return; }
            ++c.relayedFrames;
            ++m_RelayedTotal;
            for (int r = 1; r <= m_ClientMax; ++r)
            {
                const RelayClient& rc = m_Clients[static_cast<std::size_t>(r)];
                if (!rc.hello || !(rc.flags & kHelloReceive) || !host.Connected(r)) continue;
                if (r == index ? !(c.flags & kHelloLoopback) : !host.CanHear(r, index)) continue;
                host.Send(r, line);
                ++m_DeliveredTotal;
            }
        }

        void SendPeers(RelayHost& host, int index)
        {
            char line[64];
            if (FormatPeers(m_LastTalkers, line, sizeof(line))) host.Send(index, line);
        }

        bool BroadcastPeersIfChanged(RelayHost& host)
        {
            const std::uint64_t talkers = TalkerMask();
            if (talkers == m_LastTalkers) return false;
            m_LastTalkers = talkers;
            for (int r = 1; r <= m_ClientMax; ++r)
            {
                const RelayClient& rc = m_Clients[static_cast<std::size_t>(r)];
                if (rc.hello && (rc.flags & kHelloReceive) && host.Connected(r)) SendPeers(host, r);
            }
            return true;
        }

        std::array<RelayClient, kMaxPlayers + 1> m_Clients{};
        std::uint64_t m_LastTalkers = 0u;
        int m_ClientMax = kMaxPlayers;
        bool m_Enabled = false;
        std::uint32_t m_RelayedTotal = 0u, m_DeliveredTotal = 0u;
    };
}

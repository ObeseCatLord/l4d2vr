#pragma once

// Opus voice relay (OpusVoice, 0x2000), std-only: used by the dedicated
// plugin's Session (Linux and Windows) and by the modded listen host
// (game.cpp). The server never decodes Opus; it validates each frame, applies
// a per-talker rate limit and forwards the frame text unchanged to every mod
// client that opted in and may hear the talker. See vr_opus_voice.h for the
// wire format.
//
// Who hears whom (2.0.12). 2.0.5-2.0.11 used the game's voice mask
// (IVoiceServer::GetClientListening) alone. That mask is client-driven state:
// a receiver's row is only built while its client answered the voice
// manager's RequestState with "VModEnable 1", so a client that has not (or
// stopped) leaves the row all-false, and the relay then silently delivered
// nothing for the rest of the session (2.0.11, Marmot: 57 of 61 minutes). Now
// the relay owns the rule:
//   * the stock bit says yes                      -> heard;
//   * different teams (no alltalk), and the receiver is not a spectator
//                                                 -> not heard;
//   * the receiver's stock row hears somebody else -> the row is in use and
//     this bit is a deliberate mute (vban)        -> not heard;
//   * otherwise (the row is unused)               -> heard.
// Every change of who cannot hear a talker is logged with the reason, the
// teams, sv_alltalk and the receiver's raw row, and an outage is repeated
// every 30 s while it lasts.
//
// Downlink (2.0.12, relay protocol 2): clients that hello with protocol 2 on a
// host with an unreliable channel get the frames queued and sent once per
// server frame (Flush), packed up to kMaxRxFramesPerCommand per line, in the
// net channel's unreliable stream, at most kMaxUnreliableBytesPerFlush per
// listener, with the talker's previous frame repeated when it fits
// (redundancy). Protocol 1 clients (2.0.10/2.0.11) keep one reliable command
// per frame, now dropped while the listener's reliable stream already holds
// kMaxReliableBacklogBytes (the client's command buffer overflowed on the
// burst after a stall).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../vr_opus_voice.h"

namespace l4d2vr_voice_server
{
    using namespace l4d2vr_voice;

    constexpr int kTeamUnknown = -1;   // empty slot, or the host cannot read teams
    constexpr int kTeamUnassigned = 0;
    constexpr int kTeamSpectator = 1;
    constexpr int kTeamSurvivor = 2;
    constexpr int kTeamInfected = 3;

    class RelayHost
    {
    public:
        // The recipient slot holds a connected client the relay may address.
        virtual bool Connected(int index) const = 0;
        // Reliable server -> client command; `text` ends with '\n'.
        virtual void Send(int index, const char* text) = 0;
        // IVoiceServer::GetClientListening(receiver, sender), 1-based: the
        // game's own voice mask (client-driven; see the rule above).
        virtual bool CanHear(int receiver, int sender) const = 0;
        virtual std::uint64_t NowMs() const = 0;
        virtual void Log(const char* line) { (void)line; }
        // 2.0.12. Unreliable server -> client command (net_StringCmd in the
        // listener's unreliable stream); false = not sent.
        virtual bool SendUnreliable(int index, const char* text) { (void)index; (void)text; return false; }
        // This listener can be sent unreliable commands (its net channel was
        // found and checked).
        virtual bool UnreliableAvailable(int index) { (void)index; return false; }
        // Bytes waiting in the listener's reliable stream (-1 = unknown).
        virtual int PendingReliableBytes(int index) const { (void)index; return -1; }
        // Team of the player in the slot (bots too); kTeamUnknown = empty or unreadable.
        virtual int Team(int index) const { (void)index; return kTeamUnknown; }
        // sv_alltalk (-1 = unknown).
        virtual int AllTalk() const { return -1; }
    protected:
        ~RelayHost() = default;
    };

    enum class Hearing : std::uint8_t
    {
        Stock,       // the game's mask says yes
        OwnRule,     // the game's mask says no, but its row for the receiver is unused
        OtherTeam,   // different teams, no alltalk
        Muted,       // the receiver's row is in use and leaves this talker out (vban)
    };

    inline bool Hears(Hearing h) { return h == Hearing::Stock || h == Hearing::OwnRule; }

    inline const char* HearingName(Hearing h)
    {
        switch (h)
        {
        case Hearing::Stock: return "game voice mask";
        case Hearing::OwnRule: return "relay rule (game voice mask unused for this receiver)";
        case Hearing::OtherTeam: return "other team (sv_alltalk 0)";
        default: return "muted by the receiver (game voice mask)";
        }
    }

    // stock: GetClientListening(receiver, sender). Teams kTeamUnknown when not
    // readable, allTalk -1 when unknown. rowInUse: the receiver's stock row
    // hears at least one other player.
    inline Hearing DecideHearing(bool stock, int receiverTeam, int senderTeam, int allTalk, bool rowInUse)
    {
        if (stock) return Hearing::Stock;
        const bool receiverSpectates = receiverTeam == kTeamSpectator || receiverTeam == kTeamUnassigned;
        if (receiverTeam >= 0 && senderTeam >= 0 && allTalk <= 0 && receiverTeam != senderTeam && !receiverSpectates)
            return Hearing::OtherTeam;
        return rowInUse ? Hearing::Muted : Hearing::OwnRule;
    }

    struct QueuedFrame
    {
        std::uint64_t ms = 0u;
        std::uint64_t listeners = 0u;   // PlayerBit of every protocol-2 listener
        char text[kMaxFrameTextChars + 1] = {};
    };

    struct RelayClient
    {
        bool hello = false;
        std::uint32_t flags = 0u;
        std::uint32_t version = 1u;         // relay protocol of the hello (2.0.12)
        bool unreliable = false;            // acked protocol 2: packed lines in the unreliable stream
        // Rate limits (2.0.11): kept across an opt-out hello (flags 0), so
        // "hello 0, hello 3" no longer refills the burst; only a slot reset
        // (disconnect, new occupant, map change) clears them.
        bool bucketStarted = false;
        double tokens = 0.0;
        std::uint64_t refillMs = 0u;
        double helloTokens = 0.0;           // hellos per second (excess ignored)
        std::uint64_t helloRefillMs = 0u;
        std::uint32_t flagLogs = 0u;        // "Opus voice hello (flags ..)" lines written
        double rxTokens = 0.0;              // frames this listener may still receive (per-listener budget)
        std::uint64_t rxRefillMs = 0u;
        std::uint32_t rxDropped = 0u;       // frames not sent to this listener (over its budget)
        std::uint32_t backlogDropped = 0u;  // reliable frames not sent: the listener's reliable stream is backed up
        std::uint32_t unreliableLines = 0u, unreliableFailed = 0u;
        std::uint32_t relayedFrames = 0u;   // frames this talker sent that were forwarded
        std::uint32_t rejectedFrames = 0u;  // malformed or over the rate limit
        std::uint32_t deliveredFrames = 0u; // copies of this talker's frames sent to listeners (2.0.10 log)
        std::uint32_t ownRuleCopies = 0u;   // ... of them while the game's mask said no (2.0.12)
        std::uint32_t redundantCopies = 0u;
        // Diagnostics (2.0.12): who cannot hear this talker, and since when.
        std::uint64_t blockedMask = 0u;
        std::uint64_t outageLogMs = 0u;
        std::uint32_t outageFrames = 0u;    // frames nobody heard since the last outage line
        std::uint32_t rejectedLogged = 0u;
        std::uint64_t rejectLogMs = 0u;
        // Talker queue for protocol-2 listeners (flushed once per server frame).
        std::vector<QueuedFrame> queue;
        QueuedFrame lastFlushed{};
        bool haveLastFlushed = false;
    };

    class Relay
    {
    public:
        static constexpr double kFramesPerSecond = 60.0;   // 50 + slack for bursts after a hitch
        // 2.0.12: two seconds of voice (was 50 frames: 912 of 31500 frames of one
        // talker were cut after client hitches on Marmot).
        static constexpr double kBurstFrames = 100.0;
        // 2.0.11: hellos (1 per second, burst 4; clients retry every 1.5 s)
        // and what one listener receives: N talkers would be N x 50 frames/s to
        // each listener. Five talkers at full rate fit; beyond that frames are
        // dropped (the jitter buffer conceals them).
        static constexpr double kHellosPerSecond = 1.0;
        static constexpr double kHelloBurst = 4.0;
        static constexpr std::uint32_t kMaxFlagLogs = 6u;
        static constexpr double kListenerFramesPerSecond = 250.0;
        static constexpr double kListenerBurstFrames = 250.0;
        // 2.0.12 transport limits (see the header comment).
        static constexpr int kMaxReliableBacklogBytes = 1536;
        static constexpr int kMaxUnreliableBytesPerFlush = 1000;
        static constexpr std::size_t kMaxQueuedFrames = 12u;
        static constexpr std::uint64_t kMaxQueueMs = 60u;          // flushed from the command path when older
        static constexpr std::uint64_t kRedundancyMaxAgeMs = 100u;
        static constexpr std::uint64_t kOutageRepeatMs = 30000u;
        static constexpr std::uint64_t kRejectLogMs = 30000u;
        static constexpr std::uint64_t kRuleCacheMs = 1000u;

        static bool IsVoiceCommand(const char* name)
        {
            return name && std::strncmp(name, "l4d2vr_voice_", 13) == 0;
        }

        void SetEnabled(bool enabled) { m_Enabled = enabled; }
        bool Enabled() const { return m_Enabled; }
        void SetClientMax(int clientMax) { m_ClientMax = (clientMax >= 1 && clientMax <= kMaxPlayers) ? clientMax : kMaxPlayers; }

        void ResetAll()
        {
            for (RelayClient& c : m_Clients) c = RelayClient{};
            m_LastTalkers = 0u;
            m_CacheValid = false;
        }

        // A slot was reset (disconnect, reconnect, edict reuse). Tells the
        // remaining Opus clients when a talker left.
        void Reset(RelayHost& host, int index)
        {
            if (!Valid(index)) return;
            m_Clients[static_cast<std::size_t>(index)] = RelayClient{};
            m_CacheValid = false;
            BroadcastPeersIfChanged(host);
        }

        // Slots whose client left (the listen host has no disconnect hook):
        // reset them so the remaining Opus clients learn the talker is gone.
        void PruneDisconnected(RelayHost& host)
        {
            for (int i = 1; i <= m_ClientMax; ++i)
            {
                RelayClient& c = m_Clients[static_cast<std::size_t>(i)];
                if ((c.hello || c.bucketStarted) && !host.Connected(i)) c = RelayClient{};
            }
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

        // Once per server frame (plugin GameFrame, listen host client frame):
        // sends the queued frames to protocol-2 listeners.
        void Flush(RelayHost& host)
        {
            if (!m_Enabled) return;
            bool any = false;
            for (int t = 1; t <= m_ClientMax && !any; ++t) any = !m_Clients[static_cast<std::size_t>(t)].queue.empty();
            if (!any) return;
            const std::uint64_t now = host.NowMs();
            std::array<int, kMaxPlayers + 1> spent{};
            for (int t = 1; t <= m_ClientMax; ++t)
            {
                RelayClient& c = m_Clients[static_cast<std::size_t>(t)];
                if (c.queue.empty()) continue;
                std::uint64_t all = 0u;
                for (const QueuedFrame& q : c.queue) all |= q.listeners;
                for (int r = 1; r <= m_ClientMax; ++r)
                {
                    if (!(all & PlayerBit(r)) || !host.Connected(r)) continue;
                    FlushTo(host, t, c, r, now, spent[static_cast<std::size_t>(r)]);
                }
                c.lastFlushed = c.queue.back();
                c.lastFlushed.ms = now;
                c.haveLastFlushed = true;
                c.queue.clear();
            }
        }

        std::uint32_t RelayedTotal() const { return m_RelayedTotal; }
        std::uint32_t DeliveredTotal() const { return m_DeliveredTotal; }

        // The verdict for one pair, as the relay applies it (tests, diagnostics).
        Hearing Evaluate(RelayHost& host, int receiver, int sender)
        {
            RefreshCache(host);
            const bool stock = host.CanHear(receiver, sender);
            if (stock) return Hearing::Stock;
            return DecideHearing(false, TeamOf(receiver), TeamOf(sender), m_AllTalk, RowInUse(host, receiver));
        }

    private:
        bool Valid(int index) const { return index >= 1 && index <= kMaxPlayers && index <= m_ClientMax; }

        int TeamOf(int index) const { return (index >= 1 && index <= kMaxPlayers) ? m_Team[static_cast<std::size_t>(index)] : kTeamUnknown; }

        void RefreshCache(RelayHost& host)
        {
            const std::uint64_t now = host.NowMs();
            if (m_CacheValid && now >= m_CacheMs && now - m_CacheMs < kRuleCacheMs) return;
            m_CacheValid = true;
            m_CacheMs = now;
            m_Team.fill(kTeamUnknown);
            for (int i = 1; i <= m_ClientMax; ++i) m_Team[static_cast<std::size_t>(i)] = host.Team(i);
            m_AllTalk = host.AllTalk();
            m_RowInUse.fill(-1);
        }

        bool Occupied(const RelayHost& host, int index) const { return TeamOf(index) >= 0 || host.Connected(index); }

        // The receiver's row of the game's voice mask hears another player.
        bool RowInUse(RelayHost& host, int receiver)
        {
            std::int8_t& cached = m_RowInUse[static_cast<std::size_t>(receiver)];
            if (cached >= 0) return cached != 0;
            bool inUse = false;
            for (int h = 1; h <= m_ClientMax && !inUse; ++h)
                if (h != receiver && Occupied(host, h) && host.CanHear(receiver, h)) inUse = true;
            cached = inUse ? 1 : 0;
            return inUse;
        }

        std::uint64_t StockRow(RelayHost& host, int receiver) const
        {
            std::uint64_t row = 0u;
            for (int h = 1; h <= m_ClientMax; ++h)
                if (h != receiver && Occupied(host, h) && host.CanHear(receiver, h)) row |= PlayerBit(h);
            return row;
        }

        void HandleHello(RelayHost& host, int index, int argc, const char* const* argv)
        {
            std::uint32_t version = 0u, flags = 0u;
            if (argc != 3 || !ParseUnsigned(argv[1], 255u, version) || version < kProtocolVersion ||
                !ParseUnsigned(argv[2], 255u, flags))
                return;
            RelayClient& c = m_Clients[static_cast<std::size_t>(index)];
            const std::uint64_t now = host.NowMs();
            if (!c.bucketStarted)
            {
                c.bucketStarted = true;
                c.tokens = kBurstFrames;
                c.refillMs = now;
                c.helloTokens = kHelloBurst;
                c.helloRefillMs = now;
                c.rxTokens = kListenerBurstFrames;
                c.rxRefillMs = now;
            }
            if (now > c.helloRefillMs)
            {
                c.helloTokens = std::min(kHelloBurst, c.helloTokens + double(now - c.helloRefillMs) * kHellosPerSecond / 1000.0);
                c.helloRefillMs = now;
            }
            if (c.helloTokens < 1.0) return;   // hello flood: ignored (the client retries)
            c.helloTokens -= 1.0;
            flags &= kHelloMask;
            if (flags == 0u)
            {
                // Opt out: the session ends, the rate limits stay.
                c.hello = false;
                c.flags = 0u;
                c.queue.clear();
                BroadcastPeersIfChanged(host);
                return;
            }
            // Protocol 2 only where this listener's unreliable channel works.
            const std::uint32_t wanted = std::min(version, kRelayProtocolVersion);
            const std::uint32_t acked = (wanted >= 2u && host.UnreliableAvailable(index)) ? 2u : 1u;
            const bool first = !c.hello;
            if ((first || c.flags != flags || c.version != acked) && c.flagLogs < kMaxFlagLogs)
            {
                ++c.flagLogs;
                char log[192];
                std::snprintf(log, sizeof(log), "L4D2VR server: player %d Opus voice hello (flags %u, protocol %u): downlink %s\n", index,
                    static_cast<unsigned>(flags), static_cast<unsigned>(version),
                    acked >= 2u ? "packed, unreliable stream" : (wanted >= 2u ? "reliable (unreliable channel unavailable)" : "reliable (protocol 1 client)"));
                host.Log(log);
            }
            c.hello = true;
            c.flags = flags;
            c.version = acked;
            c.unreliable = acked >= 2u;
            char line[48];
            std::snprintf(line, sizeof(line), "%s %u\n", kAckCommand, static_cast<unsigned>(acked));
            host.Send(index, line);
            // The new client learns the talkers; everyone else when they changed.
            if (!BroadcastPeersIfChanged(host) && (flags & kHelloReceive))
                SendPeers(host, index);
        }

        void LogRejects(RelayHost& host, int index, RelayClient& c, std::uint64_t now)
        {
            if (c.rejectedFrames == c.rejectedLogged || now - c.rejectLogMs < kRejectLogMs) return;
            char log[192];
            std::snprintf(log, sizeof(log), "L4D2VR server: player %d Opus voice: %u frame(s) rejected in the last %.0f s (rate limit or malformed; %u in total)\n",
                index, static_cast<unsigned>(c.rejectedFrames - c.rejectedLogged),
                c.rejectLogMs ? double(now - c.rejectLogMs) / 1000.0 : 30.0, static_cast<unsigned>(c.rejectedFrames));
            host.Log(log);
            c.rejectedLogged = c.rejectedFrames;
            c.rejectLogMs = now;
        }

        void HandleUp(RelayHost& host, int index, int argc, const char* const* argv)
        {
            RelayClient& c = m_Clients[static_cast<std::size_t>(index)];
            if (!c.hello || !(c.flags & (kHelloTransmit | kHelloLoopback)) || argc != 2) return;
            // Token bucket: kFramesPerSecond sustained, kBurstFrames burst.
            const std::uint64_t now = host.NowMs();
            if (!c.rejectLogMs) c.rejectLogMs = now;
            if (now > c.refillMs)
            {
                c.tokens = std::min(kBurstFrames, c.tokens + double(now - c.refillMs) * kFramesPerSecond / 1000.0);
                c.refillMs = now;
            }
            Frame frame;
            if (c.tokens < 1.0 || !DecodeFrameText(argv[1], frame)) { ++c.rejectedFrames; LogRejects(host, index, c, now); return; }
            c.tokens -= 1.0;
            LogRejects(host, index, c, now);
            if (c.relayedFrames % 1500u == 0u)
            {
                char log[192];
                std::snprintf(log, sizeof(log), "L4D2VR server: player %d Opus voice frames relayed %u (delivered %u copies, %u by the relay rule, rejected %u)\n",
                    index, static_cast<unsigned>(c.relayedFrames), static_cast<unsigned>(c.deliveredFrames),
                    static_cast<unsigned>(c.ownRuleCopies), static_cast<unsigned>(c.rejectedFrames));
                host.Log(log);
            }
            char line[48 + kMaxFrameTextChars];
            const int n = std::snprintf(line, sizeof(line), "%s %d %s\n", kRxCommand, index, argv[1]);
            if (n <= 0 || n >= static_cast<int>(sizeof(line))) { ++c.rejectedFrames; return; }
            ++c.relayedFrames;
            ++m_RelayedTotal;
            RefreshCache(host);
            int listeners = 0;
            std::uint64_t blocked = 0u, queued = 0u;
            for (int r = 1; r <= m_ClientMax; ++r)
            {
                RelayClient& rc = m_Clients[static_cast<std::size_t>(r)];
                if (!rc.hello || !(rc.flags & kHelloReceive) || !host.Connected(r)) continue;
                if (r == index)
                {
                    if (!(c.flags & kHelloLoopback)) continue;
                }
                else
                {
                    const Hearing h = Evaluate(host, r, index);
                    if (!Hears(h)) { blocked |= PlayerBit(r); continue; }
                    if (h == Hearing::OwnRule) ++c.ownRuleCopies;
                }
                if (now > rc.rxRefillMs)
                {
                    rc.rxTokens = std::min(kListenerBurstFrames,
                        rc.rxTokens + double(now - rc.rxRefillMs) * kListenerFramesPerSecond / 1000.0);
                    rc.rxRefillMs = now;
                }
                if (r != index) ++listeners;
                if (rc.rxTokens < 1.0)
                {
                    if (rc.rxDropped++ % 500u == 0u)
                    {
                        char log[128];
                        std::snprintf(log, sizeof(log), "L4D2VR server: player %d Opus voice: over the listener budget, %u frames not sent\n",
                            r, static_cast<unsigned>(rc.rxDropped));
                        host.Log(log);
                    }
                    continue;
                }
                rc.rxTokens -= 1.0;
                if (rc.unreliable)
                {
                    queued |= PlayerBit(r);
                }
                else if (!SendReliable(host, r, rc, line))
                {
                    continue;
                }
                ++m_DeliveredTotal;
                ++c.deliveredFrames;
            }
            if (queued)
            {
                if (c.queue.size() >= kMaxQueuedFrames) c.queue.erase(c.queue.begin());   // no Flush for a while
                QueuedFrame q;
                q.ms = now;
                q.listeners = queued;
                std::snprintf(q.text, sizeof(q.text), "%s", argv[1]);
                c.queue.push_back(q);
                // A host that missed its server-frame flush still delivers.
                if (now - c.queue.front().ms >= kMaxQueueMs) Flush(host);
            }
            UpdateDiagnostics(host, index, c, listeners, blocked, now);
        }

        // One reliable rx command; false when the listener's reliable stream
        // is backed up (the frame is dropped instead of overflowing its
        // command buffer later).
        bool SendReliable(RelayHost& host, int r, RelayClient& rc, const char* line)
        {
            const int pending = host.PendingReliableBytes(r);
            if (pending >= kMaxReliableBacklogBytes)
            {
                if (rc.backlogDropped++ % 250u == 0u)
                {
                    char log[160];
                    std::snprintf(log, sizeof(log), "L4D2VR server: player %d Opus voice: reliable stream backed up (%d bytes waiting), %u frames not sent\n",
                        r, pending, static_cast<unsigned>(rc.backlogDropped));
                    host.Log(log);
                }
                return false;
            }
            host.Send(r, line);
            return true;
        }

        void FlushTo(RelayHost& host, int t, RelayClient& c, int r, std::uint64_t now, int& spent)
        {
            RelayClient& rc = m_Clients[static_cast<std::size_t>(r)];
            const char* frames[kMaxQueuedFrames + 1];
            int count = 0;
            bool redundant = false;
            if (c.haveLastFlushed && (c.lastFlushed.listeners & PlayerBit(r)) && now - c.lastFlushed.ms <= kRedundancyMaxAgeMs)
            {
                frames[count++] = c.lastFlushed.text;
                redundant = true;
            }
            for (const QueuedFrame& q : c.queue)
                if (q.listeners & PlayerBit(r)) frames[count++] = q.text;
            int i = 0;
            // The redundant copy only rides along in the first line, and only
            // if the new frames do not need its room.
            if (redundant)
            {
                const int fresh = count - 1;
                const int firstLen = static_cast<int>(sizeof(kRxCommand) - 1) + 4;
                int len = firstLen + static_cast<int>(std::strlen(frames[0])) + 1;
                int inLine = 1;
                for (int k = 1; k < count && inLine < kMaxRxFramesPerCommand; ++k)
                {
                    const int add = static_cast<int>(std::strlen(frames[k])) + 1;
                    if (len + add + 1 > kMaxRxLineChars) break;
                    len += add;
                    ++inLine;
                }
                // Drop the copy when it would push a new frame into another line.
                if (inLine - 1 < std::min(fresh, kMaxRxFramesPerCommand)) i = 1;
            }
            while (i < count)
            {
                char line[kMaxRxLineChars + 8];
                int len = std::snprintf(line, sizeof(line), "%s %d", kRxCommand, t);
                int inLine = 0;
                const int lineStart = i;
                while (i < count && inLine < kMaxRxFramesPerCommand)
                {
                    const int add = static_cast<int>(std::strlen(frames[i])) + 1;
                    if (inLine > 0 && len + add + 1 > kMaxRxLineChars) break;
                    line[len++] = ' ';
                    std::memcpy(line + len, frames[i], static_cast<std::size_t>(add - 1));
                    len += add - 1;
                    ++inLine;
                    ++i;
                }
                line[len++] = '\n';
                line[len] = '\0';
                const bool withCopy = redundant && lineStart == 0;
                if (spent + len + 2 > kMaxUnreliableBytesPerFlush)
                {
                    // Over this listener's share of the datagram: the frames are
                    // dropped (the jitter buffer conceals them).
                    const int lost = inLine - (withCopy ? 1 : 0);
                    const std::uint32_t before = rc.rxDropped;
                    rc.rxDropped += static_cast<std::uint32_t>(std::max(lost, 0));
                    if (lost > 0 && (before == 0u || before / 500u != rc.rxDropped / 500u))
                    {
                        char log[128];
                        std::snprintf(log, sizeof(log), "L4D2VR server: player %d Opus voice: over the datagram budget, %u frames not sent\n",
                            r, static_cast<unsigned>(rc.rxDropped));
                        host.Log(log);
                    }
                    continue;
                }
                if (host.SendUnreliable(r, line))
                {
                    spent += len + 2;
                    ++rc.unreliableLines;
                    if (withCopy) ++c.redundantCopies;
                    continue;
                }
                // The channel went away: this line's new frames go reliably, one per command.
                if (rc.unreliableFailed++ == 0u)
                {
                    char log[128];
                    std::snprintf(log, sizeof(log), "L4D2VR server: player %d Opus voice: unreliable send failed, frames go reliably\n", r);
                    host.Log(log);
                }
                for (int k = lineStart + (withCopy ? 1 : 0); k < i; ++k)
                {
                    char single[48 + kMaxFrameTextChars];
                    std::snprintf(single, sizeof(single), "%s %d %s\n", kRxCommand, t, frames[k]);
                    SendReliable(host, r, rc, single);
                }
            }
        }

        void LogBlocked(RelayHost& host, int talker, int receiver, const char* prefix)
        {
            const Hearing h = Evaluate(host, receiver, talker);
            const std::uint64_t row = StockRow(host, receiver);
            char log[320];
            std::snprintf(log, sizeof(log),
                "L4D2VR server: player %d Opus voice %s player %d: %s (teams %d/%d, sv_alltalk %d, game voice mask %d, receiver's mask row %x %x)\n",
                talker, prefix, receiver, HearingName(h), TeamOf(receiver), TeamOf(talker), m_AllTalk,
                host.CanHear(receiver, talker) ? 1 : 0, static_cast<unsigned>(row & 0xFFFFFFFFu), static_cast<unsigned>(row >> 32));
            host.Log(log);
        }

        // Logs every change of who cannot hear this talker (with the reason),
        // and every 30 s while nobody can.
        void UpdateDiagnostics(RelayHost& host, int index, RelayClient& c, int listeners, std::uint64_t blocked, std::uint64_t now)
        {
            if (blocked != c.blockedMask)
            {
                for (int r = 1; r <= m_ClientMax; ++r)
                {
                    const std::uint64_t bit = PlayerBit(r);
                    if ((blocked & bit) && !(c.blockedMask & bit)) LogBlocked(host, index, r, "does not reach");
                    else if (!(blocked & bit) && (c.blockedMask & bit))
                    {
                        char log[128];
                        std::snprintf(log, sizeof(log), "L4D2VR server: player %d Opus voice reaches player %d again\n", index, r);
                        host.Log(log);
                    }
                }
                c.outageFrames = 0u;
                c.blockedMask = blocked;
                c.outageLogMs = now;
            }
            if (listeners == 0 && blocked)
            {
                ++c.outageFrames;
                if (now - c.outageLogMs >= kOutageRepeatMs)
                {
                    char log[192];
                    std::snprintf(log, sizeof(log), "L4D2VR server: player %d Opus voice: still no Opus listener may hear this player (%u frames in %.0f s)\n",
                        index, static_cast<unsigned>(c.outageFrames), double(now - c.outageLogMs) / 1000.0);
                    host.Log(log);
                    for (int r = 1; r <= m_ClientMax; ++r)
                        if (blocked & PlayerBit(r)) LogBlocked(host, index, r, "does not reach");
                    c.outageLogMs = now;
                    c.outageFrames = 0u;
                }
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
        // Rule inputs, refreshed at most once per kRuleCacheMs.
        bool m_CacheValid = false;
        std::uint64_t m_CacheMs = 0u;
        std::array<int, kMaxPlayers + 1> m_Team{};
        std::array<std::int8_t, kMaxPlayers + 1> m_RowInUse{};
        int m_AllTalk = -1;
    };
}

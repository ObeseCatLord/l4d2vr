#pragma once

// Client-command side of the dedicated plugin: capability handshake, pose
// relay (protocol v2 byte-identical to the proven Linux relay
// l4d2vr_pose_server.cpp, plus protocol v3 with downconversion) and PR409
// transaction dispatch. std-only; the shell supplies a SessionHost.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../vr_pose_protocol.h"
#include "../vr_pose_protocol_v3.h"
#include "vr_server_protocol.h"
#include "vr_server_transactions.h"

namespace l4d2vr_server
{
    constexpr int kMaxPlayerSlots = 64;

    class SessionHost
    {
    public:
        // Reliable server->client command; `text` ends with '\n'. Never used as a
        // printf format by implementations.
        virtual void SendToClient(const void* entity, const char* text) = 0;
        virtual std::uint64_t NowMs() const = 0;    // monotonic milliseconds
        virtual std::uint64_t NowUs() const = 0;    // monotonic microseconds
        // Sender view yaw (degrees) for v3 -> v2 downconversion.
        virtual bool ViewYaw(int playerIndex, float& yaw) const = 0;
        virtual void Log(const char* line) = 0;
    protected:
        ~SessionHost() = default;
    };

    struct SessionCapabilities
    {
        bool usercmdDecoder = false;      // l4d2vr_server_ack 4
        bool interaction = false;         // l4d2vr_interaction_ack 1
        std::uint32_t features = 0u;      // resolved and operator-enabled bits
    };

    enum class CommandResult { Continue, Stop };

    struct SessionClient
    {
        const void* entity = nullptr;
        // Proven relay state machine.
        bool protocolSupported = false;
        bool haveSequence = false;
        std::uint16_t lastSequence = 0u;
        bool haveDecodeTime = false;
        std::uint64_t lastDecodeUs = 0u;
        bool ackPending = false;
        int ackAttempts = 0;
        std::uint64_t nextAckMs = 0u;
        // Additions.
        bool v3Supported = false;
        bool helloSeen = false;
        int bundleAttempts = 0;
        std::uint64_t nextBundleMs = 0u;
        bool bundleConfirmed = false;
        std::uint32_t requestedFeatures = kFeatureAllMask;
        bool shotgunPattern = false;      // opted in with l4d2vr_shotgun_pattern 1
        TransactionClient txn{};
    };

    class Session
    {
    public:
        static constexpr int kAckMaxAttempts = 3;               // proven relay
        static constexpr std::uint64_t kAckRetryMs = 1000u;     // proven relay
        static constexpr std::uint64_t kMinimumUploadIntervalUs = 16667u;
        static constexpr int kBundleMaxAttempts = 3;
        static constexpr std::uint64_t kBundleRetryMs = 1000u;

        explicit Session(SessionHost& host) : m_Host(host) {}

        void SetCapabilities(const SessionCapabilities& caps) { m_Caps = caps; }
        const SessionCapabilities& Capabilities() const { return m_Caps; }
        void SeedTokens(std::uint32_t seed) { m_NextToken = seed | 1u; }
        void SetClientMax(int clientMax)
        {
            m_ClientMax = (clientMax >= 1 && clientMax <= kMaxPlayerSlots) ? clientMax : kMaxPlayerSlots;
        }
        int ClientMax() const { return m_ClientMax; }
        bool IsValidPlayerIndex(int index) const
        {
            return index >= 1 && index <= kMaxPlayerSlots && index <= m_ClientMax;
        }

        void ResetAll()
        {
            for (SessionClient& c : m_Clients) c = SessionClient{};
        }

        void ResetClient(int index, const void* entity)
        {
            if (!IsValidPlayerIndex(index)) return;
            m_Clients[static_cast<std::size_t>(index)] = SessionClient{};
            m_Clients[static_cast<std::size_t>(index)].entity = entity;
        }

        const SessionClient* Client(int index) const
        {
            return IsValidPlayerIndex(index) ? &m_Clients[static_cast<std::size_t>(index)] : nullptr;
        }

        // Effective server features for this player.
        std::uint32_t EffectiveFeatures(int index) const
        {
            const SessionClient* c = Client(index);
            if (!c || !c->helloSeen) return 0u;
            return m_Caps.features & c->requestedFeatures;
        }

        // Pavlov shotgun pellets apply only to players that opted in, and only
        // while the feature is resolved and requested.
        bool ShotgunPattern(int index) const
        {
            const SessionClient* c = Client(index);
            return c && c->shotgunPattern && (EffectiveFeatures(index) & kFeatureShotgunPattern) != 0u;
        }

        // ---- Engine callbacks (relay semantics) -----------------------------
        void ClientPutInServer(int index, const void* entity)
        {
            if (!IsValidPlayerIndex(index)) return;
            ResetClient(index, entity);
            QueueInitialAck(index, entity);
        }

        void ClientActive(int index, const void* entity)
        {
            if (!IsValidPlayerIndex(index)) return;
            SessionClient& c = m_Clients[static_cast<std::size_t>(index)];
            if (c.entity != entity) ResetClient(index, entity);
            QueueInitialAck(index, entity);
        }

        void ClientDisconnect(int index, const void* entity)
        {
            if (!IsValidPlayerIndex(index)) return;
            const SessionClient& c = m_Clients[static_cast<std::size_t>(index)];
            if (!c.entity || c.entity == entity) ResetClient(index, nullptr);
        }

        void EdictFreed(int index, const void* entity)
        {
            if (!IsValidPlayerIndex(index)) return;
            if (m_Clients[static_cast<std::size_t>(index)].entity == entity) ResetClient(index, nullptr);
        }

        void GameFrame()
        {
            const std::uint64_t now = m_Host.NowMs();
            for (int index = 1; index <= m_ClientMax; ++index)
            {
                SessionClient& c = m_Clients[static_cast<std::size_t>(index)];
                if (c.ackPending && now >= c.nextAckMs)
                    SendAckAttempt(c);
                if (c.helloSeen && c.entity && !c.bundleConfirmed && c.bundleAttempts > 0 &&
                    c.bundleAttempts < kBundleMaxAttempts && now >= c.nextBundleMs)
                    SendBundle(index);
            }
        }

        // The shell reports every decoded VR usercmd: the bundle arrived.
        void NoteVRUsercmd(int index)
        {
            if (IsValidPlayerIndex(index)) m_Clients[static_cast<std::size_t>(index)].bundleConfirmed = true;
        }

        CommandResult ClientCommand(int index, const void* entity, int argc, const char* const* argv,
            AmmoBackend* ammo)
        {
            if (argc <= 0 || !argv || !argv[0]) return CommandResult::Continue;
            const char* name = argv[0];
            const bool isHello = std::strcmp(name, kPoseHelloCommand) == 0;
            const bool isUpload = std::strcmp(name, kPoseUploadCommand) == 0;
            const bool isFeatureHello = std::strcmp(name, kFeatureHelloCommand) == 0;
            const bool isShell = std::strcmp(name, kShellInsertCommand) == 0;
            const bool isMag = std::strcmp(name, kMagActionCommand) == 0;
            const bool isPattern = std::strcmp(name, kShotgunPatternCommand) == 0;
            if (!isHello && !isUpload && !isFeatureHello && !isShell && !isMag && !isPattern)
                return CommandResult::Continue;
            if (!IsValidPlayerIndex(index)) return CommandResult::Stop;

            SessionClient& c = m_Clients[static_cast<std::size_t>(index)];
            if (c.entity != entity) ResetClient(index, entity);

            if (isHello) { HandleHello(index, argc, argv); return CommandResult::Stop; }
            if (isUpload) { HandleUpload(index, argc, argv); return CommandResult::Stop; }
            if (isFeatureHello) { HandleFeatureHello(index, argc, argv); return CommandResult::Stop; }
            if (isShell) { HandleShell(index, argc, argv, ammo); return CommandResult::Stop; }
            if (isPattern)
            {
                std::uint32_t on = 0u;
                if (argc == 2 && ParseDecimal(argv[1], 1u, on))
                {
                    c.shotgunPattern = on != 0u;
                    char log[96];
                    std::snprintf(log, sizeof(log), "L4D2VR server: player %d uses %s shotgun pellets\n",
                        index, on ? "Pavlov" : "stock");
                    m_Host.Log(log);
                }
                return CommandResult::Stop;
            }
            HandleMag(index, argc, argv, ammo);
            return CommandResult::Stop;
        }

    private:
        void Send(const SessionClient& c, const char* text)
        {
            if (c.entity && text && *text) m_Host.SendToClient(c.entity, text);
        }

        void SendAckAttempt(SessionClient& c)
        {
            if (!c.entity || c.ackAttempts >= kAckMaxAttempts)
            {
                c.ackPending = false;
                return;
            }
            Send(c, kPoseAckText);
            ++c.ackAttempts;
            c.ackPending = !c.protocolSupported && c.ackAttempts < kAckMaxAttempts;
            c.nextAckMs = m_Host.NowMs() + kAckRetryMs;
        }

        void QueueInitialAck(int index, const void* entity)
        {
            if (!IsValidPlayerIndex(index) || !entity) return;
            SessionClient& c = m_Clients[static_cast<std::size_t>(index)];
            c.entity = entity;
            if (c.ackPending || c.ackAttempts >= kAckMaxAttempts) return;
            SendAckAttempt(c);
        }

        std::uint32_t NextToken()
        {
            std::uint32_t token = m_NextToken++;
            if (!token) token = m_NextToken++;
            return token;
        }

        void SendBundle(int index)
        {
            SessionClient& c = m_Clients[static_cast<std::size_t>(index)];
            if (!c.entity) return;
            char line[128];
            if (m_Caps.usercmdDecoder)
            {
                if (FormatServerAck(line, sizeof(line))) Send(c, line);
                if (m_Caps.interaction && FormatInteractionAck(line, sizeof(line))) Send(c, line);
                const bool shell = (m_Caps.features & kFeatureShellTxn) != 0u;
                const bool mag = (m_Caps.features & kFeatureMagTxn) != 0u;
                if ((shell || mag) && c.txn.entity != c.entity)
                {
                    c.txn = TransactionClient{};
                    c.txn.entity = c.entity;
                    c.txn.shell.Reset(NextToken());
                    c.txn.mag.Reset(NextToken());
                }
                if (shell && FormatShellCapability(c.txn.shell.Token(), line, sizeof(line))) Send(c, line);
                if (mag && FormatMagCapability(c.txn.mag.Token(), line, sizeof(line))) Send(c, line);
            }
            if (FormatFeatureAck(m_Caps.features & c.requestedFeatures, line, sizeof(line))) Send(c, line);
            ++c.bundleAttempts;
            c.nextBundleMs = m_Host.NowMs() + kBundleRetryMs;
            if (!m_Caps.usercmdDecoder) c.bundleConfirmed = true;
            if (c.bundleAttempts == 1)
            {
                char log[160];
                std::snprintf(log, sizeof(log),
                    "L4D2VR server: player %d hello; sent capability bundle server_ack=%d interaction=%d features=0x%x pose=v%u\n",
                    index, m_Caps.usercmdDecoder ? 1 : 0, m_Caps.interaction ? 1 : 0,
                    static_cast<unsigned>(m_Caps.features & c.requestedFeatures), c.v3Supported ? 3u : 2u);
                m_Host.Log(log);
            }
        }

        void OnHello(int index)
        {
            SessionClient& c = m_Clients[static_cast<std::size_t>(index)];
            if (c.helloSeen) return;
            c.helloSeen = true;
            SendBundle(index);
        }

        void HandleHello(int index, int argc, const char* const* argv)
        {
            SessionClient& c = m_Clients[static_cast<std::size_t>(index)];
            std::uint32_t version = 0u;
            if (argc != 2 || !ParseDecimal(argv[1], 0xFFFFu, version))
                return;
            if (version == kPoseV2)
            {
                c.protocolSupported = true;
                c.ackPending = false;
                OnHello(index);
            }
            else if (version == kPoseV3 && (m_Caps.features & kFeaturePoseV3) != 0u)
            {
                c.protocolSupported = true;
                c.v3Supported = true;
                c.ackPending = false;
                OnHello(index);
            }
        }

        void HandleFeatureHello(int index, int argc, const char* const* argv)
        {
            SessionClient& c = m_Clients[static_cast<std::size_t>(index)];
            std::uint32_t mask = 0u, version = 0u;
            if (argc != 3 || !ParseHexMask(argv[1], mask) || !ParseDecimal(argv[2], 255u, version) || version < 1u)
                return;
            c.requestedFeatures = mask & kFeatureAllMask;
            if (!c.helloSeen)
            {
                OnHello(index);
                return;
            }
            char line[64];
            if (FormatFeatureAck(m_Caps.features & c.requestedFeatures, line, sizeof(line))) Send(c, line);
        }

        void RelayText(int sender, std::uint16_t sequence, const char* payload, bool toV3Only, bool toV2Only)
        {
            char command[192] = {};
            const int length = std::snprintf(command, sizeof(command), "l4d2vr_pose_receive %d %u %s\n",
                sender, static_cast<unsigned int>(sequence), payload);
            if (length <= 0 || length >= static_cast<int>(sizeof(command))) return;
            for (int recipient = 1; recipient <= m_ClientMax; ++recipient)
            {
                if (recipient == sender) continue;
                const SessionClient& r = m_Clients[static_cast<std::size_t>(recipient)];
                if (!r.protocolSupported || !r.entity) continue;
                if (toV3Only && !r.v3Supported) continue;
                if (toV2Only && r.v3Supported) continue;
                Send(r, command);
            }
        }

        void HandleUpload(int index, int argc, const char* const* argv)
        {
            SessionClient& c = m_Clients[static_cast<std::size_t>(index)];
            if (!c.protocolSupported || argc != 3) return;
            std::uint32_t parsed = 0u;
            if (!ParseDecimal(argv[1], 0xFFFFu, parsed)) return;
            const auto sequence = static_cast<std::uint16_t>(parsed);
            if (c.haveSequence && !l4d2vr_pose::IsSequenceNewer(sequence, c.lastSequence)) return;
            const std::uint64_t now = m_Host.NowUs();
            if (c.haveDecodeTime && now - c.lastDecodeUs < kMinimumUploadIntervalUs) return;
            // Invalid traffic also consumes this validation slot (<= 60 decodes/s).
            c.haveDecodeTime = true;
            c.lastDecodeUs = now;

            const char* payload = argv[2];
            const int version = l4d2vr_pose_v3::PayloadVersionForText(payload);
            if (version == 2)
            {
                l4d2vr_pose::WirePacket packet{};
                if (!l4d2vr_pose::DecodePayload(payload, packet)) return;
                c.haveSequence = true;
                c.lastSequence = sequence;
                RelayText(index, sequence, payload, false, false);
                return;
            }
            if (version != 3 || !c.v3Supported || (m_Caps.features & kFeaturePoseV3) == 0u) return;
            l4d2vr_pose_v3::PacketV3 packet{};
            if (!l4d2vr_pose_v3::DecodeText(payload, packet)) return;
            c.haveSequence = true;
            c.lastSequence = sequence;
            RelayText(index, sequence, payload, true, false);
            float yaw = 0.0f;
            l4d2vr_pose::WirePacket v2{};
            std::string v2Text;
            if (m_Host.ViewYaw(index, yaw) && l4d2vr_pose_v3::DownconvertToV2(packet, yaw, v2) &&
                l4d2vr_pose::EncodePayload(v2, v2Text))
                RelayText(index, sequence, v2Text.c_str(), false, true);
        }

        void HandleShell(int index, int argc, const char* const* argv, AmmoBackend* ammo)
        {
            SessionClient& c = m_Clients[static_cast<std::size_t>(index)];
            l4d2vr_shell::Request request{};
            if (!ParseShellRequest(argc, argv, request)) return;
            if ((m_Caps.features & kFeatureShellTxn) == 0u || !ammo || c.txn.entity != c.entity ||
                !c.txn.shell.Token() || request.token != c.txn.shell.Token())
                return;
            const l4d2vr_shell::Reply reply = ApplyShellRequest(c.txn, *ammo, index, request, m_Host.NowMs());
            char line[192];
            if (FormatShellReply(request, reply, line, sizeof(line))) Send(c, line);
            char log[192];
            std::snprintf(log, sizeof(log), "L4D2VR server: shell player=%d seq=%u status=%u weaponId=%d clip=%d reserve=%d\n",
                index, request.sequence, static_cast<unsigned>(reply.status), request.weaponId, reply.clip, reply.reserve);
            m_Host.Log(log);
        }

        void HandleMag(int index, int argc, const char* const* argv, AmmoBackend* ammo)
        {
            SessionClient& c = m_Clients[static_cast<std::size_t>(index)];
            l4d2vr_remote_mag::Request request{};
            if (!ParseMagRequest(argc, argv, request)) return;
            if ((m_Caps.features & kFeatureMagTxn) == 0u || !ammo || c.txn.entity != c.entity ||
                !c.txn.mag.Token() || request.token != c.txn.mag.Token())
                return;
            const l4d2vr_remote_mag::Reply reply = ApplyMagRequest(c.txn, *ammo, index, request, m_Host.NowMs());
            char line[192];
            if (FormatMagReply(request, reply, line, sizeof(line))) Send(c, line);
            char log[192];
            std::snprintf(log, sizeof(log), "L4D2VR server: magazine player=%d seq=%u action=%u status=%u weaponId=%d clip=%d reserve=%d\n",
                index, request.sequence, static_cast<unsigned>(request.action), static_cast<unsigned>(reply.status),
                request.weaponId, reply.clip, reply.reserve);
            m_Host.Log(log);
        }

        SessionHost& m_Host;
        SessionCapabilities m_Caps{};
        std::array<SessionClient, kMaxPlayerSlots + 1> m_Clients{};
        int m_ClientMax = kMaxPlayerSlots;
        std::uint32_t m_NextToken = 0x5EED0001u;
    };
}

// Handshake, pose relay (v2 parity with the proven Linux relay, v3) and PR409
// transaction dispatch of the dedicated-server session.
#include "../L4D2VR/server_core/vr_server_session.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_Checks = 0;
static void CheckImpl(bool ok, const char* what, int line)
{
    ++g_Checks;
    if (!ok) { std::fprintf(stderr, "server session check failed at line %d: %s\n", line, what); std::abort(); }
}
#define Check(x) CheckImpl((x), #x, __LINE__)

using namespace l4d2vr_server;

namespace
{
    struct Sent { const void* entity; std::string text; };

    class FakeHost final : public SessionHost
    {
    public:
        std::vector<Sent> sent;
        std::uint64_t ms = 1000u, us = 1000000u;
        float yaw = 30.0f;
        bool haveYaw = true;
        void SendToClient(const void* entity, const char* text) override { sent.push_back({ entity, text }); }
        std::uint64_t NowMs() const override { return ms; }
        std::uint64_t NowUs() const override { return us; }
        bool ViewYaw(int, float& out) const override { out = yaw; return haveYaw; }
        void Log(const char*) override {}
        void Advance(std::uint64_t milliseconds) { ms += milliseconds; us += milliseconds * 1000u; }
        std::vector<std::string> To(const void* entity) const
        {
            std::vector<std::string> out;
            for (const Sent& s : sent) if (s.entity == entity) out.push_back(s.text);
            return out;
        }
    };

    class FakeAmmo final : public AmmoBackend
    {
    public:
        AmmoState state{};
        bool unsupported = false;
        int writes = 0;
        bool ReadAmmo(int, int requestedWeaponId, bool, AmmoState& out, bool& unsupportedOut) override
        {
            out = state;
            unsupportedOut = unsupported;
            return state.eligible && state.weaponId == requestedWeaponId;
        }
        bool WriteAmmo(const AmmoState& s, int clip, int reserve, bool) override
        {
            if (s.clip != state.clip || s.reserve != state.reserve) return false;
            state.clip = clip;
            state.reserve = reserve;
            ++writes;
            return true;
        }
    };

    CommandResult Dispatch(Session& s, int index, const void* entity, std::vector<const char*> args, AmmoBackend* ammo = nullptr)
    {
        return s.ClientCommand(index, entity, static_cast<int>(args.size()), args.data(), ammo);
    }

    std::string V2Payload()
    {
        l4d2vr_pose::WirePacket packet{};
        l4d2vr_pose::FinalizePacket(packet);
        std::string encoded;
        Check(l4d2vr_pose::EncodePayload(packet, encoded));
        return encoded;
    }

    std::string V3Payload(std::uint16_t sequence)
    {
        using namespace l4d2vr_pose_v3;
        PacketV3 p{};
        p.sequence = sequence;
        p.flags = kFlagHeadValid | kFlagLeftHandValid | kFlagRightHandValid;
        p.eyeOffset[2] = PackPosition(64.0f);
        p.headForward[0] = 32767;
        p.headUp[2] = 32767;
        for (int side = 0; side < 2; ++side)
        {
            p.hands[side].position[0] = PackPosition(16.0f);
            p.hands[side].position[1] = PackPosition(side == kLeft ? 8.0f : -8.0f);
            p.hands[side].position[2] = PackPosition(40.0f);
            p.hands[side].rotation[3] = 32767;
        }
        std::string text;
        Check(EncodeText(p, text));
        return text;
    }

    std::string Receive(int sender, unsigned sequence, const std::string& payload)
    {
        return "l4d2vr_pose_receive " + std::to_string(sender) + " " + std::to_string(sequence) + " " + payload + "\n";
    }

    // The proven relay scenarios (pose_server_test.cpp) against the session,
    // with no server capabilities beyond the relay.
    void TestRelayParity()
    {
        FakeHost host;
        Session s(host);
        SessionCapabilities caps{};
        caps.features = 0u;   // pose v2 relay only
        s.SetCapabilities(caps);
        int e[4] = {};
        s.SetClientMax(1);
        s.ClientPutInServer(0, &e[0]);
        s.ClientPutInServer(2, &e[2]);
        Check(host.sent.empty());
        s.SetClientMax(3);
        s.ClientPutInServer(1, &e[1]);
        s.ClientPutInServer(2, &e[2]);
        s.ClientPutInServer(3, &e[3]);
        Check(host.sent.size() == 3u);
        for (const Sent& m : host.sent) Check(m.text == "l4d2vr_pose_ack 2\n");
        // ClientActive after PutInServer does not resend while pending.
        s.ClientActive(1, &e[1]);
        Check(host.sent.size() == 3u);
        // Retries: 3 attempts, 1 s apart, stop after a hello.
        host.sent.clear();
        Check(Dispatch(s, 1, &e[1], { "l4d2vr_pose_hello", "2" }) == CommandResult::Stop);
        Check(Dispatch(s, 2, &e[2], { "l4d2vr_pose_hello", "2" }) == CommandResult::Stop);
        Check(Dispatch(s, 3, &e[3], { "l4d2vr_pose_hello", "1" }) == CommandResult::Stop);
        // A relay-only server sends nothing but the feature ack after a hello.
        Check(host.To(&e[1]) == std::vector<std::string>{ "l4d2vr_feature_ack 0 1\n" });
        Check(host.To(&e[3]).empty());
        host.sent.clear();
        host.Advance(999);
        s.GameFrame();
        Check(host.sent.empty());
        host.Advance(1);
        s.GameFrame();
        Check(host.sent.size() == 1u && host.sent[0].entity == &e[3] && host.sent[0].text == "l4d2vr_pose_ack 2\n");
        host.Advance(1000);
        s.GameFrame();
        Check(host.sent.size() == 2u);
        host.Advance(1000);
        s.GameFrame();
        Check(host.sent.size() == 2u);                                   // 3 attempts total

        const std::string payload = V2Payload();
        host.sent.clear();
        Check(Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "10", payload.c_str() }) == CommandResult::Stop);
        Check(host.sent.size() == 1u && host.sent[0].entity == &e[2] && host.sent[0].text == Receive(1, 10, payload));
        // Stale and duplicate sequences, rate limit.
        host.sent.clear();
        host.Advance(20);
        Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "10", payload.c_str() });
        host.Advance(20);
        Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "9", payload.c_str() });
        Check(host.sent.empty());
        host.Advance(20);
        Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "11", payload.c_str() });
        Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "12", payload.c_str() });   // < 16.667 ms later
        Check(host.sent.size() == 1u && host.sent[0].text == Receive(1, 11, payload));
        // Wrap-around.
        for (const char* step : { "20000", "40000", "60000" })
        {
            host.Advance(20);
            Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", step, payload.c_str() });
        }
        host.sent.clear();
        host.Advance(20);
        Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "65535", payload.c_str() });
        host.Advance(20);
        Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "0", payload.c_str() });
        Check(host.sent.size() == 2u && host.sent[1].text == Receive(1, 0, payload));
        // Corrupted payload consumes the slot; short payload rejected.
        std::string bad = payload;
        bad[0] = bad[0] == 'A' ? 'B' : 'A';
        host.sent.clear();
        host.Advance(20);
        Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "1", bad.c_str() });
        Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "1", payload.c_str() });
        Check(host.sent.empty());
        host.Advance(20);
        Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "1", payload.c_str() });
        Check(host.sent.size() == 1u);
        host.sent.clear();
        host.Advance(20);
        Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "2", "short" });
        Check(host.sent.empty());
        // Unknown commands continue; invalid player slots are consumed.
        Check(Dispatch(s, 1, &e[1], { "say", "hello" }) == CommandResult::Continue);
        Check(Dispatch(s, -1, &e[1], { "l4d2vr_pose_hello", "2" }) == CommandResult::Stop);
        // Disconnect + replacement: needs a fresh hello.
        s.ClientDisconnect(1, &e[1]);
        s.ClientPutInServer(1, &e[1]);
        Check(host.sent.back().entity == &e[1] && host.sent.back().text == "l4d2vr_pose_ack 2\n");
        host.sent.clear();
        host.Advance(20);
        Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "5", payload.c_str() });
        Check(host.sent.empty());
        // Level reset.
        s.ResetAll();
        host.sent.clear();
        Dispatch(s, 1, &e[1], { "l4d2vr_pose_upload", "6", payload.c_str() });
        Check(host.sent.empty());
    }

    void TestBundle()
    {
        FakeHost host;
        Session s(host);
        SessionCapabilities caps{};
        caps.usercmdDecoder = true;
        caps.interaction = true;
        caps.features = kFeatureAllMask;
        s.SetCapabilities(caps);
        s.SeedTokens(41u);
        s.SetClientMax(4);
        int desktop = 0, vr = 0;
        s.ClientPutInServer(1, &desktop);
        s.ClientPutInServer(2, &vr);
        Check(host.To(&desktop) == std::vector<std::string>{ "l4d2vr_pose_ack 2\n" });
        // Desktop never sees more than the relay's pose ack, even after retries.
        for (int i = 0; i < 10; ++i) { host.Advance(1000); s.GameFrame(); }
        for (const std::string& t : host.To(&desktop)) Check(t == "l4d2vr_pose_ack 2\n");
        Check(host.To(&desktop).size() == 3u);
        Check(s.EffectiveFeatures(1) == 0u);

        host.sent.clear();
        Dispatch(s, 2, &vr, { "l4d2vr_pose_hello", "2" });
        const auto bundle = host.To(&vr);
        Check(bundle.size() == 5u);
        Check(bundle[0] == "l4d2vr_server_ack 4\n");
        Check(bundle[1] == "l4d2vr_interaction_ack 1\n");
        Check(bundle[2] == "l4d2vr_shell_capability 1 41\n");
        Check(bundle[3] == "l4d2vr_mag_capability 3 42\n");
        Check(bundle[4] == "l4d2vr_feature_ack 3ff 1\n");
        Check(s.EffectiveFeatures(2) == kFeatureAllMask);
        // The client answers server_ack with another hello: no duplicate bundle.
        host.sent.clear();
        Dispatch(s, 2, &vr, { "l4d2vr_pose_hello", "2" });
        Check(host.sent.empty());
        // Unconfirmed bundle is retried (same tokens) until a VR usercmd arrives.
        host.Advance(1000);
        s.GameFrame();
        Check(host.To(&vr).size() == 5u && host.To(&vr)[2] == "l4d2vr_shell_capability 1 41\n");
        s.NoteVRUsercmd(2);
        host.sent.clear();
        host.Advance(1000);
        s.GameFrame();
        Check(host.sent.empty());
        // Feature hello narrows this player's features and re-acks.
        Dispatch(s, 2, &vr, { "l4d2vr_feature_hello", "0x105", "1" });
        Check(host.To(&vr) == std::vector<std::string>{ "l4d2vr_feature_ack 105 1\n" });
        Check(s.EffectiveFeatures(2) == 0x105u);
        Check(Dispatch(s, 2, &vr, { "l4d2vr_feature_hello", "zz", "1" }) == CommandResult::Stop);
        Check(s.EffectiveFeatures(2) == 0x105u);

        // Decoder missing: no server_ack, only feature ack (pose v3 bit).
        FakeHost host2;
        Session relay(host2);
        SessionCapabilities relayCaps{};
        relayCaps.features = kFeaturePoseV3;
        relay.SetCapabilities(relayCaps);
        relay.SetClientMax(2);
        relay.ClientPutInServer(1, &vr);
        host2.sent.clear();
        Dispatch(relay, 1, &vr, { "l4d2vr_pose_hello", "3" });
        Check(host2.To(&vr) == std::vector<std::string>{ "l4d2vr_feature_ack 100 1\n" });
    }

    void TestPoseV3()
    {
        FakeHost host;
        Session s(host);
        SessionCapabilities caps{};
        caps.features = kFeaturePoseV3;
        s.SetCapabilities(caps);
        s.SetClientMax(3);
        int a = 0, b = 0, c = 0;
        s.ClientPutInServer(1, &a);
        s.ClientPutInServer(2, &b);
        s.ClientPutInServer(3, &c);
        Dispatch(s, 1, &a, { "l4d2vr_pose_hello", "3" });   // v3 sender
        Dispatch(s, 2, &b, { "l4d2vr_pose_hello", "3" });   // v3 receiver
        Dispatch(s, 3, &c, { "l4d2vr_pose_hello", "2" });   // v2 receiver
        host.sent.clear();
        const std::string v3 = V3Payload(7);
        Check(Dispatch(s, 1, &a, { "l4d2vr_pose_upload", "7", v3.c_str() }) == CommandResult::Stop);
        const auto toB = host.To(&b), toC = host.To(&c);
        Check(toB.size() == 1u && toB[0] == Receive(1, 7, v3));
        Check(toC.size() == 1u);
        // The v2 peer receives a valid 72-character v2 payload.
        const std::string prefix = "l4d2vr_pose_receive 1 7 ";
        Check(toC[0].compare(0, prefix.size(), prefix) == 0);
        const std::string v2 = toC[0].substr(prefix.size(), toC[0].size() - prefix.size() - 1u);
        l4d2vr_pose::WirePacket packet{};
        Check(v2.size() == 72u && l4d2vr_pose::DecodePayload(v2.c_str(), packet));
        Check(l4d2vr_pose::PacketValidMask(packet) == l4d2vr_pose::kValidMask);
        // A v2 upload from a v2 sender reaches both peers unchanged.
        host.sent.clear();
        host.Advance(20);
        const std::string p2 = V2Payload();
        Dispatch(s, 3, &c, { "l4d2vr_pose_upload", "1", p2.c_str() });
        Check(host.To(&a) == std::vector<std::string>{ Receive(3, 1, p2) } && host.To(&b) == host.To(&a));
        // A v2-only sender may not upload v3; without view yaw, v2 peers are skipped.
        host.sent.clear();
        host.Advance(20);
        Dispatch(s, 3, &c, { "l4d2vr_pose_upload", "2", v3.c_str() });
        Check(host.sent.empty());
        host.haveYaw = false;
        host.Advance(20);
        Dispatch(s, 1, &a, { "l4d2vr_pose_upload", "8", V3Payload(8).c_str() });
        Check(host.To(&b).size() == 1u && host.To(&c).empty());
        // Relay without the PoseV3 feature ignores hello 3.
        FakeHost host2;
        Session plain(host2);
        plain.SetCapabilities(SessionCapabilities{});
        plain.SetClientMax(2);
        plain.ClientPutInServer(1, &a);
        host2.sent.clear();
        Dispatch(plain, 1, &a, { "l4d2vr_pose_hello", "3" });
        Check(host2.sent.empty() && !plain.Client(1)->protocolSupported);
    }

    void TestTransactions()
    {
        FakeHost host;
        Session s(host);
        SessionCapabilities caps{};
        caps.usercmdDecoder = true;
        caps.features = kFeatureMagTxn | kFeatureShellTxn;
        s.SetCapabilities(caps);
        s.SeedTokens(1001u);
        s.SetClientMax(2);
        int vr = 0;
        s.ClientPutInServer(1, &vr);
        FakeAmmo ammo;
        // Before the hello no ledger exists: commands are consumed silently.
        host.sent.clear();
        Check(Dispatch(s, 1, &vr, { "l4d2vr_shell_insert_v1", "1001", "1", "12289", "200", "3", "4", "50" }, &ammo) == CommandResult::Stop);
        Check(host.sent.empty());
        Dispatch(s, 1, &vr, { "l4d2vr_pose_hello", "2" });
        const auto bundle = host.To(&vr);
        Check(bundle.size() == 4u && bundle[1] == "l4d2vr_shell_capability 1 1001\n" && bundle[2] == "l4d2vr_mag_capability 3 1002\n");

        ammo.state = AmmoState{ true, 0x3001u, 200u, 3, 4, 50, 8, false, nullptr, nullptr, 7 };
        host.sent.clear();
        Dispatch(s, 1, &vr, { "l4d2vr_shell_insert_v1", "1001", "1", "12289", "200", "3", "4", "50" }, &ammo);
        Check(host.To(&vr) == std::vector<std::string>{ "l4d2vr_shell_result 1001 1 12289 200 3 4 50 0 5 49\n" });
        Check(ammo.writes == 1 && ammo.state.clip == 5 && ammo.state.reserve == 49);
        // Exact repeat returns the memo without a second write.
        host.sent.clear();
        Dispatch(s, 1, &vr, { "l4d2vr_shell_insert_v1", "1001", "1", "12289", "200", "3", "4", "50" }, &ammo);
        Check(host.To(&vr) == std::vector<std::string>{ "l4d2vr_shell_result 1001 1 12289 200 3 4 50 0 5 49\n" } && ammo.writes == 1);
        // Wrong token: no reply. Bad arity/number: no reply.
        host.sent.clear();
        Dispatch(s, 1, &vr, { "l4d2vr_shell_insert_v1", "999", "2", "12289", "200", "3", "5", "49" }, &ammo);
        Dispatch(s, 1, &vr, { "l4d2vr_shell_insert_v1", "1001", "2", "12289", "200", "3", "5" }, &ammo);
        Dispatch(s, 1, &vr, { "l4d2vr_shell_insert_v1", "1001", "2", "12289", "200", "3", "-5", "49" }, &ammo);
        Check(host.sent.empty());

        // Magazine eject (29 rounds back to the reserve) then insert.
        ammo.state = AmmoState{ true, 0x3002u, 300u, 5, 30, 100, 50, false, nullptr, nullptr, 3 };
        host.sent.clear();
        Dispatch(s, 1, &vr, { "l4d2vr_mag_action_v2", "1002", "1", "12290", "300", "5", "30", "100", "0" }, &ammo);
        Check(host.To(&vr) == std::vector<std::string>{ "l4d2vr_mag_result 1002 1 12290 300 5 30 100 0 0 1 129\n" });
        host.Advance(200);
        host.sent.clear();
        Dispatch(s, 1, &vr, { "l4d2vr_mag_action_v2", "1002", "2", "12290", "301", "5", "1", "129", "1" }, &ammo);
        Check(host.To(&vr) == std::vector<std::string>{ "l4d2vr_mag_result 1002 2 12290 301 5 1 129 1 0 50 80\n" });
        // Backend that cannot serve: Unsupported (7).
        ammo.unsupported = true;
        host.sent.clear();
        Dispatch(s, 1, &vr, { "l4d2vr_mag_action_v2", "1002", "3", "12290", "302", "5", "50", "80", "0" }, &ammo);
        Check(host.To(&vr) == std::vector<std::string>{ "l4d2vr_mag_result 1002 3 12290 302 5 50 80 0 7 0 0\n" });
        // Without the feature bit the commands are consumed silently.
        FakeHost host2;
        Session off(host2);
        SessionCapabilities offCaps{};
        offCaps.usercmdDecoder = true;
        off.SetCapabilities(offCaps);
        off.SetClientMax(2);
        off.ClientPutInServer(1, &vr);
        Dispatch(off, 1, &vr, { "l4d2vr_pose_hello", "2" });
        host2.sent.clear();
        Check(Dispatch(off, 1, &vr, { "l4d2vr_mag_action_v2", "1", "1", "12290", "300", "5", "30", "100", "0" }, &ammo) == CommandResult::Stop);
        Check(host2.sent.empty());
    }
}

int main()
{
    TestRelayParity();
    TestBundle();
    TestPoseV3();
    TestTransactions();
    std::printf("vr_server_session_tests: %d checks passed\n", g_Checks);
    return 0;
}

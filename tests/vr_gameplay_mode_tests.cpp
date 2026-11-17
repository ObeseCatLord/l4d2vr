#include "../L4D2VR/vr_gameplay_mode.h"
#include "../L4D2VR/vr_server_features.h"
#include "../L4D2VR/vr_grip_release.h"
#include "../L4D2VR/vr_convar_policy.h"
#include <cstdio>
#include <cstdlib>

using namespace l4d2vr_gameplay_mode;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static Inputs In(Config config, bool vr, bool inGame, bool listen, bool ack, bool grace)
{
    Inputs in;
    in.config = config;
    in.vrActive = vr;
    in.inGame = inGame;
    in.listenHost = listen;
    in.serverAck = ack;
    in.graceExpired = grace;
    return in;
}

static void TestParse()
{
    Config c = Config::Physical;
    CHECK(ParseConfig("auto", c) && c == Config::Auto);
    CHECK(ParseConfig(" Classic ", c) && c == Config::Classic);
    CHECK(ParseConfig("PHYSICAL", c) && c == Config::Physical);
    c = Config::Classic;
    CHECK(!ParseConfig("pavlov", c) && c == Config::Classic);
    CHECK(!ParseConfig("", c));
}

// Every config value x server capability x listen host / remote x VR / desktop.
static void TestResolveMatrix()
{
    const Config configs[] = { Config::Auto, Config::Physical, Config::Classic };
    for (Config config : configs)
        for (int vr = 0; vr < 2; ++vr)
            for (int listen = 0; listen < 2; ++listen)
                for (int ack = 0; ack < 2; ++ack)
                    for (int grace = 0; grace < 2; ++grace)
                    {
                        const Result r = Resolve(In(config, vr != 0, true, listen != 0, ack != 0, grace != 0));
                        if (!vr)
                        {
                            // Desktop players stay vanilla whatever the config says.
                            CHECK(r.effective == Effective::Vanilla);
                            continue;
                        }
                        if (config == Config::Classic)
                        {
                            CHECK(r.effective == Effective::Classic);
                            CHECK(r.reason == Reason::ForcedClassic);
                            continue;
                        }
                        // Auto and Physical: physical only with a modded server.
                        if (listen || ack)
                            CHECK(r.effective == Effective::Physical);
                        else if (!grace)
                            CHECK(r.effective == Effective::PendingClassic);
                        else
                            CHECK(r.effective == Effective::Classic);
                        // Never physical against a server that did not acknowledge.
                        if (!listen && !ack)
                            CHECK(ClassicLike(r.effective));
                    }
}

static void TestReasons()
{
    CHECK(Resolve(In(Config::Auto, true, true, true, false, false)).reason == Reason::ListenHost);
    CHECK(Resolve(In(Config::Auto, true, true, false, true, false)).reason == Reason::ServerAck);
    CHECK(Resolve(In(Config::Auto, true, true, false, false, false)).reason == Reason::AwaitingAck);
    CHECK(Resolve(In(Config::Auto, true, true, false, false, true)).reason == Reason::NoAck);
    CHECK(Resolve(In(Config::Physical, true, true, false, false, true)).reason == Reason::ForcedPhysicalWithoutAck);
    CHECK(Resolve(In(Config::Auto, true, false, false, false, false)).reason == Reason::NotInGame);
    CHECK(Resolve(In(Config::Auto, false, true, false, true, true)).reason == Reason::Desktop);
    // Names exist for the log.
    CHECK(std::strlen(EffectiveName(Effective::PendingClassic)) > 0);
    CHECK(std::strlen(ReasonName(Reason::ForcedPhysicalWithoutAck)) > 0);
    CHECK(std::strcmp(ConfigName(Config::Classic), "classic") == 0);
}

// A late acknowledgement promotes Classic to Physical; the session end demotes again.
static void TestLateAckAndSessionReset()
{
    Inputs in = In(Config::Auto, true, true, false, false, true);
    CHECK(Resolve(in).effective == Effective::Classic);
    in.serverAck = true;
    CHECK(Resolve(in).effective == Effective::Physical);
    in.serverAck = false;
    in.graceExpired = false;
    in.inGame = false;
    CHECK(Resolve(in).effective == Effective::PendingClassic);
}

// Plain server: only stock data.
static void TestClassicWirePolicy()
{
    const Effective classicLike[] = { Effective::Classic, Effective::PendingClassic };
    for (Effective e : classicLike)
    {
        const WirePolicy p = PolicyFor(e, 0u, false);
        CHECK(!p.encodeUsercmd);
        CHECK(!p.protocolImpulses);
        CHECK(!p.gameplayCommands);
        CHECK(!p.poseRelay);
        CHECK(!p.opusVoice);

        // Every protocol impulse and marker bit is removed.
        for (int impulse = 0; impulse < 256; ++impulse)
        {
            int imp = impulse;
            int buttons = 1 | (1 << 13) | static_cast<int>(0x3fu << 26);
            SanitizeStockUsercmd(p, imp, buttons);
            CHECK(!IsProtocolImpulse(imp));
            CHECK((static_cast<std::uint32_t>(buttons) & kProtocolButtonBits) == 0u);
            // Stock buttons (attack, reload) and stock impulses are untouched.
            CHECK((buttons & 1) != 0 && (buttons & (1 << 13)) != 0);
            if (impulse < kFirstProtocolImpulse || impulse > kLastProtocolImpulse)
                CHECK(imp == impulse);
        }
        // The named protocol impulses of the plugin are all in the range.
        CHECK(IsProtocolImpulse(l4d2vr_grip::kPickupImpulse));
        CHECK(IsProtocolImpulse(l4d2vr_grip::kReleaseImpulse));
        CHECK(IsProtocolImpulse(l4d2vr_grip::kBlockNativeReloadImpulse));
        CHECK(IsProtocolImpulse(l4d2vr_grip::kStickyBlockNativeReloadImpulse));
        CHECK(IsProtocolImpulse(l4d2vr_grip::kAllowNativeReloadImpulse));
        CHECK(IsProtocolImpulse(238) && IsProtocolImpulse(239) && IsProtocolImpulse(244));
        CHECK(!IsProtocolImpulse(100) && !IsProtocolImpulse(201) && !IsProtocolImpulse(0));

        // Commands: only the hello reply leaves; stock commands are untouched.
        CHECK(AllowServerCommand(p, "l4d2vr_pose_hello 3"));
        CHECK(!AllowServerCommand(p, "l4d2vr_pose_upload 12 abc"));
        CHECK(!AllowServerCommand(p, "l4d2vr_feature_hello 3fff 1"));
        CHECK(!AllowServerCommand(p, "l4d2vr_shotgun_pattern 1"));
        CHECK(!AllowServerCommand(p, "l4d2vr_shell_insert_v1 1 2 3"));
        CHECK(!AllowServerCommand(p, "l4d2vr_mag_action_v3 1"));
        CHECK(!AllowServerCommand(p, "l4d2vr_mag_dual_v1 1"));
        CHECK(!AllowServerCommand(p, "l4d2vr_pistol_mag_v1 1"));
        CHECK(!AllowServerCommand(p, "l4d2vr_pistol_ammo_ack 1"));
        CHECK(!AllowServerCommand(p, "l4d2vr_voice_hello 1"));
        CHECK(!AllowServerCommand(p, "  l4d2vr_voice_up 1 abc"));
        CHECK(AllowServerCommand(p, "say hello"));
        CHECK(AllowServerCommand(p, "vocalize PlayerNiceShot"));
        CHECK(AllowServerCommand(p, "spec_next"));
        CHECK(!AllowServerCommand(p, nullptr));
    }
}

// Forced classic on a server with the sticky reload gate: only the release passes.
static void TestReloadGateRelease()
{
    const WirePolicy p = PolicyFor(Effective::Classic, 0u, false);
    for (int impulse = kFirstProtocolImpulse; impulse <= kLastProtocolImpulse; ++impulse)
    {
        int imp = impulse, buttons = 0;
        SanitizeStockUsercmd(p, imp, buttons, true);
        CHECK(imp == (impulse == l4d2vr_grip::kAllowNativeReloadImpulse ? impulse : 0));
        imp = impulse;
        SanitizeStockUsercmd(p, imp, buttons, false);
        CHECK(imp == 0);
    }
    CHECK(kAllowNativeReloadImpulse == l4d2vr_grip::kAllowNativeReloadImpulse);
}

// A partial plugin: the relay features it offers are used in classic too.
static void TestPartialPluginFeatures()
{
    const WirePolicy p = PolicyFor(Effective::Classic, l4d2vr_features::OpusVoice, true);
    CHECK(kOpusVoiceFeature == l4d2vr_features::OpusVoice);
    CHECK(p.opusVoice && p.poseRelay);
    CHECK(!p.encodeUsercmd && !p.protocolImpulses && !p.gameplayCommands);
    CHECK(AllowServerCommand(p, "l4d2vr_voice_hello 1"));
    CHECK(AllowServerCommand(p, "l4d2vr_pose_upload 1 abc"));
    CHECK(!AllowServerCommand(p, "l4d2vr_mag_action_v3 1"));
    // Gameplay bits offered by the server do not open gameplay traffic in classic.
    const WirePolicy all = PolicyFor(Effective::Classic, 0xffffffffu, true);
    CHECK(!all.gameplayCommands && !all.protocolImpulses && !all.encodeUsercmd);
}

static void TestPhysicalAndDesktopUnchanged()
{
    const WirePolicy phys = PolicyFor(Effective::Physical, 0u, false);
    CHECK(phys.encodeUsercmd && phys.protocolImpulses && phys.gameplayCommands);
    int imp = 236, buttons = static_cast<int>(1u << 27);
    CHECK(!SanitizeStockUsercmd(phys, imp, buttons));
    CHECK(imp == 236 && buttons == static_cast<int>(1u << 27));
    CHECK(AllowServerCommand(phys, "l4d2vr_mag_action_v3 1"));
    // Desktop keeps its old behaviour (the existing gates decide).
    const WirePolicy desk = PolicyFor(Effective::Vanilla, 0u, false);
    CHECK(desk.gameplayCommands && AllowServerCommand(desk, "l4d2vr_pose_hello 3"));
}

// The mod never writes a cheat-protected or server-replicated cvar directly.
static void TestConVarPolicy()
{
    using namespace l4d2vr_convar_policy;
    constexpr int kArchive = 1 << 7;
    CHECK(ModMayWrite(0));
    CHECK(ModMayWrite(kArchive));
    CHECK(!ModMayWrite(kFcvarCheat));
    CHECK(!ModMayWrite(kFcvarReplicated));
    CHECK(!ModMayWrite(kFcvarCheat | kArchive));
    CHECK(!ModMayWrite(-1));
    // Stripping FCVAR_ARCHIVE keeps the cheat / replicated bits.
    CHECK(KeepServerOwnedFlags(kFcvarCheat | kArchive, 0) == kFcvarCheat);
    CHECK(KeepServerOwnedFlags(kArchive, 0) == 0);
    CHECK(KeepServerOwnedFlags(kFcvarReplicated, kFcvarCheat) == kFcvarReplicated);
}

int main()
{
    TestConVarPolicy();
    TestParse();
    TestResolveMatrix();
    TestReasons();
    TestLateAckAndSessionReset();
    TestClassicWirePolicy();
    TestReloadGateRelease();
    TestPartialPluginFeatures();
    TestPhysicalAndDesktopUnchanged();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_gameplay_mode_tests: all passed\n");
    return EXIT_SUCCESS;
}

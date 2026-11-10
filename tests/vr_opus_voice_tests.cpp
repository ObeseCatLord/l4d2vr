// Opus voice chat, std-only parts: wire format, settings, VAD, transmit
// gating, jitter buffer with FEC/PLC decisions, negotiation, spatial gains,
// AGC and the server relay (vr_opus_voice.h, server_core/vr_server_voice.h).
#include "../L4D2VR/vr_opus_voice.h"
#include "../L4D2VR/server_core/vr_server_voice.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

static int g_Checks = 0;
static void CheckImpl(bool result, int line, const char* what)
{
    ++g_Checks;
    if (!result) { std::fprintf(stderr, "Check failed at line %d: %s\n", line, what); std::abort(); }
}
#define Check(result) CheckImpl((result), __LINE__, #result)

using namespace l4d2vr_voice;

static Frame MakeFrame(std::uint16_t seq, std::uint8_t spurt, std::uint8_t flags, int size, std::uint8_t fill = 0x5Au)
{
    Frame f;
    f.sequence = seq;
    f.talkspurt = spurt;
    f.flags = flags;
    f.size = static_cast<std::uint8_t>(size);
    for (int i = 0; i < size; ++i) f.data[i] = static_cast<std::uint8_t>(fill + i * 7);
    return f;
}

static void TestBase64()
{
    std::uint32_t rng = 12345u;
    for (int len = 0; len <= kMaxFrameBytes; ++len)
    {
        std::uint8_t in[kMaxFrameBytes], out[kMaxFrameBytes];
        for (int i = 0; i < len; ++i) { rng = rng * 1664525u + 1013904223u; in[i] = static_cast<std::uint8_t>(rng >> 24); }
        char text[kMaxFrameTextChars + 4] = {};
        const int chars = Base64Encode(in, len, text, sizeof(text));
        Check(chars == (len * 4 + 2) / 3);
        for (int i = 0; i < chars; ++i) Check(Base64Value(text[i]) >= 0);
        Check(Base64Decode(text, out, sizeof(out)) == len);
        Check(std::memcmp(in, out, static_cast<std::size_t>(len)) == 0);
    }
    std::uint8_t out[8];
    char small[4];
    const std::uint8_t three[3] = { 1, 2, 3 };
    Check(Base64Encode(three, 3, small, sizeof(small)) == -1);   // needs 5 bytes with the NUL
    Check(Base64Decode("A", out, 8) == -1);                       // impossible length
    Check(Base64Decode("AB+/", out, 8) == -1);                    // standard base64 characters are rejected
    Check(Base64Decode("AB==", out, 8) == -1);                    // no padding
    Check(Base64Decode("AB", out, 8) == -1);                      // non-canonical (low bits set)
    Check(Base64Decode("AA", out, 8) == 1 && out[0] == 0);
    Check(Base64Decode("AAAAAAAAAAAA", out, 8) == -1);           // overflow
}

static void TestFrames()
{
    for (int size = 1; size <= kMaxOpusBytes; size += 13)
    {
        const Frame f = MakeFrame(static_cast<std::uint16_t>(65530 + size), 7, size == 1 ? kFlagStart : 0, size);
        char text[kMaxFrameTextChars + 1] = {};
        Check(EncodeFrameText(f, text, sizeof(text)));
        Check(std::strlen(text) <= static_cast<std::size_t>(kMaxFrameTextChars));
        Frame g;
        Check(DecodeFrameText(text, g));
        Check(g.sequence == f.sequence && g.talkspurt == 7 && g.flags == f.flags && g.size == f.size);
        Check(std::memcmp(g.data, f.data, f.size) == 0);
        // The whole client command fits the engine's ServerCmd buffer ("cmd " + text < 255).
        const std::string command = std::string(kUpCommand) + " " + text;
        Check(command.size() + 4 < 255);
    }
    {
        const Frame full = MakeFrame(1, 1, 0, kMaxOpusBytes);
        char text[kMaxFrameTextChars + 1] = {};
        Check(EncodeFrameText(full, text, sizeof(text)));
        Check(std::strlen(text) == static_cast<std::size_t>(kMaxFrameTextChars));
        Check(std::string(kUpCommand).size() + 1 + std::strlen(text) + 4 < 255);
    }
    // End frames may be empty; nothing else may.
    Frame end = MakeFrame(9, 2, kFlagEnd, 0);
    char text[64];
    Check(EncodeFrameText(end, text, sizeof(text)));
    Frame g;
    Check(DecodeFrameText(text, g) && g.size == 0 && (g.flags & kFlagEnd));
    Frame empty = MakeFrame(9, 2, 0, 0);
    Check(!EncodeFrameText(empty, text, sizeof(text)));
    Frame badFlags = MakeFrame(9, 2, 0x80, 4);
    Check(!EncodeFrameText(badFlags, text, sizeof(text)));
    // Tampered bytes: wrong version, length mismatch, oversize.
    std::uint8_t bytes[kMaxFrameBytes + 8];
    const Frame f = MakeFrame(3, 1, 0, 10);
    const int n = SerializeFrame(f, bytes, sizeof(bytes));
    Check(n == kHeaderBytes + 10);
    Check(ParseFrameBytes(bytes, n, g));
    Check(!ParseFrameBytes(bytes, n - 1, g));
    bytes[0] = 2; Check(!ParseFrameBytes(bytes, n, g)); bytes[0] = 1;
    bytes[5] = kMaxOpusBytes + 1; Check(!ParseFrameBytes(bytes, kHeaderBytes + kMaxOpusBytes + 1, g));
    Check(!DecodeFrameText("not base64!", g));
    std::string tooLong(kMaxFrameTextChars + 4, 'A');
    Check(!DecodeFrameText(tooLong.c_str(), g));
    Check(SequenceDelta(2, 65534) == 4 && SequenceDelta(65534, 2) == -4);
    // Peers command.
    char line[64];
    Check(FormatPeers(PlayerBit(1) | PlayerBit(33) | PlayerBit(64), line, sizeof(line)));
    Check(std::string(line) == "l4d2vr_voice_peers 1 80000001\n");
    std::uint32_t v = 0;
    Check(ParseHex32("80000001", v) && v == 0x80000001u);
    Check(!ParseHex32("123456789", v) && !ParseHex32("", v) && !ParseHex32("x1", v));
    Check(ParseUnsigned("64", 64, v) && v == 64 && !ParseUnsigned("65", 64, v) && !ParseUnsigned("-1", 64, v));
    Check(PlayerBit(0) == 0 && PlayerBit(65) == 0 && PlayerBit(64) == (std::uint64_t{ 1 } << 63));
}

static void TestSettings()
{
    const Settings d = ParseSettingsText("");
    Check(d.enabled && d.bitrate == 32000 && d.fec && d.mode == TransmitMode::FollowGame && d.positional && d.agc);
    Check(!d.loopback && d.testToneHz == 0.0f && d.pushToTalkKey.empty());
    const Settings s = ParseSettingsText(
        "// comment\n"
        "OpusVoiceEnabled=false\n"
        "OpusVoiceBitrate=999999   # clamps to 64 kbps\n"
        "opusvoicemode = vad\n"
        "OpusVoiceVolume=1.5\n"
        "OpusVoiceMicGain=-3\n"
        "OpusVoiceVadSensitivity=0.8\n"
        "OpusVoicePositional=off\n"
        "OpusVoiceRadioVolume=2\n"
        "OpusVoiceSpatialDistanceMeters=1\n"
        "OpusVoiceAgcMaxGain=99\n"
        "OpusVoicePushToTalkKey= V \n"
        "OpusVoiceTestToneHz=440\n"
        "OpusVoiceFec=0\r\n"
        "OpusVoiceExpectedLossPercent=abc\n");
    Check(!s.enabled && s.bitrate == 64000 && s.mode == TransmitMode::VoiceActivation);
    Check(std::fabs(s.volume - 1.5f) < 1e-6f && s.micGain == 0.0f && std::fabs(s.vadSensitivity - 0.8f) < 1e-6f);
    Check(!s.positional && s.radioVolume == 1.0f && s.spatialDistanceMeters == 2.0f && s.agcMaxGain == 16.0f);
    Check(s.pushToTalkKey == "V" && s.testToneHz == 440.0f && !s.fec && s.expectedLossPercent == 5);
    // Later lines win (config2.txt appended after config.txt).
    Check(ParseSettingsText("OpusVoiceBitrate=24000\nOpusVoiceBitrate=16000\n").bitrate == 16000);
    Check(ParseSettingsText("OpusVoiceMode=ptt\n").mode == TransmitMode::PushToTalk);
    Check(ParseSettingsText("OpusVoiceMode=nonsense\n").mode == TransmitMode::FollowGame);
    Check(ParseSettingsText("OpusVoiceMode=game\n").mode == TransmitMode::FollowGame);
    Check(ParseSettingsText("OpusVoiceMode=Auto\n").mode == TransmitMode::FollowGame);
}

// 2.0.10: Opus voice follows the game's voice settings (voice_enable,
// voice_vox, voice_scale, volume).
static void TestGameVoiceSettings()
{
    Settings s;   // OpusVoiceMode=game
    GameVoiceSettings g;
    // Cvars not read yet: push to talk, settings unchanged.
    Settings e = ResolveWithGame(s, g);
    Check(e.enabled && e.mode == TransmitMode::PushToTalk && e.volume == 1.0f);
    // The game's Open mic -> voice activation; Push to talk -> push to talk.
    g.known = true; g.openMic = true;
    Check(ResolveWithGame(s, g).mode == TransmitMode::VoiceActivation);
    g.openMic = false;
    Check(ResolveWithGame(s, g).mode == TransmitMode::PushToTalk);
    // An explicit mode wins over the game's setting.
    Settings ptt = s; ptt.mode = TransmitMode::PushToTalk;
    Settings vad = s; vad.mode = TransmitMode::VoiceActivation;
    g.openMic = true;
    Check(ResolveWithGame(ptt, g).mode == TransmitMode::PushToTalk);
    g.openMic = false;
    Check(ResolveWithGame(vad, g).mode == TransmitMode::VoiceActivation);
    // The game's voice disabled -> Opus off too (and never on when the key is off).
    g.enabled = false;
    Check(!ResolveWithGame(s, g).enabled);
    g.enabled = true;
    Settings off = s; off.enabled = false;
    Check(!ResolveWithGame(off, g).enabled && ResolveWithGame(s, g).enabled);
    // Playback volume: OpusVoiceVolume x voice_scale x volume, clamped.
    g.receiveScale = 0.5f; g.masterVolume = 0.8f;
    Check(std::fabs(ResolveWithGame(s, g).volume - 0.4f) < 1e-5f);
    Settings loud = s; loud.volume = 2.0f;
    g.receiveScale = 5.0f; g.masterVolume = 3.0f;
    Check(ResolveWithGame(loud, g).volume == 2.0f);
    g.receiveScale = -1.0f;
    Check(ResolveWithGame(loud, g).volume == 0.0f);
    GameVoiceSettings a, b;
    Check(SameGameVoice(a, b));
    b.receiveScale = 0.9f;
    Check(!SameGameVoice(a, b));
}

// 2.0.10: a peer's stock voice is dropped only while its Opus voice arrives.
static void TestStockVoiceGate()
{
    double hold = -1e30;
    // A peer that never sent Opus (open mic in the game, Opus waiting for push to talk).
    Check(!DropStockVoice(true, 1000.0, hold));
    // Not a peer: never dropped.
    hold = StockVoiceHoldUntil(false, 1000.0);
    Check(!DropStockVoice(false, 1001.0, hold));
    // Opus arriving: dropped, also across gaps of bursty delivery.
    Check(DropStockVoice(true, 1001.0, hold));
    Check(DropStockVoice(true, 1000.0 + kStockVoiceHoldMs - 1.0, hold));
    // Opus stopped without an End frame (lost): the game's voice plays again.
    Check(!DropStockVoice(true, 1000.0 + kStockVoiceHoldMs + 1.0, hold));
    // The End frame: only the jitter buffer's tail is covered.
    hold = StockVoiceHoldUntil(true, 5000.0);
    Check(DropStockVoice(true, 5000.0 + kStockVoiceEndHoldMs - 1.0, hold));
    Check(!DropStockVoice(true, 5000.0 + kStockVoiceEndHoldMs + 1.0, hold));
}

// 2.0.10: the VR voice defaults run once (vr/config_migrations_applied.txt).
static void TestVrVoiceDefaultsState()
{
    const std::string id = kVrVoiceDefaultsId;
    Check(!MigrationStateHasId("", id));
    const std::string fresh = MigrationStateWithId("", id);
    Check(MigrationStateHasId(fresh, id) && fresh.find("# L4D2VR") == 0);
    const std::string existing = "# header\n2026-11-03_magazine_interaction_on\n2026-11-04_pavlov_toggle_grip";
    const std::string added = MigrationStateWithId(existing, id);
    Check(added == existing + "\n" + id + "\n");
    Check(MigrationStateHasId(added, "2026-11-04_pavlov_toggle_grip") && MigrationStateHasId(added, id));
    Check(MigrationStateWithId(added, id) == added);   // once
    Check(MigrationStateHasId("  " + id + "  # note\r\n", id));
    Check(!MigrationStateHasId("# " + id + "\n", id));
    Check(!MigrationStateHasId(id + "_other\n", id));
    // Open mic, voice on, full receive volume.
    bool vox = false, scale = false, enable = false;
    for (const GameCvarDefault& d : kVrVoiceCvarDefaults)
    {
        const std::string name = d.name, value = d.value;
        if (name == "voice_vox" && value == "1") vox = true;
        if (name == "voice_scale" && value == "1") scale = true;
        if (name == "voice_enable" && value == "1") enable = true;
    }
    Check(vox && scale && enable);
}

static void FillTone(std::int16_t* pcm, int n, double hz, double amp, int offset)
{
    for (int i = 0; i < n; ++i)
        pcm[i] = static_cast<std::int16_t>(amp * std::sin(2.0 * 3.14159265358979 * hz * (offset + i) / kSampleRate));
}

static void FillNoise(std::int16_t* pcm, int n, int amp, std::uint32_t& rng)
{
    for (int i = 0; i < n; ++i) { rng = rng * 1664525u + 1013904223u; pcm[i] = static_cast<std::int16_t>(int(rng >> 16) % (2 * amp + 1) - amp); }
}

static void TestVad()
{
    std::int16_t pcm[kFrameSamples];
    std::uint32_t rng = 7u;
    Vad vad;
    // Steady room noise never opens the gate.
    for (int i = 0; i < 200; ++i) { FillNoise(pcm, kFrameSamples, 150, rng); Check(!vad.Update(pcm, kFrameSamples, 0.55f)); }
    // Speech-level tone opens after two frames.
    FillTone(pcm, kFrameSamples, 300.0, 6000.0, 0);
    Check(!vad.Update(pcm, kFrameSamples, 0.55f));
    Check(vad.Update(pcm, kFrameSamples, 0.55f));
    // Hangover: stays open through 14 quiet frames, closes on the 15th.
    for (int i = 0; i < Vad::kHangoverFrames - 1; ++i) { FillNoise(pcm, kFrameSamples, 150, rng); Check(vad.Update(pcm, kFrameSamples, 0.55f)); }
    FillNoise(pcm, kFrameSamples, 150, rng);
    Check(!vad.Update(pcm, kFrameSamples, 0.55f));
    // Quiet speech: a sensitive gate opens, an insensitive one does not.
    Vad sensitive, deaf;
    for (int i = 0; i < 100; ++i)
    {
        FillNoise(pcm, kFrameSamples, 60, rng);
        sensitive.Update(pcm, kFrameSamples, 1.0f);
        deaf.Update(pcm, kFrameSamples, 0.0f);
    }
    bool openS = false, openD = false;
    for (int i = 0; i < 4; ++i)
    {
        FillTone(pcm, kFrameSamples, 250.0, 450.0, i * kFrameSamples);
        openS = sensitive.Update(pcm, kFrameSamples, 1.0f);
        openD = deaf.Update(pcm, kFrameSamples, 0.0f);
    }
    Check(openS && !openD);
    Check(sensitive.Level() > 0.0f && sensitive.Noise() >= 10.0f);
}

static void TestTransmitter()
{
    Transmitter tx;
    // Push-to-talk: press -> new spurt, encode; release -> 8 tail frames, then End.
    Transmitter::Step s = tx.Update(true, true, true, 3);
    Check(s.resetEncoder && s.encode && s.prerollFrames == 0 && !s.sendEnd && tx.Transmitting());
    Frame f;
    tx.Stamp(f, false);
    Check((f.flags & kFlagStart) && f.talkspurt == 1 && f.sequence == 0);
    s = tx.Update(true, true, true, 3);
    Check(!s.resetEncoder && s.encode);
    tx.Stamp(f, false);
    Check(!(f.flags & kFlagStart) && f.sequence == 1);
    for (int i = 0; i < Transmitter::kPttTailFrames; ++i) { s = tx.Update(false, true, true, 3); Check(s.encode && !s.sendEnd); }
    s = tx.Update(false, true, true, 3);
    Check(!s.encode && s.sendEnd && !tx.Transmitting());
    tx.Stamp(f, true);
    Check(f.flags == kFlagEnd && f.sequence == 2);
    s = tx.Update(false, true, true, 3);
    Check(!s.encode && !s.sendEnd);
    // Voice activation: pre-roll first, no tail.
    s = tx.Update(true, false, true, 3);
    Check(s.resetEncoder && s.prerollFrames == 3 && s.encode);
    tx.Stamp(f, false);
    Check((f.flags & kFlagStart) && f.talkspurt == 2);
    s = tx.Update(false, false, true, 3);
    Check(s.sendEnd && !s.encode);
    // The session closing mid-spurt ends it once.
    tx.Update(true, true, true, 0);
    s = tx.Update(true, true, false, 0);
    Check(s.sendEnd && !s.encode);
    s = tx.Update(true, true, false, 0);
    Check(!s.sendEnd && !s.encode);
}

struct Playout
{
    std::vector<JitterBuffer::Action> actions;
    std::vector<std::uint16_t> sequences;
};

static void Pull(JitterBuffer& jb, double now, Playout& p)
{
    const JitterBuffer::Output o = jb.Next(now);
    p.actions.push_back(o.action);
    p.sequences.push_back(o.frame.sequence);
}

static int CountAction(const Playout& p, JitterBuffer::Action a)
{
    int n = 0;
    for (auto x : p.actions) n += x == a;
    return n;
}

static void TestJitterSteady()
{
    // 50 frames at a steady 20 ms with a fixed 35 ms network delay, then End.
    JitterBuffer jb;
    Playout p;
    double now = 0.0;
    int sent = 0;
    for (int tick = 0; tick < 200; ++tick, now += 10.0)
    {
        const double sendTime = sent * 20.0;
        if (sent <= 50 && now >= sendTime + 35.0)
        {
            jb.Insert(sent < 50 ? MakeFrame(static_cast<std::uint16_t>(1000 + sent), 4, sent == 0 ? kFlagStart : 0, 40)
                                : MakeFrame(static_cast<std::uint16_t>(1000 + sent), 4, kFlagEnd, 0), now);
            ++sent;
        }
        if (tick % 2 == 0) Pull(jb, now, p);
    }
    Check(CountAction(p, JitterBuffer::Action::Decode) == 50);
    Check(CountAction(p, JitterBuffer::Action::End) == 1);
    Check(CountAction(p, JitterBuffer::Action::Conceal) == 0 && CountAction(p, JitterBuffer::Action::DecodeFec) == 0);
    // In order, starting after the 60 ms target.
    std::uint16_t expect = 1000;
    for (std::size_t i = 0; i < p.actions.size(); ++i)
        if (p.actions[i] == JitterBuffer::Action::Decode) Check(p.sequences[i] == expect++);
    Check(!jb.Playing() && jb.GetStats().talkspurts == 1);
    std::size_t firstDecode = 0;
    while (p.actions[firstDecode] != JitterBuffer::Action::Decode) ++firstDecode;
    Check(firstDecode * 20.0 >= 35.0 + JitterBuffer::kMinTargetMs - 20.0);
}

static void TestJitterLoss()
{
    // Frames 5 and 9, 10 are lost: 5 is recovered from 6's FEC, 9 is concealed
    // (10 is not there for FEC) and 10 from 11's FEC.
    JitterBuffer jb;
    for (int i = 0; i < 20; ++i)
        if (i != 5 && i != 9 && i != 10) jb.Insert(MakeFrame(static_cast<std::uint16_t>(i), 1, i == 0 ? kFlagStart : 0, 30), 0.0);
    Playout p;
    for (int tick = 0; tick < 20; ++tick) Pull(jb, 100.0 + tick * 20.0, p);
    Check(p.actions[5] == JitterBuffer::Action::DecodeFec && p.sequences[5] == 6);
    Check(p.actions[6] == JitterBuffer::Action::Decode && p.sequences[6] == 6);
    Check(p.actions[9] == JitterBuffer::Action::Conceal);
    Check(p.actions[10] == JitterBuffer::Action::DecodeFec && p.sequences[10] == 11);
    Check(p.actions[11] == JitterBuffer::Action::Decode && p.sequences[11] == 11);
    Check(jb.GetStats().fec == 2 && jb.GetStats().concealed == 1);
    // The stream stops without its End frame: brief PLC, then the spurt ends.
    Playout q;
    for (int tick = 0; tick < 10; ++tick) Pull(jb, 600.0 + tick * 20.0, q);
    Check(CountAction(q, JitterBuffer::Action::Conceal) == JitterBuffer::kMaxEmptyConceal);
    Check(q.actions[JitterBuffer::kMaxEmptyConceal] == JitterBuffer::Action::End);
    Check(!jb.Playing());
}

static void TestJitterOrderAndDuplicates()
{
    JitterBuffer jb;
    const int order[] = { 0, 2, 1, 3, 3, 5, 4, 6 };
    for (int i : order) jb.Insert(MakeFrame(static_cast<std::uint16_t>(i), 1, i == 0 ? kFlagStart : 0, 20), 0.0);
    Check(jb.GetStats().duplicates == 1 && jb.Count() == 7);
    Playout p;
    for (int tick = 0; tick < 7; ++tick) Pull(jb, 100.0 + tick * 20.0, p);
    for (int i = 0; i < 7; ++i) Check(p.actions[i] == JitterBuffer::Action::Decode && p.sequences[i] == i);
    // A frame arriving after its slot played is dropped as late.
    jb.Insert(MakeFrame(2, 1, 0, 20), 300.0);
    Check(jb.GetStats().late == 1);
}

static void TestJitterBurstAndGap()
{
    // A 400 ms stall then everything at once (reliable channel after loss):
    // the buffer skips ahead instead of keeping 400 ms of extra delay.
    JitterBuffer jb;
    for (int i = 0; i < 5; ++i) jb.Insert(MakeFrame(static_cast<std::uint16_t>(i), 3, i == 0 ? kFlagStart : 0, 20), i * 20.0);
    Playout p;
    for (int tick = 0; tick < 4; ++tick) Pull(jb, 100.0 + tick * 20.0, p);
    for (int i = 5; i < 30; ++i) jb.Insert(MakeFrame(static_cast<std::uint16_t>(i), 3, 0, 20), 560.0);
    Check(jb.TargetMs() > JitterBuffer::kMinTargetMs);   // the spread raised the target
    Pull(jb, 580.0, p);
    Check(jb.GetStats().skipped > 0);
    Check(jb.Count() * kFrameMs <= jb.TargetMs());
    // Many frames lost upstream: jump to the next frame instead of 600 ms of PLC.
    JitterBuffer gap;
    Playout g;
    for (int i = 0; i < 4; ++i) gap.Insert(MakeFrame(static_cast<std::uint16_t>(i), 1, 0, 20), i * 20.0);
    for (int tick = 0; tick < 4; ++tick) Pull(gap, 60.0 + tick * 20.0, g);
    for (int i = 40; i < 44; ++i) gap.Insert(MakeFrame(static_cast<std::uint16_t>(i), 1, 0, 20), 130.0);
    for (int tick = 0; tick < 4; ++tick) Pull(gap, 140.0 + tick * 20.0, g);
    Check(CountAction(g, JitterBuffer::Action::Decode) == 8 && CountAction(g, JitterBuffer::Action::Conceal) == 0);
    Check(g.sequences[4] == 40);
}

static void TestJitterStall()
{
    // Frames 5..7 are held up 60 ms by the network (a burst on the reliable
    // channel): the buffer waits for them (PLC meanwhile) instead of dropping them.
    JitterBuffer jb;
    for (int i = 0; i < 5; ++i) jb.Insert(MakeFrame(static_cast<std::uint16_t>(i), 1, i == 0 ? kFlagStart : 0, 20), i * 20.0);
    Playout p;
    for (int tick = 0; tick < 5; ++tick) Pull(jb, 80.0 + tick * 20.0, p);
    for (int tick = 0; tick < 3; ++tick) Pull(jb, 180.0 + tick * 20.0, p);   // nothing there yet
    for (int i = 5; i < 10; ++i) jb.Insert(MakeFrame(static_cast<std::uint16_t>(i), 1, 0, 20), 240.0);
    for (int tick = 0; tick < 5; ++tick) Pull(jb, 240.0 + tick * 20.0, p);
    Check(CountAction(p, JitterBuffer::Action::Decode) == 10);
    Check(CountAction(p, JitterBuffer::Action::Conceal) == 3 && jb.GetStats().stalls == 3);
    Check(jb.GetStats().late == 0);
    for (std::size_t i = 0, seq = 0; i < p.actions.size(); ++i)
        if (p.actions[i] == JitterBuffer::Action::Decode) Check(p.sequences[i] == seq++);
}

static void TestJitterTalkspurts()
{
    // Spurt 1 ends while spurt 2's first frames are already buffered: both play fully.
    JitterBuffer jb;
    std::vector<std::pair<double, Frame>> events;
    for (int i = 0; i < 6; ++i) events.push_back({ i * 20.0, MakeFrame(static_cast<std::uint16_t>(i), 1, i == 0 ? kFlagStart : 0, 20) });
    events.push_back({ 120.0, MakeFrame(6, 1, kFlagEnd, 0) });
    for (int i = 7; i < 12; ++i) events.push_back({ 140.0 + (i - 7) * 20.0, MakeFrame(static_cast<std::uint16_t>(i), 2, i == 7 ? kFlagStart : 0, 20) });
    Playout p;
    std::size_t next = 0;
    for (double now = 0.0; now < 700.0; now += 10.0)
    {
        while (next < events.size() && events[next].first <= now) { jb.Insert(events[next].second, now); ++next; }
        if (now >= 60.0 && static_cast<int>(now) % 20 == 0) Pull(jb, now, p);
    }
    Check(CountAction(p, JitterBuffer::Action::Decode) == 11);
    Check(CountAction(p, JitterBuffer::Action::End) >= 1);
    Check(jb.GetStats().talkspurts == 2 && jb.GetStats().skipped == 0);
    // After a long silence the next spurt starts clean.
    jb.Insert(MakeFrame(500, 9, kFlagStart, 20), 5000.0);
    Check(!jb.Playing() && jb.Count() == 1);
}

static void TestNegotiation()
{
    ClientNegotiation n;
    ClientNegotiation::Inputs in;
    in.enabled = true;
    in.serverFeature = false;
    Check(n.Update(in) == -1 && !n.Active());
    in.serverFeature = true;
    in.transmit = true;
    in.nowMs = 1000.0;
    Check(n.Update(in) == int(kHelloReceive | kHelloTransmit));
    Check(n.Update(in) == -1);                                     // no repeat at once
    in.nowMs = 2600.0;
    Check(n.Update(in) == int(kHelloReceive | kHelloTransmit));    // retry
    n.OnPeers(1, 0);
    Check(n.Peers() == 0u);                                        // not before the ack
    n.OnAck(1);
    Check(n.Active() && n.TransmitAllowed());
    in.nowMs = 9000.0;
    Check(n.Update(in) == -1);                                     // acked: no retries
    n.OnPeers(0x5u, 0x1u);
    Check(n.DropStockVoiceFrom(1) && !n.DropStockVoiceFrom(2) && n.DropStockVoiceFrom(3) && n.DropStockVoiceFrom(33));
    // Loopback changes the flags: a new hello, the ack stays valid.
    in.loopback = true;
    Check(n.Update(in) == int(kHelloReceive | kHelloTransmit | kHelloLoopback));
    // Microphone lost: receive only.
    in.transmit = false;
    in.loopback = false;
    Check(n.Update(in) == int(kHelloReceive));
    Check(n.Active() && !n.TransmitAllowed());
    // Disabled: one opt-out hello (flags 0), then silence.
    in.enabled = false;
    Check(n.Update(in) == 0 && !n.Active() && !n.DropStockVoiceFrom(1));
    Check(n.Update(in) == -1);
    // Retries stop after kHelloAttempts without an ack.
    ClientNegotiation r;
    in.enabled = true;
    int hellos = 0;
    for (int i = 0; i < 40; ++i) { in.nowMs = 20000.0 + i * 500.0; hellos += r.Update(in) >= 0; }
    // Four quick hellos, then one every 6 s (a lost ack never ends voice for good).
    Check(hellos == ClientNegotiation::kHelloAttempts + 2 && !r.Active());
    // A new server feature ack restarts the handshake.
    r.Restart();
    Check(r.Update(in) == int(kHelloReceive));
    // Old protocol acks are ignored.
    r.OnAck(0);
    Check(!r.Active());
    // A repeated bundle: one more hello, the session stays active.
    r.OnAck(1);
    Check(r.Active());
    in.nowMs += 100.0;
    Check(r.Update(in) == -1);
    r.Refresh();
    Check(r.Active() && r.Update(in) == int(kHelloReceive));
    Check(r.Update(in) == -1 && r.Active());
}

static void TestSpatial()
{
    Settings s;
    const Vec3 listener{ 0.0f, 0.0f, 64.0f };
    // In front at 1 m: centred, full level.
    Gains g = SpatialGains(listener, 0.0f, Vec3{ 39.37f, 0.0f, 64.0f }, s);
    Check(std::fabs(g.left - g.right) < 1e-3f && g.left > 0.95f);
    // To the right (yaw 0 looks along +x; right is -y): louder in the right ear.
    g = SpatialGains(listener, 0.0f, Vec3{ 0.0f, -80.0f, 64.0f }, s);
    Check(g.right > g.left * 2.0f);
    // Turning the head 180 degrees swaps the ears.
    const Gains turned = SpatialGains(listener, 180.0f, Vec3{ 0.0f, -80.0f, 64.0f }, s);
    Check(std::fabs(turned.left - g.right) < 1e-3f && std::fabs(turned.right - g.left) < 1e-3f);
    // Left at yaw 90 (looking along +y, right is +x): speaker at -x is on the left.
    g = SpatialGains(listener, 90.0f, Vec3{ -80.0f, 0.0f, 64.0f }, s);
    Check(g.left > g.right * 2.0f);
    // Farther is quieter, but never below the radio floor; beyond the spatial
    // distance it is a centred radio voice.
    const Gains near = SpatialGains(listener, 0.0f, Vec3{ 2.0f * 39.37f, 0.0f, 64.0f }, s);
    const Gains mid = SpatialGains(listener, 0.0f, Vec3{ 8.0f * 39.37f, 0.0f, 64.0f }, s);
    const Gains far = SpatialGains(listener, 0.0f, Vec3{ 100.0f * 39.37f, 0.0f, 64.0f }, s);
    Check(near.left > mid.left && mid.left >= s.radioVolume * 0.85f - 1e-4f);
    Check(std::fabs(far.left - s.radioVolume) < 1e-3f && std::fabs(far.right - s.radioVolume) < 1e-3f);
    s.positional = false;
    g = SpatialGains(listener, 0.0f, Vec3{ 0.0f, -80.0f, 64.0f }, s);
    Check(g.left == 1.0f && g.right == 1.0f);
}

static void TestAgcAndLimit()
{
    std::int16_t pcm[kFrameSamples];
    Agc agc;
    float rms = 0.0f;
    for (int frame = 0; frame < 120; ++frame)
    {
        FillTone(pcm, kFrameSamples, 220.0, 900.0, frame * kFrameSamples);   // a quiet microphone
        agc.Process(pcm, kFrameSamples, true, 6.0f);
        double sum = 0.0;
        for (int i = 0; i < kFrameSamples; ++i) sum += double(pcm[i]) * pcm[i];
        rms = static_cast<float>(std::sqrt(sum / kFrameSamples));
    }
    Check(rms > 3000.0f && rms < 6000.0f && agc.Gain() > 4.0f && agc.Gain() <= 6.0f + 1e-3f);
    // Loud input is never pushed into clipping.
    Agc loud;
    for (int frame = 0; frame < 20; ++frame)
    {
        FillTone(pcm, kFrameSamples, 220.0, 30000.0, frame * kFrameSamples);
        loud.Process(pcm, kFrameSamples, true, 16.0f);
        for (int i = 0; i < kFrameSamples; ++i) Check(pcm[i] > -32768 && pcm[i] < 32767);
    }
    Check(loud.Gain() < 1.0f);
    // Disabled: untouched.
    FillTone(pcm, kFrameSamples, 220.0, 900.0, 0);
    const std::int16_t before = pcm[100];
    Agc off;
    off.Process(pcm, kFrameSamples, false, 6.0f);
    Check(pcm[100] == before);
    float prev = -10.0f;
    for (float x = -4.0f; x <= 4.0f; x += 0.01f)
    {
        const float y = SoftLimit(x);
        Check(y >= prev && std::fabs(y) < 1.0f);
        prev = y;
    }
    Check(SoftLimit(0.5f) == 0.5f);
}

static void TestTone()
{
    ToneSource tone;
    tone.Configure(440.0);
    std::vector<std::int16_t> pcm(kSampleRate / 2);
    tone.Fill(pcm.data(), static_cast<int>(pcm.size()));
    Check(ToneToNoiseDb(pcm.data(), static_cast<int>(pcm.size()), 440.0) > 30.0);
    std::uint32_t rng = 3u;
    FillNoise(pcm.data(), static_cast<int>(pcm.size()), 8000, rng);
    Check(ToneToNoiseDb(pcm.data(), static_cast<int>(pcm.size()), 440.0) < 10.0);
    // On/off pattern: the off part is near-silent noise.
    ToneSource pulsed;
    pulsed.Configure(440.0, 100.0, 100.0);
    std::vector<std::int16_t> p(kSampleRate / 5);
    pulsed.Fill(p.data(), static_cast<int>(p.size()));
    int loudOn = 0, loudOff = 0;
    for (int i = 0; i < kSampleRate / 10; ++i) loudOn = std::max(loudOn, std::abs(int(p[static_cast<std::size_t>(i)])));
    for (int i = kSampleRate / 10; i < kSampleRate / 5; ++i) loudOff = std::max(loudOff, std::abs(int(p[static_cast<std::size_t>(i)])));
    Check(loudOn > 8000 && loudOff <= 20);
}

// ---- server relay --------------------------------------------------------------------
namespace
{
    struct FakeHost final : l4d2vr_voice_server::RelayHost
    {
        std::map<int, std::vector<std::string>> sent;
        bool connected[kMaxPlayers + 1]{};
        bool deaf[kMaxPlayers + 1][kMaxPlayers + 1]{};   // [receiver][sender]
        std::uint64_t now = 1000u;
        bool Connected(int i) const override { return i >= 1 && i <= kMaxPlayers && connected[i]; }
        void Send(int i, const char* text) override { sent[i].push_back(text); }
        bool CanHear(int r, int s) const override { return !deaf[r][s]; }
        std::uint64_t NowMs() const override { return now; }
        std::vector<std::string> logs;
        void Log(const char* line) override { logs.push_back(line); }
        int Count(int i, const char* prefix) const
        {
            auto it = sent.find(i);
            if (it == sent.end()) return 0;
            int n = 0;
            for (const auto& t : it->second) n += t.rfind(prefix, 0) == 0;
            return n;
        }
    };

    bool Dispatch(l4d2vr_voice_server::Relay& relay, FakeHost& host, int index, std::vector<std::string> args)
    {
        std::vector<const char*> argv;
        for (const auto& a : args) argv.push_back(a.c_str());
        return relay.ClientCommand(host, index, static_cast<int>(argv.size()), argv.data());
    }
}

static void TestRelay()
{
    using l4d2vr_voice_server::Relay;
    FakeHost host;
    for (int i = 1; i <= 4; ++i) host.connected[i] = true;
    Relay relay;
    relay.SetClientMax(8);
    char text[kMaxFrameTextChars + 1] = {};
    Check(EncodeFrameText(MakeFrame(1, 1, kFlagStart, 50), text, sizeof(text)));

    // Disabled (server without the feature): commands are consumed, nothing sent.
    Check(Dispatch(relay, host, 1, { "l4d2vr_voice_hello", "1", "3" }));
    Check(host.sent.empty());
    Check(!Dispatch(relay, host, 1, { "l4d2vr_pose_hello", "2" }));   // not ours
    relay.SetEnabled(true);

    // Player 1 talks and listens; 2 listens; 3 is a vanilla client; 4 listens but cannot hear 1.
    Check(Dispatch(relay, host, 1, { "l4d2vr_voice_hello", "1", "3" }));
    Check(host.Count(1, "l4d2vr_voice_ack 1") == 1);
    Check(host.Count(1, "l4d2vr_voice_peers 1 0") == 1);
    Check(Dispatch(relay, host, 2, { "l4d2vr_voice_hello", "1", "1" }));
    Check(host.Count(2, "l4d2vr_voice_ack 1") == 1 && host.Count(2, "l4d2vr_voice_peers 1 0") == 1);
    Check(host.Count(1, "l4d2vr_voice_peers") == 1);    // the talker set did not change
    Check(Dispatch(relay, host, 4, { "l4d2vr_voice_hello", "1", "1" }));
    host.deaf[4][1] = true;
    Check(relay.TalkerMask() == PlayerBit(1));

    Check(Dispatch(relay, host, 1, { "l4d2vr_voice_up", text }));
    const std::string rx = std::string("l4d2vr_voice_rx 1 ") + text + "\n";
    Check(host.Count(2, rx.c_str()) == 1);
    Check(host.Count(1, "l4d2vr_voice_rx") == 0);        // no loopback asked
    Check(host.Count(3, "l4d2vr_voice_rx") == 0);        // vanilla client never gets Opus
    Check(host.Count(4, "l4d2vr_voice_rx") == 0);        // game rules: cannot hear
    Check(relay.RelayedTotal() == 1 && relay.DeliveredTotal() == 1);
    Check(relay.Client(1)->deliveredFrames == 1u && !relay.Client(1)->loggedUnheard);

    // A listener cannot send; malformed frames are rejected.
    Check(Dispatch(relay, host, 2, { "l4d2vr_voice_up", text }));
    Check(host.Count(1, "l4d2vr_voice_rx") == 0);
    Check(Dispatch(relay, host, 1, { "l4d2vr_voice_up", "!!!" }));
    Check(Dispatch(relay, host, 1, { "l4d2vr_voice_up" }));
    Check(relay.Client(1)->rejectedFrames == 1 && relay.RelayedTotal() == 1);

    // Loopback: the talker hears itself.
    Check(Dispatch(relay, host, 1, { "l4d2vr_voice_hello", "1", "7" }));
    Check(Dispatch(relay, host, 1, { "l4d2vr_voice_up", text }));
    Check(host.Count(1, "l4d2vr_voice_rx 1 ") == 1);

    // Rate limit: a flood is cut to the burst, then refills at 60/s.
    Relay flood;
    flood.SetEnabled(true);
    flood.SetClientMax(8);
    FakeHost h2;
    h2.connected[1] = h2.connected[2] = true;
    Dispatch(flood, h2, 1, { "l4d2vr_voice_hello", "1", "2" });
    Dispatch(flood, h2, 2, { "l4d2vr_voice_hello", "1", "1" });
    for (int i = 0; i < 100; ++i) Dispatch(flood, h2, 1, { "l4d2vr_voice_up", text });
    Check(h2.Count(2, "l4d2vr_voice_rx") == static_cast<int>(Relay::kBurstFrames));
    h2.now += 1000u;
    for (int i = 0; i < 100; ++i) Dispatch(flood, h2, 1, { "l4d2vr_voice_up", text });
    Check(h2.Count(2, "l4d2vr_voice_rx") == static_cast<int>(Relay::kBurstFrames) * 2);
    // Steady 50 frames/s are never limited.
    Relay steady;
    steady.SetEnabled(true);
    FakeHost h3;
    h3.connected[1] = h3.connected[2] = true;
    Dispatch(steady, h3, 1, { "l4d2vr_voice_hello", "1", "2" });
    Dispatch(steady, h3, 2, { "l4d2vr_voice_hello", "1", "1" });
    for (int i = 0; i < 500; ++i) { h3.now += 20u; Dispatch(steady, h3, 1, { "l4d2vr_voice_up", text }); }
    Check(h3.Count(2, "l4d2vr_voice_rx") == 500);

    // 2.0.10 log: copies delivered, and once when no Opus listener may hear the talker.
    int relayedLogs = 0;
    for (const auto& l : h3.logs) relayedLogs += l.find("player 1 Opus voice frames relayed 0 (delivered 0 copies, rejected 0)") != std::string::npos;
    Check(relayedLogs == 1);
    Relay muted;
    muted.SetEnabled(true);
    FakeHost h4;
    h4.connected[1] = h4.connected[2] = true;
    h4.deaf[2][1] = true;
    Dispatch(muted, h4, 1, { "l4d2vr_voice_hello", "1", "3" });
    Dispatch(muted, h4, 2, { "l4d2vr_voice_hello", "1", "3" });
    h4.logs.clear();
    for (int i = 0; i < 3; ++i) { h4.now += 20u; Dispatch(muted, h4, 1, { "l4d2vr_voice_up", text }); }
    Check(h4.Count(2, "l4d2vr_voice_rx") == 0 && muted.Client(1)->loggedUnheard && muted.Client(1)->deliveredFrames == 0u);
    int unheardLogs = 0;
    for (const auto& l : h4.logs) unheardLogs += l.find("cannot hear this player") != std::string::npos;
    Check(unheardLogs == 1);

    // A talker leaving updates everyone's peers; an opt-out (flags 0) too.
    host.sent.clear();
    relay.Reset(host, 1);
    Check(host.Count(2, "l4d2vr_voice_peers 0 0") == 1 && host.Count(4, "l4d2vr_voice_peers 0 0") == 1);
    Check(Dispatch(relay, host, 2, { "l4d2vr_voice_hello", "1", "3" }));
    Check(host.Count(4, "l4d2vr_voice_peers 2 0") == 1);
    Check(Dispatch(relay, host, 2, { "l4d2vr_voice_hello", "1", "0" }));
    Check(host.Count(4, "l4d2vr_voice_peers 0 0") == 2 && !relay.Client(2)->hello);
    // Bad hellos are ignored.
    host.sent.clear();
    Check(Dispatch(relay, host, 3, { "l4d2vr_voice_hello", "0", "1" }));
    Check(Dispatch(relay, host, 3, { "l4d2vr_voice_hello", "1" }));
    Check(Dispatch(relay, host, 9, { "l4d2vr_voice_hello", "1", "1" }));   // beyond maxclients
    Check(host.sent.empty());
}

int main()
{
    TestBase64();
    TestFrames();
    TestSettings();
    TestGameVoiceSettings();
    TestStockVoiceGate();
    TestVrVoiceDefaultsState();
    TestVad();
    TestTransmitter();
    TestJitterSteady();
    TestJitterLoss();
    TestJitterOrderAndDuplicates();
    TestJitterBurstAndGap();
    TestJitterStall();
    TestJitterTalkspurts();
    TestNegotiation();
    TestSpatial();
    TestAgcAndLimit();
    TestTone();
    TestRelay();
    std::printf("vr_opus_voice_tests: %d checks passed\n", g_Checks);
    return 0;
}

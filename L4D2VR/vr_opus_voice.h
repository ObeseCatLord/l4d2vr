#pragma once

// Opus voice chat (2.0.5): the std-only half, shared by the client DLL, the
// dedicated plugin and the unit tests (tests/vr_opus_voice_tests.cpp).
//
// L4D2's own voice codec is fixed (no sv_voicecodec; vaudio_miles/speex at
// 11 kHz), which is why stock voice sounds grainy. Mod clients carry their own
// voice instead: 48 kHz mono Opus, 20 ms frames, through the L4D2VR server
// (dedicated plugin or a modded listen host), feature-gated by OpusVoice
// (0x2000). Players and servers without it keep the game's own voice.
//
// Wire (all text, through the same string-command channel as the pose relay):
//   client -> server  l4d2vr_voice_hello <version> <flags>   opt in (flags: Hello*)
//   server -> client  l4d2vr_voice_ack <version>             relay accepted
//   client -> server  l4d2vr_voice_up <frame>                 one 20 ms frame,
//                     sent unreliably (IVEngineClient::ServerCmd(.., false))
//   server -> client  l4d2vr_voice_rx <player> <frame>        relayed frame
//   server -> client  l4d2vr_voice_peers <lo> <hi>            players (bit i-1)
//                     that may send Opus: while their Opus voice is actually
//                     arriving, their stock voice is dropped by this client
//                     (each is heard once); otherwise it plays (2.0.10)
// <frame> is base64url (no padding) of:
//   [0] version (1)  [1] flags (Start, End)  [2..3] sequence (LE)
//   [4] talkspurt id [5] Opus bytes N        [6..] N Opus bytes (N = 0 only with End)
// IVEngineClient::ServerCmd formats "cmd %s" into 255 bytes, so a voice_up
// command stays below 250 characters: kMaxOpusBytes caps the encoder.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace l4d2vr_voice
{
    constexpr int kSampleRate = 48000;
    constexpr int kFrameMs = 20;
    constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;   // 960
    constexpr unsigned kProtocolVersion = 1u;
    constexpr int kHeaderBytes = 6;
    constexpr int kMaxOpusBytes = 160;          // 64 kbps peak per 20 ms frame
    constexpr int kMaxFrameBytes = kHeaderBytes + kMaxOpusBytes;
    constexpr int kMaxFrameTextChars = (kMaxFrameBytes * 4 + 2) / 3;  // 222
    constexpr int kMaxPlayers = 64;

    constexpr char kHelloCommand[] = "l4d2vr_voice_hello";
    constexpr char kAckCommand[] = "l4d2vr_voice_ack";
    constexpr char kUpCommand[] = "l4d2vr_voice_up";
    constexpr char kRxCommand[] = "l4d2vr_voice_rx";
    constexpr char kPeersCommand[] = "l4d2vr_voice_peers";

    enum FrameFlags : std::uint8_t { kFlagStart = 0x01u, kFlagEnd = 0x02u, kFlagMask = 0x03u };
    enum HelloFlags : std::uint32_t
    {
        kHelloReceive = 0x1u,    // relay other players' Opus voice to me
        kHelloTransmit = 0x2u,   // I send Opus voice (drop my stock voice on Opus clients)
        kHelloLoopback = 0x4u,   // relay my own voice back to me (microphone test, self-test)
        kHelloMask = 0x7u,
    };

    // The engine's ServerCmd buffer is 255 bytes including "cmd ".
    static_assert(sizeof(kUpCommand) - 1 + 1 + kMaxFrameTextChars < 250, "voice_up must fit IVEngineClient::ServerCmd");

    struct Frame
    {
        std::uint16_t sequence = 0u;
        std::uint8_t talkspurt = 0u;
        std::uint8_t flags = 0u;
        std::uint8_t size = 0u;               // Opus bytes
        std::uint8_t data[kMaxOpusBytes]{};
    };

    // Signed distance a - b on the uint16 sequence circle.
    inline int SequenceDelta(std::uint16_t a, std::uint16_t b)
    {
        return static_cast<int>(static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b)));
    }

    // ---- base64url (no padding) -------------------------------------------------
    // Only [A-Za-z0-9_-]: none of these split a Source command (no quotes,
    // semicolons, "//", or the tokenizer break set {}()':).
    inline char Base64Char(unsigned v)
    {
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        return table[v & 63u];
    }

    inline int Base64Value(char c)
    {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    }

    // Returns the characters written (excluding the NUL), or -1 if out is too small.
    inline int Base64Encode(const std::uint8_t* in, int length, char* out, std::size_t capacity)
    {
        if (length < 0 || !out) return -1;
        const std::size_t needed = static_cast<std::size_t>((length * 4 + 2) / 3);
        if (needed + 1 > capacity) return -1;
        std::size_t o = 0;
        int i = 0;
        for (; i + 2 < length; i += 3)
        {
            const unsigned v = (unsigned(in[i]) << 16) | (unsigned(in[i + 1]) << 8) | unsigned(in[i + 2]);
            out[o++] = Base64Char(v >> 18); out[o++] = Base64Char(v >> 12);
            out[o++] = Base64Char(v >> 6); out[o++] = Base64Char(v);
        }
        if (length - i == 1)
        {
            const unsigned v = unsigned(in[i]) << 16;
            out[o++] = Base64Char(v >> 18); out[o++] = Base64Char(v >> 12);
        }
        else if (length - i == 2)
        {
            const unsigned v = (unsigned(in[i]) << 16) | (unsigned(in[i + 1]) << 8);
            out[o++] = Base64Char(v >> 18); out[o++] = Base64Char(v >> 12); out[o++] = Base64Char(v >> 6);
        }
        out[o] = '\0';
        return static_cast<int>(o);
    }

    // Returns the bytes decoded, or -1 on a bad character, a bad length or overflow.
    inline int Base64Decode(const char* text, std::uint8_t* out, int capacity)
    {
        if (!text || !out) return -1;
        const std::size_t n = std::strlen(text);
        if (n % 4 == 1) return -1;
        const int bytes = static_cast<int>(n / 4 * 3 + (n % 4 ? n % 4 - 1 : 0));
        if (bytes > capacity) return -1;
        int o = 0;
        unsigned acc = 0;
        int bits = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const int v = Base64Value(text[i]);
            if (v < 0) return -1;
            acc = (acc << 6) | static_cast<unsigned>(v);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out[o++] = static_cast<std::uint8_t>((acc >> bits) & 0xFFu);
            }
        }
        // Canonical: the unused low bits of the last character are zero.
        if (bits > 0 && (acc & ((1u << bits) - 1u)) != 0u) return -1;
        return o;
    }

    // ---- frame <-> text --------------------------------------------------------------
    inline bool ValidFrame(const Frame& f)
    {
        if ((f.flags & ~kFlagMask) != 0u || f.size > kMaxOpusBytes) return false;
        return f.size > 0u || (f.flags & kFlagEnd) != 0u;
    }

    inline int SerializeFrame(const Frame& f, std::uint8_t* out, int capacity)
    {
        if (!ValidFrame(f) || capacity < kHeaderBytes + f.size) return -1;
        out[0] = static_cast<std::uint8_t>(kProtocolVersion);
        out[1] = f.flags;
        out[2] = static_cast<std::uint8_t>(f.sequence & 0xFFu);
        out[3] = static_cast<std::uint8_t>(f.sequence >> 8);
        out[4] = f.talkspurt;
        out[5] = f.size;
        if (f.size) std::memcpy(out + kHeaderBytes, f.data, f.size);
        return kHeaderBytes + f.size;
    }

    inline bool ParseFrameBytes(const std::uint8_t* in, int length, Frame& f)
    {
        f = Frame{};
        if (!in || length < kHeaderBytes || in[0] != kProtocolVersion) return false;
        f.flags = in[1];
        f.sequence = static_cast<std::uint16_t>(in[2] | (in[3] << 8));
        f.talkspurt = in[4];
        f.size = in[5];
        if (length != kHeaderBytes + f.size || !ValidFrame(f)) return false;
        if (f.size) std::memcpy(f.data, in + kHeaderBytes, f.size);
        return true;
    }

    inline bool EncodeFrameText(const Frame& f, char* out, std::size_t capacity)
    {
        std::uint8_t bytes[kMaxFrameBytes];
        const int n = SerializeFrame(f, bytes, sizeof(bytes));
        return n > 0 && Base64Encode(bytes, n, out, capacity) > 0;
    }

    inline bool DecodeFrameText(const char* text, Frame& f)
    {
        f = Frame{};
        if (!text || std::strlen(text) > static_cast<std::size_t>(kMaxFrameTextChars)) return false;
        std::uint8_t bytes[kMaxFrameBytes];
        const int n = Base64Decode(text, bytes, sizeof(bytes));
        return n > 0 && ParseFrameBytes(bytes, n, f);
    }

    // ---- small parsers (commands) ---------------------------------------------------
    inline bool ParseUnsigned(const char* text, std::uint32_t maximum, std::uint32_t& out)
    {
        out = 0u;
        if (!text || !*text) return false;
        std::uint64_t v = 0u;
        for (const char* p = text; *p; ++p)
        {
            if (*p < '0' || *p > '9') return false;
            v = v * 10u + static_cast<unsigned>(*p - '0');
            if (v > maximum) return false;
        }
        out = static_cast<std::uint32_t>(v);
        return true;
    }

    inline bool ParseHex32(const char* text, std::uint32_t& out)
    {
        out = 0u;
        if (!text || !*text) return false;
        int digits = 0;
        for (const char* p = text; *p; ++p)
        {
            unsigned nibble;
            if (*p >= '0' && *p <= '9') nibble = unsigned(*p - '0');
            else if (*p >= 'a' && *p <= 'f') nibble = unsigned(*p - 'a' + 10);
            else if (*p >= 'A' && *p <= 'F') nibble = unsigned(*p - 'A' + 10);
            else return false;
            if (++digits > 8) return false;
            out = (out << 4) | nibble;
        }
        return true;
    }

    inline std::uint64_t PlayerBit(int playerIndex)
    {
        return (playerIndex >= 1 && playerIndex <= kMaxPlayers) ? (std::uint64_t{ 1 } << (playerIndex - 1)) : 0u;
    }

    inline bool FormatPeers(std::uint64_t mask, char* out, std::size_t capacity)
    {
        const int n = std::snprintf(out, capacity, "%s %x %x\n", kPeersCommand,
            static_cast<unsigned>(mask & 0xFFFFFFFFu), static_cast<unsigned>(mask >> 32));
        return n > 0 && static_cast<std::size_t>(n) < capacity;
    }

    // ---- settings -------------------------------------------------------------------
    // FollowGame (2.0.10 default): the game's own voice setting decides, the
    // way it decides for the game's voice: "Open mic" (voice_vox 1) = voice
    // activation, "Push to talk" (voice_vox 0) = push to talk.
    enum class TransmitMode : std::uint8_t { PushToTalk, VoiceActivation, FollowGame };

    struct Settings
    {
        bool enabled = true;                 // OpusVoiceEnabled
        int bitrate = 32000;                 // OpusVoiceBitrate (bits/s, 12000..64000)
        bool fec = true;                     // OpusVoiceFec (in-band FEC, upstream is unreliable)
        int expectedLossPercent = 5;         // OpusVoiceExpectedLossPercent (FEC strength)
        TransmitMode mode = TransmitMode::FollowGame;   // OpusVoiceMode = game | ptt | vad
        // OpusVoiceVadSensitivity: game (kVadSensitivityGame, 2.0.11 default) = the
        // game's microphone sensitivity (voice_threshold) decides; 0..1 = this value
        // (higher opens on quieter speech). ResolveWithGame turns game into 0..1.
        float vadSensitivity = -1.0f;
        float micGain = 1.0f;                // OpusVoiceMicGain (0..4)
        bool micAgc = true;                  // OpusVoiceMicAgc: raise a quiet microphone before sending (2.0.11)
        float volume = 1.0f;                 // OpusVoiceVolume (0..2)
        bool positional = true;              // OpusVoicePositional
        float radioVolume = 0.55f;           // OpusVoiceRadioVolume: distant talkers (0..1)
        float spatialDistanceMeters = 18.0f; // OpusVoiceSpatialDistanceMeters: fully "radio" from here
        bool agc = true;                     // OpusVoiceAgc
        float agcMaxGain = 6.0f;             // OpusVoiceAgcMaxGain (1..16)
        bool loopback = false;               // OpusVoiceLoopback: hear yourself through the server
        std::string pushToTalkKey;           // OpusVoicePushToTalkKey: extra desktop key ("" = the game's voice key only)
        float testToneHz = 0.0f;             // OpusVoiceTestToneHz: replaces the microphone (testing)
        std::string testDump;                // OpusVoiceTestDump: file prefix for decoded audio (testing)
    };

    inline std::string Lower(std::string s)
    {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }

    inline std::string Trim(const std::string& s)
    {
        std::size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
        return s.substr(b, e - b);
    }

    inline bool ParseBoolText(const std::string& raw, bool fallback)
    {
        const std::string v = Lower(Trim(raw));
        if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
        if (v == "0" || v == "false" || v == "off" || v == "no") return false;
        return fallback;
    }

    inline float ParseFloatText(const std::string& raw, float fallback)
    {
        const std::string v = Trim(raw);
        if (v.empty()) return fallback;
        char* end = nullptr;
        const float f = std::strtof(v.c_str(), &end);
        return (end && *end == '\0' && std::isfinite(f)) ? f : fallback;
    }

    // lookup(key, value) -> true when the key is set. Out-of-range values clamp.
    template <class Lookup>
    Settings ParseSettings(Lookup&& lookup)
    {
        Settings s;
        std::string v;
        auto str = [&](const char* key) -> bool { v.clear(); return lookup(key, v); };
        if (str("OpusVoiceEnabled")) s.enabled = ParseBoolText(v, s.enabled);
        if (str("OpusVoiceBitrate"))
            s.bitrate = static_cast<int>(std::clamp(ParseFloatText(v, float(s.bitrate)), 12000.0f, 64000.0f));
        if (str("OpusVoiceFec")) s.fec = ParseBoolText(v, s.fec);
        if (str("OpusVoiceExpectedLossPercent"))
            s.expectedLossPercent = static_cast<int>(std::clamp(ParseFloatText(v, float(s.expectedLossPercent)), 0.0f, 40.0f));
        if (str("OpusVoiceMode"))
        {
            const std::string m = Lower(Trim(v));
            if (m == "vad" || m == "voice" || m == "voiceactivation" || m == "open") s.mode = TransmitMode::VoiceActivation;
            else if (m == "ptt" || m == "pushtotalk" || m == "push") s.mode = TransmitMode::PushToTalk;
            else if (m == "game" || m == "auto" || m == "follow" || m == "followgame") s.mode = TransmitMode::FollowGame;
        }
        if (str("OpusVoiceVadSensitivity"))
        {
            const std::string m = Lower(Trim(v));
            if (m == "game" || m == "auto" || m == "follow" || m == "followgame") s.vadSensitivity = -1.0f;
            else
            {
                const float f = ParseFloatText(v, s.vadSensitivity);
                s.vadSensitivity = f < 0.0f ? -1.0f : std::min(f, 1.0f);
            }
        }
        if (str("OpusVoiceMicGain")) s.micGain = std::clamp(ParseFloatText(v, s.micGain), 0.0f, 4.0f);
        if (str("OpusVoiceMicAgc")) s.micAgc = ParseBoolText(v, s.micAgc);
        if (str("OpusVoiceVolume")) s.volume = std::clamp(ParseFloatText(v, s.volume), 0.0f, 2.0f);
        if (str("OpusVoicePositional")) s.positional = ParseBoolText(v, s.positional);
        if (str("OpusVoiceRadioVolume")) s.radioVolume = std::clamp(ParseFloatText(v, s.radioVolume), 0.0f, 1.0f);
        if (str("OpusVoiceSpatialDistanceMeters"))
            s.spatialDistanceMeters = std::clamp(ParseFloatText(v, s.spatialDistanceMeters), 2.0f, 200.0f);
        if (str("OpusVoiceAgc")) s.agc = ParseBoolText(v, s.agc);
        if (str("OpusVoiceAgcMaxGain")) s.agcMaxGain = std::clamp(ParseFloatText(v, s.agcMaxGain), 1.0f, 16.0f);
        if (str("OpusVoiceLoopback")) s.loopback = ParseBoolText(v, s.loopback);
        if (str("OpusVoicePushToTalkKey")) s.pushToTalkKey = Trim(v);
        if (str("OpusVoiceTestToneHz")) s.testToneHz = std::clamp(ParseFloatText(v, 0.0f), 0.0f, 8000.0f);
        if (str("OpusVoiceTestDump")) s.testDump = Trim(v);
        return s;
    }

    // VR/config.txt text (key=value, // # ; comments). Later keys win.
    inline Settings ParseSettingsText(const std::string& text)
    {
        std::vector<std::pair<std::string, std::string>> kv;
        std::size_t pos = 0;
        while (pos <= text.size())
        {
            std::size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            for (const char* c : { "//", "#", ";" })
            {
                const std::size_t cut = line.find(c);
                if (cut != std::string::npos) line.erase(cut);
            }
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Trim(line.substr(0, eq));
            if (key.size() >= 3 && static_cast<unsigned char>(key[0]) == 0xEF &&
                static_cast<unsigned char>(key[1]) == 0xBB && static_cast<unsigned char>(key[2]) == 0xBF)
                key = Trim(key.substr(3));
            if (!key.empty()) kv.emplace_back(key, Trim(line.substr(eq + 1)));
            if (nl == text.size()) break;
        }
        return ParseSettings([&](const char* key, std::string& out) {
            bool found = false;
            for (const auto& p : kv)
                if (Lower(p.first) == Lower(key)) { out = p.second; found = true; }
            return found;
        });
    }

    // ---- the game's own voice settings (2.0.10) ----------------------------------------
    // Read from the game's cvars (Options > Audio > Voice), live: Opus voice
    // follows what the player sets there instead of ignoring it.
    struct GameVoiceSettings
    {
        bool known = false;           // the cvars were read (false: the settings alone decide)
        bool enabled = true;          // voice_enable ("Voice communication" Enabled/Disabled)
        bool openMic = false;         // voice_vox ("Open mic" vs "Push to talk")
        float receiveScale = 1.0f;    // voice_scale ("Voice receive volume")
        float masterVolume = 1.0f;    // volume (the game's master volume also scales its voice)
        float micThreshold = -1.0f;   // voice_threshold ("Microphone sensitivity"; < 0 = not read) (2.0.11)
    };

    // ---- microphone sensitivity (2.0.11) ------------------------------------------------
    // The game's Options > Audio "Microphone sensitivity" slider is voice_threshold
    // (resource/ui/l4d360ui/audio.res: SldVoiceVoxThreshold, 1..2000, inverseFill:
    // a fuller bar = a lower threshold = more sensitive). The engine default is
    // 2000, the least sensitive end. engine.dll (open mic) treats a captured buffer
    // as silence when voice_threshold is above both its peak-to-peak range and its
    // highest sample, so it is a 16-bit sample-level threshold: speech at RMS r
    // spans roughly 6r peak to peak, so 2000 needs speech of RMS ~330 and 65 opens
    // on almost anything (the game has no noise tracking at all).
    // Opus voice keeps its adaptive noise floor and maps the slider onto its
    // sensitivity instead: the game's default (2000) = kDefaultVadSensitivity, the
    // most sensitive end (20 and below) = 1, logarithmic in between.
    constexpr float kVadSensitivityGame = -1.0f;
    constexpr float kDefaultVadSensitivity = 0.6f;
    constexpr float kGameMicThresholdMax = 2000.0f;   // engine default, least sensitive
    constexpr float kGameMicThresholdMin = 20.0f;     // and below: most sensitive

    inline float SensitivityFromGameThreshold(float threshold)
    {
        if (!std::isfinite(threshold) || threshold <= 0.0f) return kDefaultVadSensitivity;
        const float t = std::clamp(threshold, kGameMicThresholdMin, kGameMicThresholdMax);
        const float frac = std::log(kGameMicThresholdMax / t) / std::log(kGameMicThresholdMax / kGameMicThresholdMin);
        return kDefaultVadSensitivity + (1.0f - kDefaultVadSensitivity) * frac;
    }

    inline bool SameGameVoice(const GameVoiceSettings& a, const GameVoiceSettings& b)
    {
        return a.known == b.known && a.enabled == b.enabled && a.openMic == b.openMic &&
            std::fabs(a.receiveScale - b.receiveScale) < 1e-3f && std::fabs(a.masterVolume - b.masterVolume) < 1e-3f &&
            std::fabs(a.micThreshold - b.micThreshold) < 1e-2f;
    }

    // The settings in effect: OpusVoiceEnabled and the game's voice switch,
    // OpusVoiceMode=game resolved from the open-mic setting, and the playback
    // volume scaled like the game's voice (voice_scale x volume), and
    // OpusVoiceVadSensitivity=game from the game's microphone sensitivity (2.0.11).
    inline Settings ResolveWithGame(Settings s, const GameVoiceSettings& g)
    {
        if (s.vadSensitivity < 0.0f)
            s.vadSensitivity = (g.known && g.micThreshold > 0.0f) ? SensitivityFromGameThreshold(g.micThreshold)
                                                                  : kDefaultVadSensitivity;
        if (g.known && !g.enabled) s.enabled = false;
        if (s.mode == TransmitMode::FollowGame)
            s.mode = (g.known && g.openMic) ? TransmitMode::VoiceActivation : TransmitMode::PushToTalk;
        if (g.known)
            s.volume = std::clamp(s.volume * std::clamp(g.receiveScale, 0.0f, 2.0f) * std::clamp(g.masterVolume, 0.0f, 1.0f),
                0.0f, 2.0f);
        return s;
    }

    inline const char* ModeName(TransmitMode m)
    {
        return m == TransmitMode::VoiceActivation ? "open mic (voice activation)"
            : m == TransmitMode::PushToTalk ? "push to talk" : "follows the game";
    }

    // ---- stock voice of Opus talkers (2.0.10) ------------------------------------------
    // A peer (hello with transmit) is heard through Opus only while its Opus
    // frames actually arrive. 2.0.5-2.0.9 dropped a peer's stock voice whenever
    // it was a peer, so an open-mic talker (stock voice on, Opus waiting for
    // push-to-talk) was heard by no one with the mod. Now its stock voice is
    // dropped only from its first Opus frame until a moment after the last.
    constexpr double kStockVoiceHoldMs = 1500.0;    // after an Opus frame (covers bursty arrival)
    constexpr double kStockVoiceEndHoldMs = 400.0;  // after the End frame (the jitter buffer's tail)

    inline double StockVoiceHoldUntil(bool endFrame, double nowMs)
    {
        return nowMs + (endFrame ? kStockVoiceEndHoldMs : kStockVoiceHoldMs);
    }

    inline bool DropStockVoice(bool peer, double nowMs, double holdUntilMs)
    {
        return peer && nowMs < holdUntilMs;
    }

    // ---- VR voice defaults, once (2.0.10) ------------------------------------------------
    // The first VR session after 2.0.10 sets the game's voice to open mic at
    // full receive volume, once (recorded in vr/config_migrations_applied.txt);
    // what the player sets afterwards in the game's menu is kept. Desktop
    // players keep the game's defaults.
    constexpr char kVrVoiceDefaultsId[] = "2026-11-09_vr_voice_open_mic";
    struct GameCvarDefault { const char* name; const char* value; };
    constexpr GameCvarDefault kVrVoiceCvarDefaults[] = {
        { "voice_enable", "1" },      // voice on
        { "voice_modenable", "1" },   // the server's voice manager lets this client hear others
        { "voice_vox", "1" },         // open mic (no button to hold in a headset)
        { "voice_scale", "1" },       // receive volume at the menu's maximum (the game's voice is quiet)
    };

    // vr/config_migrations_applied.txt: one id per line, '#' comments.
    inline bool MigrationStateHasId(const std::string& text, const std::string& id)
    {
        std::size_t pos = 0;
        while (pos <= text.size())
        {
            std::size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            const std::size_t hash = line.find('#');
            if (hash != std::string::npos) line.erase(hash);
            if (Trim(line) == id) return true;
            if (nl == text.size()) break;
            pos = nl + 1;
        }
        return false;
    }

    inline std::string MigrationStateWithId(std::string text, const std::string& id)
    {
        if (MigrationStateHasId(text, id)) return text;
        if (text.empty()) text = "# L4D2VR one-shot config migrations. Do not edit unless you want a migration to run again.\n";
        if (text.back() != '\n') text += '\n';
        return text + id + "\n";
    }

    // ---- voice activation (energy gate, adaptive noise floor) ------------------------
    // Opens after two loud frames (40 ms), closes after kHangoverFrames quiet
    // frames; the caller keeps kPrerollFrames so the word onset is sent too.
    // 2.0.11 retune for quiet microphones (Quest over WiVRn into Proton: speech
    // at RMS ~150-400): the gate opens at 3x (sensitivity 1) .. 5x (0) the noise
    // floor and above an absolute floor of 400 (0) .. 20 (1) RMS, logarithmic
    // (66 at the default 0.6). The noise floor follows the minimum frame level of
    // the last kNoiseWindowFrames (minimum statistics): the short pauses between
    // words keep it at the room's level while someone talks, and steady noise
    // (a fan, hum) has no pauses, so the floor rises to it and the gate closes.
    class Vad
    {
    public:
        static constexpr int kOpenFrames = 2;
        static constexpr int kHangoverFrames = 20;     // 400 ms
        static constexpr int kPrerollFrames = 5;       // 100 ms
        static constexpr int kNoiseWindowFrames = 60;  // 1.2 s
        static constexpr float kCloseFraction = 0.55f; // closes below this part of the open level
        static constexpr float kMinNoise = 5.0f;

        static float OpenRatio(float sensitivity) { return 5.0f - 2.0f * std::clamp(sensitivity, 0.0f, 1.0f); }
        static float AbsoluteFloor(float sensitivity) { return 400.0f * std::pow(0.05f, std::clamp(sensitivity, 0.0f, 1.0f)); }

        void Reset() { *this = Vad{}; }

        static float FrameRms(const std::int16_t* pcm, int n)
        {
            double sum = 0.0;
            for (int i = 0; i < n; ++i) sum += double(pcm[i]) * double(pcm[i]);
            return n > 0 ? static_cast<float>(std::sqrt(sum / n)) : 0.0f;
        }

        // sensitivity 0..1 (higher opens on quieter speech). Returns the gate.
        bool Update(const std::int16_t* pcm, int n, float sensitivity)
        {
            const float rms = FrameRms(pcm, n);
            m_Rms = rms;
            m_Level = m_Level * 0.7f + rms * 0.3f;
            if (m_Noise < 0.0f)
            {
                m_Noise = std::max(rms, kMinNoise);
                m_Window.fill(rms);
            }
            m_Window[static_cast<std::size_t>(m_WindowPos)] = rms;
            m_WindowPos = (m_WindowPos + 1) % kNoiseWindowFrames;
            float windowMin = rms;
            for (float w : m_Window) windowMin = std::min(windowMin, w);

            m_OpenAt = std::max(m_Noise * OpenRatio(sensitivity), AbsoluteFloor(sensitivity));
            m_CloseAt = m_OpenAt * kCloseFraction;
            m_JustOpened = false;
            if (!m_Active)
            {
                m_Loud = rms > m_OpenAt ? m_Loud + 1 : 0;
                if (m_Loud >= kOpenFrames) { m_Active = true; m_Quiet = 0; ++m_Talkspurts; m_JustOpened = true; }
            }
            else
            {
                if (rms < m_CloseAt) { if (++m_Quiet >= kHangoverFrames) { m_Active = false; m_Loud = 0; } }
                else m_Quiet = 0;
            }
            // Down at once on a quieter frame, up toward the window minimum (~1 s).
            if (rms < m_Noise) m_Noise += (rms - m_Noise) * 0.25f;
            else if (windowMin > m_Noise) m_Noise += (windowMin - m_Noise) * 0.02f;
            m_Noise = std::clamp(m_Noise, kMinNoise, 8000.0f);
            return m_Active;
        }

        bool Active() const { return m_Active; }
        float Level() const { return m_Level; }
        float Noise() const { return m_Noise; }
        float Rms() const { return m_Rms; }          // the last frame
        float OpenAt() const { return m_OpenAt; }
        float CloseAt() const { return m_CloseAt; }
        std::uint32_t Talkspurts() const { return m_Talkspurts; }
        bool JustOpened() const { return m_JustOpened; }   // the gate opened on the last frame
        // The last frame is speech: the gate is open and the frame is above its close level.
        bool SpeechFrame() const { return m_Active && m_Rms >= m_CloseAt; }

    private:
        float m_Noise = -1.0f, m_Level = 0.0f, m_Rms = 0.0f, m_OpenAt = 0.0f, m_CloseAt = 0.0f;
        std::array<float, kNoiseWindowFrames> m_Window{};
        int m_WindowPos = 0;
        int m_Loud = 0, m_Quiet = 0;
        bool m_Active = false, m_JustOpened = false;
        std::uint32_t m_Talkspurts = 0u;
    };

    // ---- microphone AGC (sent voice, 2.0.11) ---------------------------------------------
    // Raises a quiet microphone toward kTarget before encoding (never lowers a
    // loud one; at most kMaxGain). The level is learnt from speech frames only
    // (Observe while the VAD calls the frame speech), so background noise never
    // drives the gain up; Apply is used on frames that are sent, the gain
    // ramps across the frame and is limited so the peak never clips.
    class MicAgc
    {
    public:
        static constexpr float kTarget = 3000.0f;   // RMS ~ -21 dBFS speech
        static constexpr float kMaxGain = 4.0f;     // +12 dB

        void Reset() { *this = MicAgc{}; }
        void Observe(float speechRms)
        {
            if (speechRms <= 1.0f) return;
            m_Level = m_Level <= 0.0f ? speechRms
                : m_Level + (speechRms - m_Level) * (speechRms > m_Level ? 0.2f : 0.03f);
            const float want = std::clamp(kTarget / m_Level, 1.0f, kMaxGain);
            m_Gain += (want - m_Gain) * (want < m_Gain ? 0.3f : 0.05f);
        }
        void Apply(std::int16_t* pcm, int n)
        {
            if (n <= 0) return;
            int peak = 1;
            for (int i = 0; i < n; ++i) peak = std::max(peak, std::abs(int(pcm[i])));
            const float limit = std::max(1.0f, 30000.0f / float(peak));
            const float gain = std::min(m_Gain, limit);
            const float from = std::min(m_Applied, limit);   // a sudden loud frame is not ramped into clipping
            for (int i = 0; i < n; ++i)
            {
                const float g = from + (gain - from) * float(i + 1) / float(n);
                pcm[i] = static_cast<std::int16_t>(std::clamp(static_cast<int>(std::lround(pcm[i] * g)), -32768, 32767));
            }
            m_Applied = gain;
        }
        float Gain() const { return m_Gain; }
    private:
        float m_Level = 0.0f, m_Gain = 1.0f, m_Applied = 1.0f;
    };

    // ---- microphone level statistics (logged every 10 s, 2.0.11) -------------------------
    struct MicLevelStats
    {
        std::uint32_t frames = 0, openFrames = 0, speechFrames = 0, clippedFrames = 0, talkspurts = 0;
        double rmsSum = 0.0, speechRmsSum = 0.0;
        float speechPeak = 0.0f;
        float noise = 0.0f, openAt = 0.0f, sensitivity = 0.0f, agcGain = 1.0f;

        // One captured frame after the VAD ran on it.
        void Add(const Vad& vad, bool clipped, float sens, float gain)
        {
            ++frames;
            rmsSum += vad.Rms();
            if (vad.Active()) ++openFrames;
            if (vad.JustOpened()) ++talkspurts;
            if (vad.SpeechFrame()) { ++speechFrames; speechRmsSum += vad.Rms(); speechPeak = std::max(speechPeak, vad.Rms()); }
            if (clipped) ++clippedFrames;
            noise = vad.Noise();
            openAt = vad.OpenAt();
            sensitivity = sens;
            agcGain = gain;
        }
        float AverageRms() const { return frames ? static_cast<float>(rmsSum / frames) : 0.0f; }
        float SpeechAverageRms() const { return speechFrames ? static_cast<float>(speechRmsSum / speechFrames) : 0.0f; }
        float OpenPercent() const { return frames ? 100.0f * float(openFrames) / float(frames) : 0.0f; }
    };

    // A frame that reached (or nearly reached) full scale: the microphone or its gain clips.
    inline bool FrameClipped(const std::int16_t* pcm, int n)
    {
        for (int i = 0; i < n; ++i) if (pcm[i] >= 32700 || pcm[i] <= -32700) return true;
        return false;
    }

    // ---- transmit state (gate -> frames to send) --------------------------------------
    // Push-to-talk keeps sending kPttTailFrames after the release (the last
    // syllable); voice activation sends the pre-roll before the first frame.
    class Transmitter
    {
    public:
        static constexpr int kPttTailFrames = 8;   // 160 ms

        struct Step
        {
            bool resetEncoder = false;   // a new talkspurt: OPUS_RESET_STATE first
            int prerollFrames = 0;       // encode this many pre-roll frames (oldest first) before `encode`
            bool encode = false;         // encode the current frame
            bool sendEnd = false;        // send an empty End frame (talkspurt over)
        };

        // gate = push-to-talk held or the VAD open; allowed = the session may send.
        Step Update(bool gate, bool pushToTalkMode, bool allowed, int prerollAvailable)
        {
            Step s;
            if (!allowed)
            {
                if (m_Transmitting) { s.sendEnd = true; m_Transmitting = false; }
                m_Tail = 0;
                return s;
            }
            if (gate) m_Tail = pushToTalkMode ? kPttTailFrames : 0;
            const bool sending = gate || (pushToTalkMode && m_Tail > 0);
            if (!gate && m_Tail > 0) --m_Tail;
            if (sending && !m_Transmitting)
            {
                m_Transmitting = true;
                if (++m_Talkspurt == 0u) m_Talkspurt = 1u;
                m_FirstOfSpurt = true;
                s.resetEncoder = true;
                s.prerollFrames = pushToTalkMode ? 0 : std::clamp(prerollAvailable, 0, Vad::kPrerollFrames);
                s.encode = true;
            }
            else if (sending)
            {
                s.encode = true;
            }
            else if (m_Transmitting)
            {
                s.sendEnd = true;
                m_Transmitting = false;
            }
            return s;
        }

        // Stamps a frame about to be sent (sequence, talkspurt, Start flag).
        void Stamp(Frame& f, bool end)
        {
            f.sequence = m_Sequence++;
            f.talkspurt = m_Talkspurt;
            f.flags = end ? static_cast<std::uint8_t>(kFlagEnd) : std::uint8_t{ 0 };
            if (!end && m_FirstOfSpurt) { f.flags |= kFlagStart; m_FirstOfSpurt = false; }
        }

        bool Transmitting() const { return m_Transmitting; }
        std::uint8_t Talkspurt() const { return m_Talkspurt; }

    private:
        bool m_Transmitting = false;
        bool m_FirstOfSpurt = false;
        int m_Tail = 0;
        std::uint16_t m_Sequence = 0u;
        std::uint8_t m_Talkspurt = 0u;
    };

    // ---- jitter buffer ---------------------------------------------------------------
    // Pull-driven: the playback clock calls Next() once per 20 ms of audio it
    // needs, so the device clock paces playout. Arrival times only decide when
    // a talkspurt starts and how deep the buffer is kept (adaptive target).
    class JitterBuffer
    {
    public:
        static constexpr int kCapacity = 24;          // 480 ms
        static constexpr int kMinTargetMs = 60;
        static constexpr int kMaxTargetMs = 300;
        static constexpr int kMaxEmptyConceal = 8;    // 160 ms waiting with nothing buffered, then stop
        static constexpr double kSilenceResetMs = 600.0;

        enum class Action { None, Decode, DecodeFec, Conceal, End };
        struct Output
        {
            Action action = Action::None;
            Frame frame{};   // Decode: the frame; DecodeFec: the NEXT frame (its FEC carries the missing one)
        };
        struct Stats
        {
            std::uint32_t inserted = 0, duplicates = 0, late = 0, overflow = 0;
            std::uint32_t decoded = 0, fec = 0, concealed = 0, skipped = 0, talkspurts = 0, underruns = 0, stalls = 0;
        };

        void Reset()
        {
            m_Count = 0;
            m_Playing = false;
            m_EmptyConceal = 0;
            m_HaveSpurt = false;
        }

        void Insert(const Frame& f, double nowMs)
        {
            if (!ValidFrame(f)) return;
            if (!m_HaveSpurt || nowMs - m_LastArrival > kSilenceResetMs)
            {
                // First frame, or after a long silence: start clean.
                Reset();
                BeginSpurt(f, nowMs);
            }
            else if (f.talkspurt != m_Spurt)
            {
                // The next talkspurt: sequence numbers continue across spurts, so
                // frames of the old one still buffered keep their order.
                BeginSpurt(f, nowMs);
            }
            m_LastArrival = nowMs;
            // Arrival spread relative to the sender's 20 ms clock -> target delay.
            const double transit = nowMs - SequenceDelta(f.sequence, m_SpurtBaseSeq) * double(kFrameMs);
            m_TransitMin = std::min(m_TransitMin, transit);
            m_TransitMax = std::max(m_TransitMax, transit);
            const int spread = static_cast<int>(m_TransitMax - m_TransitMin);
            const int wanted = std::clamp(spread + kFrameMs, kMinTargetMs, kMaxTargetMs);
            if (wanted > m_TargetMs) m_TargetMs = wanted;   // grows at once; shrinks between talkspurts

            if (m_Playing && SequenceDelta(f.sequence, m_Expected) < 0) { ++m_Stats.late; return; }
            for (int i = 0; i < m_Count; ++i)
                if (m_Frames[i].sequence == f.sequence) { ++m_Stats.duplicates; return; }
            if (m_Count == kCapacity) { ++m_Stats.overflow; DropOldest(); }
            int at = m_Count;
            while (at > 0 && SequenceDelta(m_Frames[at - 1].sequence, f.sequence) > 0) { m_Frames[at] = m_Frames[at - 1]; --at; }
            m_Frames[at] = f;
            ++m_Count;
            ++m_Stats.inserted;
        }

        Output Next(double nowMs)
        {
            Output out;
            if (!m_Playing)
            {
                if (m_Count == 0) return out;
                // Start once the first frame waited the target delay, or the buffer holds it.
                const bool waited = nowMs - m_FirstArrival >= m_TargetMs;
                const bool deep = m_Count * kFrameMs >= m_TargetMs;
                if (!waited && !deep) return out;
                m_Playing = true;
                m_Expected = m_Frames[0].sequence;
                m_EmptyConceal = 0;
            }
            // Far too much buffered (a burst after a stall): skip ahead to the target.
            if (m_Count * kFrameMs > m_TargetMs + 6 * kFrameMs)
            {
                while (m_Count > 1 && m_Count * kFrameMs > m_TargetMs)
                {
                    DropOldest();
                    ++m_Stats.skipped;
                }
                m_Expected = m_Frames[0].sequence;
            }
            // A long gap (many frames lost upstream): jump to the next frame
            // rather than conceal it all (PLC fades to silence after ~100 ms anyway).
            if (m_Count > 0 && SequenceDelta(m_Frames[0].sequence, m_Expected) > kMaxEmptyConceal)
            {
                m_Stats.skipped += static_cast<std::uint32_t>(SequenceDelta(m_Frames[0].sequence, m_Expected));
                m_Expected = m_Frames[0].sequence;
            }
            const int found = Find(m_Expected);
            if (found >= 0)
            {
                out.frame = m_Frames[found];
                Remove(found);
                m_EmptyConceal = 0;
                ++m_Expected;
                if (out.frame.size == 0u && (out.frame.flags & kFlagEnd))
                {
                    out.action = Action::End;
                    FinishSpurt();
                    return out;
                }
                out.action = Action::Decode;
                ++m_Stats.decoded;
                return out;
            }
            if (m_Count > 0)
            {
                // Missing frame with later ones buffered: FEC from the next one, else PLC.
                const int next = Find(static_cast<std::uint16_t>(m_Expected + 1u));
                if (next >= 0)
                {
                    out.action = Action::DecodeFec;
                    out.frame = m_Frames[next];
                    ++m_Stats.fec;
                }
                else
                {
                    out.action = Action::Conceal;
                    ++m_Stats.concealed;
                }
                m_Impairment = std::min(m_Impairment + 1, 4);
                ++m_Expected;
                m_EmptyConceal = 0;
                // Drop frames that can no longer play in order.
                for (int i = 0; i < m_Count;)
                    if (SequenceDelta(m_Frames[i].sequence, m_Expected) < 0) Remove(i); else ++i;
                return out;
            }
            // Nothing buffered: conceal briefly (late frames), then stop.
            ++m_Stats.underruns;
            if (++m_EmptyConceal > kMaxEmptyConceal)
            {
                out.action = Action::End;
                FinishSpurt();
                return out;
            }
            // The expected frame is late, not lost (nothing after it arrived):
            // hold the playout position, so it still plays when it comes (the
            // delay grows by this much instead of the frame being dropped).
            out.action = Action::Conceal;
            ++m_Stats.concealed;
            ++m_Stats.stalls;
            m_Impairment = std::min(m_Impairment + 1, 4);
            return out;
        }

        bool Playing() const { return m_Playing; }
        int Count() const { return m_Count; }
        int TargetMs() const { return m_TargetMs; }
        const Stats& GetStats() const { return m_Stats; }

    private:
        int Find(std::uint16_t seq) const
        {
            for (int i = 0; i < m_Count; ++i) if (m_Frames[i].sequence == seq) return i;
            return -1;
        }
        void Remove(int i)
        {
            for (int j = i + 1; j < m_Count; ++j) m_Frames[j - 1] = m_Frames[j];
            --m_Count;
        }
        void DropOldest() { if (m_Count > 0) Remove(0); }
        void BeginSpurt(const Frame& f, double nowMs)
        {
            m_HaveSpurt = true;
            m_Spurt = f.talkspurt;
            m_FirstArrival = nowMs;
            m_SpurtBaseSeq = f.sequence;
            m_TransitMin = 1e30;
            m_TransitMax = -1e30;
            ++m_Stats.talkspurts;
        }
        void FinishSpurt()
        {
            // Adapt for the next talkspurt: rough ones keep a deeper buffer.
            m_TargetMs = m_Impairment ? std::min(kMaxTargetMs, std::max(m_TargetMs, kMinTargetMs + 20 * m_Impairment))
                                      : std::max(kMinTargetMs, m_TargetMs - 20);
            m_Impairment = 0;
            m_Playing = false;
            m_EmptyConceal = 0;
            // Frames still buffered belong to the next talkspurt (it starts after
            // its own target delay); anything older can no longer play.
            for (int i = 0; i < m_Count;)
                if (SequenceDelta(m_Frames[i].sequence, m_Expected) < 0) Remove(i); else ++i;
            if (m_Count == 0) m_HaveSpurt = false;
        }

        Frame m_Frames[kCapacity]{};
        int m_Count = 0;
        bool m_Playing = false;
        bool m_HaveSpurt = false;
        std::uint8_t m_Spurt = 0u;
        std::uint16_t m_Expected = 0u;
        std::uint16_t m_SpurtBaseSeq = 0u;
        double m_FirstArrival = 0.0, m_LastArrival = -1e30;
        double m_TransitMin = 1e30, m_TransitMax = -1e30;
        int m_TargetMs = kMinTargetMs;
        int m_Impairment = 0;
        int m_EmptyConceal = 0;
        Stats m_Stats{};
    };

    // ---- negotiation (client) -------------------------------------------------------------
    class ClientNegotiation
    {
    public:
        static constexpr int kHelloAttempts = 4;           // quick attempts, then
        static constexpr double kHelloRetryMs = 1500.0;
        static constexpr double kHelloSlowRetryMs = 6000.0;  // keep asking slowly (a lost ack never ends voice)

        struct Inputs
        {
            bool enabled = false;        // OpusVoiceEnabled and in a game
            bool serverFeature = false;  // OpusVoice in l4d2vr_feature_ack
            bool transmit = false;       // a microphone (or test tone) is usable
            bool loopback = false;
            // 2.0.11: other players' Opus voice can be played without hearing
            // them twice (the stock voice filter is installed). Without it the
            // hello leaves kHelloReceive out: everyone is heard once, through
            // the game's voice.
            bool receive = true;
            double nowMs = 0.0;
        };

        void Reset() { *this = ClientNegotiation{}; }

        // Returns the hello flags to send now, or -1. Flags 0 = opt out.
        int Update(const Inputs& in)
        {
            const bool receive = in.receive || in.loopback;   // loopback needs the relay to send to us
            const std::uint32_t flags = (in.enabled && in.serverFeature)
                ? ((receive ? kHelloReceive : 0u) | (in.transmit ? kHelloTransmit : 0u) | (in.loopback ? kHelloLoopback : 0u)) : 0u;
            const bool want = flags != 0u;
            if (!want)
            {
                m_Acked = false;
                m_Peers = 0u;
                if (m_SentFlags > 0) { m_SentFlags = 0; m_Attempts = 0; return 0; }   // opt out once
                m_SentFlags = -1;
                return -1;
            }
            if (static_cast<int>(flags) != m_SentFlags || m_Refresh)
            {
                m_Refresh = false;
                m_SentFlags = static_cast<int>(flags);
                m_Attempts = 1;
                m_LastHelloMs = in.nowMs;
                return static_cast<int>(flags);
            }
            if (!m_Acked && in.nowMs - m_LastHelloMs >= (m_Attempts < kHelloAttempts ? kHelloRetryMs : kHelloSlowRetryMs))
            {
                ++m_Attempts;
                m_LastHelloMs = in.nowMs;
                return static_cast<int>(flags);
            }
            return -1;
        }

        // A new server feature ack (map change, reconnect), or a client stall
        // long enough to be a map change (2.0.11): handshake again.
        void Restart() { m_SentFlags = -1; m_Acked = false; m_Attempts = 0; m_Peers = 0u; }
        // The same server repeated its capability bundle: say hello again (the
        // relay may have been reset) but keep the session running meanwhile.
        void Refresh() { if (m_SentFlags > 0) m_Refresh = true; }

        void OnAck(std::uint32_t version) { if (version >= kProtocolVersion && m_SentFlags > 0) m_Acked = true; }
        void OnPeers(std::uint32_t lo, std::uint32_t hi) { if (m_Acked) m_Peers = (std::uint64_t{ hi } << 32) | lo; }

        bool Active() const { return m_Acked && m_SentFlags > 0; }
        bool TransmitAllowed() const { return Active() && (m_SentFlags & kHelloTransmit) != 0; }
        std::uint64_t Peers() const { return Active() ? m_Peers : 0u; }
        // Stock voice from this player (1-based) duplicates its Opus voice.
        bool DropStockVoiceFrom(int playerIndex) const { return (Peers() & PlayerBit(playerIndex)) != 0u; }
        int SentFlags() const { return m_SentFlags; }

    private:
        int m_SentFlags = -1;
        bool m_Refresh = false;
        int m_Attempts = 0;
        double m_LastHelloMs = 0.0;
        bool m_Acked = false;
        std::uint64_t m_Peers = 0u;
    };

    // ---- microphone device policy (2.0.11) -----------------------------------------------
    // When the audio thread opens or closes the microphone.
    // * A failed waveInOpen is retried every kCaptureFailedRetryMs while the
    //   relay is active (was: never again in this process).
    // * Push to talk: the microphone opens on the first press and closes
    //   kPttIdleCloseMs after the last one (the game's own voice records only
    //   while the key is down; an always-open mic shows as "in use" in Windows
    //   and forces a Bluetooth headset into its hands-free profile).
    // * Open mic: open while the relay allows transmitting.
    struct CapturePolicy
    {
        static constexpr double kRetryMs = 5000.0;
        static constexpr double kCaptureFailedRetryMs = 15000.0;
        static constexpr double kPttIdleCloseMs = 45000.0;
        static constexpr double kMinOpenMs = 2000.0;   // a probe that worked waits for the new ack

        struct Inputs
        {
            bool active = false;           // the relay acked our hello
            bool transmitAllowed = false;  // ... with kHelloTransmit
            bool captureFailed = false;    // the last waveInOpen failed
            bool pushToTalk = false;       // transmit mode
            bool pttHeld = false;
            bool open = false;             // the device is open now
            double nowMs = 0.0;
        };
        enum class Action { None, Open, Close };

        Action Update(const Inputs& in)
        {
            if (in.pttHeld) m_LastPressMs = in.nowMs;
            bool want = false;
            if (!in.active) want = false;
            else if (in.captureFailed) want = true;   // keep probing (headset mic appears later)
            else if (!in.transmitAllowed) want = false;
            else if (in.pushToTalk) want = in.pttHeld || (m_LastPressMs > -1e29 && in.nowMs - m_LastPressMs < kPttIdleCloseMs);
            else want = true;
            if (want && !in.open && in.nowMs >= m_RetryAtMs)
            {
                m_RetryAtMs = in.nowMs + (in.captureFailed ? kCaptureFailedRetryMs : kRetryMs);
                m_OpenedMs = in.nowMs;
                return Action::Open;
            }
            if (!want && in.open && (!in.active || in.nowMs - m_OpenedMs >= kMinOpenMs)) return Action::Close;
            return Action::None;
        }
        // The open attempt failed: next try after the failure back-off.
        void OpenFailed(double nowMs) { m_RetryAtMs = nowMs + kCaptureFailedRetryMs; }
        void Reset() { *this = CapturePolicy{}; }

    private:
        double m_RetryAtMs = 0.0;
        double m_OpenedMs = -1e30;
        double m_LastPressMs = -1e30;
    };

    // ---- playback device health (2.0.11) ---------------------------------------------------
    // A talker's game voice may only be dropped while its Opus voice can really
    // be played: waveOut is open and returned a buffer recently. Without a
    // device (timer clock) or with a stalled one, the game's voice plays.
    constexpr double kOutputHealthyMs = 500.0;     // a buffer completed this recently
    constexpr double kOutputStallMs = 1500.0;      // no buffer completed for this long: reopen
    constexpr double kOutputRetryMs = 10000.0;     // no device: try again this often

    inline bool OutputPlaying(bool deviceOpen, double nowMs, double lastDoneMs)
    {
        return deviceOpen && nowMs - lastDoneMs < kOutputHealthyMs;
    }
    inline bool OutputStalled(bool deviceOpen, double nowMs, double lastDoneMs)
    {
        return deviceOpen && nowMs - lastDoneMs > kOutputStallMs;
    }

    // ---- spatial playback --------------------------------------------------------------
    struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
    struct Gains { float left = 1.0f, right = 1.0f; };

    constexpr float kUnitsPerMeter = 39.37f;

    // Source axes: yaw 0 looks along +x, the listener's right is (sin yaw, -cos yaw).
    // Near talkers are panned (constant power, at most 70% to one side) and
    // attenuated with distance; from spatialDistanceMeters on a talker is a
    // centred "radio" voice at radioVolume, so a teammate across the map stays
    // audible (L4D2's own voice is not positional at all).
    inline Gains SpatialGains(const Vec3& listener, float listenerYawDeg, const Vec3& speaker, const Settings& s)
    {
        if (!s.positional) return { 1.0f, 1.0f };
        const float dx = speaker.x - listener.x, dy = speaker.y - listener.y, dz = speaker.z - listener.z;
        const float distUnits = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float meters = distUnits / kUnitsPerMeter;
        const float yaw = listenerYawDeg * 3.14159265f / 180.0f;
        const float horizontal = std::sqrt(dx * dx + dy * dy);
        float pan = horizontal > 1.0f ? (dx * std::sin(yaw) - dy * std::cos(yaw)) / horizontal : 0.0f;   // +1 right
        pan = std::clamp(pan, -1.0f, 1.0f) * 0.7f;
        const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;      // 0..pi/2
        const float panL = std::cos(angle) * 1.41421356f, panR = std::sin(angle) * 1.41421356f;
        // Full level within 1.5 m, then a gentle roll-off (half at ~7.5 m).
        const float positional = 1.0f / (1.0f + std::max(0.0f, meters - 1.5f) / 6.0f);
        const float blend = std::clamp(meters / std::max(s.spatialDistanceMeters, 1.0f), 0.0f, 1.0f);
        Gains g;
        g.left = (1.0f - blend) * positional * panL + blend * s.radioVolume;
        g.right = (1.0f - blend) * positional * panR + blend * s.radioVolume;
        // Never quieter than the radio level (teammates must stay intelligible).
        const float floorLevel = s.radioVolume * 0.85f;
        g.left = std::max(g.left, floorLevel * (pan > 0.0f ? 1.0f - pan * 0.5f : 1.0f));
        g.right = std::max(g.right, floorLevel * (pan < 0.0f ? 1.0f + pan * 0.5f : 1.0f));
        return g;
    }

    // ---- automatic gain control (received voice) -------------------------------------
    // Brings each talker to one speech level whatever their microphone: the
    // level follows speech frames only (above a floor), falls fast, rises slowly,
    // ramps across the frame and never clips.
    class Agc
    {
    public:
        void Reset() { *this = Agc{}; }
        void Process(std::int16_t* pcm, int n, bool enabled, float maxGain)
        {
            if (!enabled || n <= 0) { m_Applied = 1.0f; return; }
            constexpr float kTarget = 5000.0f, kFloor = 300.0f;   // RMS ~ -16 dBFS speech, ~ -41 dBFS floor
            double sum = 0.0;
            int peak = 1;
            for (int i = 0; i < n; ++i) { sum += double(pcm[i]) * pcm[i]; peak = std::max(peak, std::abs(int(pcm[i]))); }
            const float rms = static_cast<float>(std::sqrt(sum / n));
            if (rms > kFloor)
            {
                m_Level = m_Level <= 0.0f ? rms : m_Level + (rms - m_Level) * (rms > m_Level ? 0.3f : 0.05f);
                const float want = std::clamp(kTarget / m_Level, 0.5f, std::max(1.0f, maxGain));
                m_Gain += (want - m_Gain) * (want < m_Gain ? 0.3f : 0.05f);
            }
            const float gain = std::min(m_Gain, 32000.0f / float(peak));
            const float from = m_Applied;
            for (int i = 0; i < n; ++i)
            {
                const float g = from + (gain - from) * float(i + 1) / float(n);
                pcm[i] = static_cast<std::int16_t>(std::clamp(static_cast<int>(std::lround(pcm[i] * g)), -32768, 32767));
            }
            m_Applied = gain;
        }
        float Gain() const { return m_Applied; }
    private:
        float m_Level = 0.0f, m_Gain = 1.0f, m_Applied = 1.0f;
    };

    // ---- mixing ---------------------------------------------------------------------------
    // Soft limiter for the summed mix (float, 1.0 = full scale): linear to 0.75,
    // then a smooth knee that approaches 1.0.
    inline float SoftLimit(float x)
    {
        const float a = std::fabs(x);
        if (a <= 0.75f) return x;
        const float over = a - 0.75f;
        const float y = 0.75f + 0.25f * (over / (over + 0.25f));
        return x < 0.0f ? -y : y;
    }

    // ---- test signal and analysis --------------------------------------------------------------
    class ToneSource
    {
    public:
        void Configure(double hz, double onMs = 0.0, double offMs = 0.0) { m_Hz = hz; m_OnMs = onMs; m_OffMs = offMs; }
        bool Enabled() const { return m_Hz > 0.0; }
        void Fill(std::int16_t* out, int n)
        {
            const double period = m_OnMs + m_OffMs;
            for (int i = 0; i < n; ++i, ++m_Sample)
            {
                const double t = double(m_Sample) / kSampleRate;
                const bool on = period <= 0.0 || std::fmod(t * 1000.0, period) < m_OnMs;
                out[i] = on ? static_cast<std::int16_t>(9000.0 * std::sin(2.0 * 3.14159265358979 * m_Hz * t))
                            : static_cast<std::int16_t>((static_cast<int>(m_Sample * 7919u) % 41) - 20);
            }
        }
    private:
        double m_Hz = 0.0, m_OnMs = 0.0, m_OffMs = 0.0;
        std::uint64_t m_Sample = 0u;
    };

    // Goertzel power of one frequency (normalized by n^2).
    inline double TonePower(const std::int16_t* pcm, int n, double hz)
    {
        if (n <= 0) return 0.0;
        const double w = 2.0 * 3.14159265358979 * hz / kSampleRate;
        const double c = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double s0 = pcm[i] / 32768.0 + c * s1 - s2;
            s2 = s1; s1 = s0;
        }
        return (s1 * s1 + s2 * s2 - c * s1 * s2) / (double(n) * n);
    }

    // Tone versus the strongest of a few off-tone probes, in dB.
    inline double ToneToNoiseDb(const std::int16_t* pcm, int n, double hz)
    {
        const double tone = TonePower(pcm, n, hz);
        double other = 1e-12;
        for (double f : { hz * 0.5, hz * 1.37, hz * 2.11, hz * 2.9, 3100.0, 6200.0 })
            if (std::fabs(f - hz) > 40.0) other = std::max(other, TonePower(pcm, n, f));
        return 10.0 * std::log10(std::max(tone, 1e-12) / other);
    }
}

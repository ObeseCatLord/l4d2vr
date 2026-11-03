#pragma once

// Opus voice chat, client runtime (Windows / Proton): microphone capture with
// waveIn, Opus encode, the handshake with the server relay, per-talker jitter
// buffers and Opus decoding with FEC/PLC, and a stereo mix played with waveOut
// at each talker's head position. One audio thread owns the devices and the
// codecs; the game thread feeds it (OpusVoiceClient::Frame from CreateMove,
// the l4d2vr_voice_* server commands) and sends the encoded frames.
// The std-only logic lives in vr_opus_voice.h.

#include <cstdint>
#include <functional>

#include "vr_opus_voice.h"

namespace OpusVoiceClient
{
    struct Stats
    {
        bool active = false;          // the server acked the voice relay
        bool transmitAllowed = false;
        bool captureOpen = false;     // microphone (or test tone) running
        bool outputOpen = false;      // waveOut playing (false = timer clock, no device)
        bool transmitting = false;
        std::uint32_t framesSent = 0, bytesSent = 0, talkspurtsSent = 0;
        std::uint32_t framesReceived = 0, framesRejected = 0;
        std::uint32_t decoded = 0, fec = 0, concealed = 0;
        std::uint32_t mixedBlocks = 0, stockVoiceDropped = 0, stockVoicePassed = 0;
        std::uint64_t peers = 0u;
        float inputLevel = 0.0f;      // VAD level (RMS, 16-bit units)
        int lastJitterTargetMs = 0;
        double maxArrivalGapMs = 0.0;   // longest wait between two received frames of one talkspurt (since the last stats line)
        double loopbackToneDb = -100.0;   // tone vs off-tone of this client's own voice played back (loopback)
        std::uint32_t loopbackSamples = 0;
    };

    struct FrameInputs
    {
        bool inGame = false;
        int localPlayer = 0;                  // 1-based
        std::uint32_t serverFeatures = 0u;    // l4d2vr_feature_ack mask (0 = plain server)
        l4d2vr_voice::Vec3 listener{};        // head position (Source units)
        float listenerYawDeg = 0.0f;          // head yaw
        bool gameWindowFocused = true;        // for the desktop push-to-talk key
        // Head position of a talker (1-based); false when unknown (centred).
        std::function<bool(int player, l4d2vr_voice::Vec3& head)> speakerHead;
        // Sends a string command to the server (unreliable).
        std::function<void(const char* command, bool reliable)> serverCommand;
    };

    void ApplySettings(const l4d2vr_voice::Settings& settings);
    l4d2vr_voice::Settings CurrentSettings();
    // Desktop mode (no VR config parse): read the OpusVoice* keys from
    // VR/config.txt and VR/config2.txt (rechecked every few seconds).
    void PollDesktopSettings();

    // Game thread, once per frame while connected.
    void Frame(const FrameInputs& in);
    // The server's feature ack arrived (new map or server): handshake again.
    void OnServerFeatures(std::uint32_t mask);
    // Disconnect or relay reset.
    void OnSessionReset();

    // l4d2vr_voice_ack / _peers / _rx.
    void OnAck(std::uint32_t version);
    void OnPeers(std::uint32_t lo, std::uint32_t hi);
    void OnFrame(int player, const char* text);

    // Push-to-talk sources: the game's +voicerecord (desktop voice key and the
    // VR voice combo), the OpenXR VoicePushToTalk action, the self-test.
    void SetStockVoiceRecord(bool held);
    void SetControllerPushToTalk(bool held);
    void SetSelfTestPushToTalk(int state);   // -1 = not overriding, 0/1 = forced
    // Self-test: replace the microphone with a tone and loop our voice back.
    void SetSelfTestTone(double hz, double onMs, double offMs);   // hz 0 = off
    void SetSelfTestLoopback(int state);     // -1 = settings, 0/1 = forced
    void SetSelfTestMode(int state);         // -1 = settings, 0 = ptt, 1 = vad
    void ResetLoopbackAnalysis();

    // Stock voice (CClientState::ProcessVoiceData) from this sender (0-based
    // client slot) duplicates its Opus voice: drop it.
    bool ShouldDropStockVoice(int senderClientSlot);
    // Hooks engine.dll's ProcessVoiceData (after MH_Initialize); once.
    bool InstallStockVoiceFilter();
    void NoteStockVoiceDropped();

    bool IsTalking(int player);   // 1-based; the local player = transmitting
    Stats GetStats();
    void Shutdown();
}

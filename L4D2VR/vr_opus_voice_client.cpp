// Opus voice chat, client runtime (see vr_opus_voice_client.h).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmsystem.h>

#include "vr_opus_voice_client.h"

#include <opus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "game.h"
#include "MinHook.h"

#pragma comment(lib, "Winmm.lib")

namespace
{
    using namespace l4d2vr_voice;

    constexpr int kBlockFrames = kFrameSamples / 2;   // 10 ms output blocks
    constexpr int kCaptureBuffers = 8;                 // 8 x 20 ms
    constexpr int kOutputBuffers = 5;                  // 5 x 10 ms queued (~50 ms)
    constexpr double kTalkingHoldMs = 250.0;

    double NowMs()
    {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    struct Speaker
    {
        // Shared (g_Mutex).
        JitterBuffer jitter;
        float targetLeft = 1.0f, targetRight = 1.0f;
        double lastArrivalMs = -1e30;
        double lastAudioMs = -1e30;
        bool used = false;
        // Audio thread only.
        OpusDecoder* decoder = nullptr;
        std::vector<std::int16_t> pcm;   // decoded, not yet mixed
        Agc agc;
        float appliedLeft = 1.0f, appliedRight = 1.0f;
        FILE* dump = nullptr;
    };

    std::mutex g_Mutex;
    Settings g_Settings;                               // g_Mutex
    std::array<Speaker, kMaxPlayers + 1> g_Speakers;   // g_Mutex for the shared fields
    std::deque<std::string> g_Outgoing;                // g_Mutex
    OpusVoiceClient::Stats g_Stats;                    // g_Mutex
    std::vector<std::int16_t> g_LoopbackPcm;           // g_Mutex

    // Game thread only.
    ClientNegotiation g_Negotiation;
    std::uint32_t g_ServerFeatures = 0u;
    bool g_FeaturesValid = false;        // a feature ack arrived in this server session
    double g_LastFrameMs = 0.0, g_LastFeatureAckMs = 0.0;
    double g_LastStatsLogMs = 0.0;
    double g_DesktopSettingsCheckMs = -1e30;
    std::string g_DesktopSettingsStamp;
    bool g_SettingsFromVR = false;

    // Cross-thread flags.
    std::atomic<bool> g_PttStock{ false }, g_PttController{ false }, g_PttKey{ false };
    std::atomic<int> g_PttSelfTest{ -1 }, g_LoopbackSelfTest{ -1 }, g_ModeSelfTest{ -1 };
    std::atomic<double> g_ToneHz{ 0.0 }, g_ToneOnMs{ 0.0 }, g_ToneOffMs{ 0.0 };
    std::atomic<std::uint32_t> g_ToneGeneration{ 0u };
    std::atomic<bool> g_Active{ false }, g_TransmitAllowed{ false }, g_CaptureFailed{ false };
    std::atomic<bool> g_Transmitting{ false };
    std::atomic<std::uint64_t> g_Peers{ 0u };
    std::atomic<int> g_LocalPlayer{ 0 };
    std::atomic<std::uint32_t> g_StockDropped{ 0u };
    std::atomic<std::uint32_t> g_StockPassed{ 0u };

    std::thread g_Thread;
    std::atomic<bool> g_ThreadRun{ false };
    HANDLE g_WakeEvent = nullptr;
    std::once_flag g_StartOnce;

    Settings SettingsSnapshot()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_Settings;
    }

    bool LoopbackWanted(const Settings& s)
    {
        const int forced = g_LoopbackSelfTest.load();
        return forced >= 0 ? forced != 0 : s.loopback;
    }

    TransmitMode ModeWanted(const Settings& s)
    {
        const int forced = g_ModeSelfTest.load();
        return forced >= 0 ? (forced ? TransmitMode::VoiceActivation : TransmitMode::PushToTalk) : s.mode;
    }

    double ToneHz(const Settings& s)
    {
        const double forced = g_ToneHz.load();
        return forced > 0.0 ? forced : s.testToneHz;
    }

    bool PushToTalkHeld()
    {
        const int forced = g_PttSelfTest.load();
        if (forced >= 0) return forced != 0;
        return g_PttStock.load() || g_PttController.load() || g_PttKey.load();
    }

    // ---- audio thread -------------------------------------------------------------------
    class AudioThread
    {
    public:
        void Run()
        {
            m_CaptureEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            m_OutputEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            while (g_ThreadRun.load())
            {
                const Settings s = SettingsSnapshot();
                const bool want = s.enabled && g_Active.load();
                if (!want)
                {
                    CloseAll();
                    WaitForSingleObject(g_WakeEvent, 100);
                    continue;
                }
                const double now = NowMs();
                UpdateEncoder(s);
                const bool wantCapture = g_TransmitAllowed.load();
                const double toneHz = ToneHz(s);
                if (toneHz > 0.0)
                {
                    if (m_WaveIn) CloseCapture();
                    if (!m_ToneActive || m_ToneGeneration != g_ToneGeneration.load())
                    {
                        m_Tone = ToneSource{};
                        m_Tone.Configure(toneHz, g_ToneOnMs.load(), g_ToneOffMs.load());
                        m_ToneGeneration = g_ToneGeneration.load();
                        m_ToneStartMs = now;
                        m_ToneSamples = 0;
                        m_ToneActive = true;
                        Game::logMsg("[VR][Voice] test tone %.0f Hz (%.0f ms on / %.0f ms off) replaces the microphone",
                            toneHz, g_ToneOnMs.load(), g_ToneOffMs.load());
                    }
                }
                else
                {
                    m_ToneActive = false;
                    if (wantCapture && !m_WaveIn && now >= m_CaptureRetryMs) OpenCapture(now);
                    if (!wantCapture && m_WaveIn) CloseCapture();
                }
                if (!m_WaveOut && !m_TimerClock && now >= m_OutputRetryMs) OpenOutput(now);

                HANDLE handles[3] = { g_WakeEvent, m_CaptureEvent, m_OutputEvent };
                WaitForMultipleObjects(3, handles, FALSE, 5);

                ProcessCapture(s);
                ProcessOutput(s);
                PublishDeviceStats();
            }
            CloseAll();
            for (Speaker& sp : g_Speakers)
            {
                if (sp.decoder) opus_decoder_destroy(sp.decoder);
                sp.decoder = nullptr;
                if (sp.dump) std::fclose(sp.dump);
                sp.dump = nullptr;
            }
            if (m_Encoder) opus_encoder_destroy(m_Encoder);
            m_Encoder = nullptr;
            if (m_MixDump) std::fclose(m_MixDump);
            m_MixDump = nullptr;
            CloseHandle(m_CaptureEvent);
            CloseHandle(m_OutputEvent);
        }

    private:
        // ---- encoder ----
        void UpdateEncoder(const Settings& s)
        {
            if (!m_Encoder)
            {
                int error = 0;
                m_Encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
                if (!m_Encoder)
                {
                    Game::logMsg("[VR][Voice] opus_encoder_create failed (%d)", error);
                    return;
                }
                opus_encoder_ctl(m_Encoder, OPUS_SET_VBR(1));
                opus_encoder_ctl(m_Encoder, OPUS_SET_VBR_CONSTRAINT(0));
                opus_encoder_ctl(m_Encoder, OPUS_SET_COMPLEXITY(10));
                opus_encoder_ctl(m_Encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
                opus_encoder_ctl(m_Encoder, OPUS_SET_DTX(0));
                m_EncBitrate = -1;
            }
            const int loss = s.fec ? std::max(1, s.expectedLossPercent) : 0;
            if (m_EncBitrate != s.bitrate || m_EncFec != s.fec || m_EncLoss != loss)
            {
                opus_encoder_ctl(m_Encoder, OPUS_SET_BITRATE(s.bitrate));
                opus_encoder_ctl(m_Encoder, OPUS_SET_INBAND_FEC(s.fec ? 1 : 0));
                opus_encoder_ctl(m_Encoder, OPUS_SET_PACKET_LOSS_PERC(loss));
                m_EncBitrate = s.bitrate;
                m_EncFec = s.fec;
                m_EncLoss = loss;
                Game::logMsg("[VR][Voice] Opus encoder: 48 kHz mono, %d bit/s VBR, FEC %s (%d%% loss), 20 ms frames",
                    s.bitrate, s.fec ? "on" : "off", loss);
            }
        }

        // ---- capture ----
        void OpenCapture(double now)
        {
            m_CaptureRetryMs = now + 5000.0;
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = 1;
            format.nSamplesPerSec = kSampleRate;
            format.wBitsPerSample = 16;
            format.nBlockAlign = 2;
            format.nAvgBytesPerSec = kSampleRate * 2;
            HWAVEIN handle = nullptr;
            const MMRESULT r = waveInOpen(&handle, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(m_CaptureEvent), 0,
                CALLBACK_EVENT);
            if (r != MMSYSERR_NOERROR || !handle)
            {
                if (!g_CaptureFailed.exchange(true))
                    Game::logMsg("[VR][Voice] no microphone (waveInOpen %u); receive only", static_cast<unsigned>(r));
                return;
            }
            m_WaveIn = handle;
            for (int i = 0; i < kCaptureBuffers; ++i)
            {
                m_CaptureData[i].assign(kFrameSamples, 0);
                WAVEHDR& h = m_CaptureHeaders[i];
                h = WAVEHDR{};
                h.lpData = reinterpret_cast<LPSTR>(m_CaptureData[i].data());
                h.dwBufferLength = kFrameSamples * 2;
                waveInPrepareHeader(m_WaveIn, &h, sizeof(h));
                waveInAddBuffer(m_WaveIn, &h, sizeof(h));
            }
            waveInStart(m_WaveIn);
            g_CaptureFailed.store(false);
            Game::logMsg("[VR][Voice] microphone open (default recording device, 48 kHz mono)");
        }

        void CloseCapture()
        {
            if (!m_WaveIn) return;
            waveInStop(m_WaveIn);
            waveInReset(m_WaveIn);
            for (WAVEHDR& h : m_CaptureHeaders)
                if (h.dwFlags & WHDR_PREPARED) waveInUnprepareHeader(m_WaveIn, &h, sizeof(h));
            waveInClose(m_WaveIn);
            m_WaveIn = nullptr;
            m_CapturePending.clear();
        }

        void ProcessCapture(const Settings& s)
        {
            std::int16_t pcm[kFrameSamples];
            if (m_ToneActive)
            {
                // Synthetic microphone paced by the wall clock.
                const double due = (NowMs() - m_ToneStartMs) * kSampleRate / 1000.0;
                for (int n = 0; n < 10 && double(m_ToneSamples + kFrameSamples) <= due; ++n)
                {
                    m_Tone.Fill(pcm, kFrameSamples);
                    m_ToneSamples += kFrameSamples;
                    CaptureFrame(s, pcm);
                }
                if (due - double(m_ToneSamples) > kSampleRate) m_ToneSamples = static_cast<std::uint64_t>(due);   // after a hitch
                return;
            }
            if (!m_WaveIn) { if (g_Transmitting.load()) CaptureFrame(s, nullptr); return; }
            for (WAVEHDR& h : m_CaptureHeaders)
            {
                if (!(h.dwFlags & WHDR_DONE)) continue;
                const auto* samples = reinterpret_cast<const std::int16_t*>(h.lpData);
                m_CapturePending.insert(m_CapturePending.end(), samples, samples + h.dwBytesRecorded / 2);
                h.dwFlags &= ~WHDR_DONE;
                waveInAddBuffer(m_WaveIn, &h, sizeof(h));
            }
            // A backlog (hitch) is dropped rather than sent late.
            if (m_CapturePending.size() > static_cast<std::size_t>(kFrameSamples * 10))
                m_CapturePending.erase(m_CapturePending.begin(), m_CapturePending.end() - kFrameSamples);
            while (m_CapturePending.size() >= static_cast<std::size_t>(kFrameSamples))
            {
                std::copy_n(m_CapturePending.begin(), kFrameSamples, pcm);
                m_CapturePending.erase(m_CapturePending.begin(), m_CapturePending.begin() + kFrameSamples);
                CaptureFrame(s, pcm);
            }
        }

        // pcm == nullptr: no new audio (device lost) -> only end the talkspurt.
        void CaptureFrame(const Settings& s, std::int16_t* pcm)
        {
            const bool allowed = g_TransmitAllowed.load() && m_Encoder && pcm;
            const bool ptt = ModeWanted(s) == TransmitMode::PushToTalk;
            bool gate = false;
            if (pcm)
            {
                if (s.micGain != 1.0f)
                    for (int i = 0; i < kFrameSamples; ++i)
                        pcm[i] = static_cast<std::int16_t>(std::clamp(static_cast<int>(pcm[i] * s.micGain), -32768, 32767));
                const bool vad = m_Vad.Update(pcm, kFrameSamples, s.vadSensitivity);
                gate = ptt ? PushToTalkHeld() : vad;
            }
            const Transmitter::Step step = m_Tx.Update(gate, ptt, allowed, static_cast<int>(m_Preroll.size()));
            if (step.resetEncoder) opus_encoder_ctl(m_Encoder, OPUS_RESET_STATE);
            const int skip = static_cast<int>(m_Preroll.size()) - step.prerollFrames;
            for (int i = 0; i < static_cast<int>(m_Preroll.size()); ++i)
                if (i >= skip) Encode(m_Preroll[static_cast<std::size_t>(i)].data());
            if (step.resetEncoder) m_Preroll.clear();
            if (step.encode) Encode(pcm);
            if (step.sendEnd) QueueEnd();
            if (!step.encode && pcm)
            {
                m_Preroll.emplace_back(pcm, pcm + kFrameSamples);
                if (m_Preroll.size() > static_cast<std::size_t>(Vad::kPrerollFrames)) m_Preroll.pop_front();
            }
            g_Transmitting.store(m_Tx.Transmitting());
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_Stats.inputLevel = m_Vad.Level();
            if (step.resetEncoder) ++g_Stats.talkspurtsSent;
        }

        void Encode(const std::int16_t* pcm)
        {
            if (!m_Encoder || !pcm) return;
            Frame f;
            const int n = opus_encode(m_Encoder, pcm, kFrameSamples, f.data, kMaxOpusBytes);
            if (n <= 0) return;
            f.size = static_cast<std::uint8_t>(n);
            m_Tx.Stamp(f, false);
            Queue(f);
        }

        void QueueEnd()
        {
            Frame f;
            m_Tx.Stamp(f, true);
            Queue(f);
        }

        void Queue(const Frame& f)
        {
            char text[kMaxFrameTextChars + 1];
            if (!EncodeFrameText(f, text, sizeof(text))) return;
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_Outgoing.emplace_back(text);
            // The game thread sends every frame; a long stall drops the oldest.
            while (g_Outgoing.size() > 25u) g_Outgoing.pop_front();
            ++g_Stats.framesSent;
            g_Stats.bytesSent += f.size;
        }

        // ---- output ----
        void OpenOutput(double now)
        {
            m_OutputRetryMs = now + 10000.0;
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = 2;
            format.nSamplesPerSec = kSampleRate;
            format.wBitsPerSample = 16;
            format.nBlockAlign = 4;
            format.nAvgBytesPerSec = kSampleRate * 4;
            HWAVEOUT handle = nullptr;
            const MMRESULT r = waveOutOpen(&handle, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(m_OutputEvent), 0,
                CALLBACK_EVENT);
            if (r != MMSYSERR_NOERROR || !handle)
            {
                // No playback device: mix on a timer (stats, loopback analysis, dumps still work).
                Game::logMsg("[VR][Voice] no playback device (waveOutOpen %u); mixing on a timer", static_cast<unsigned>(r));
                m_TimerClock = true;
                m_TimerMixMs = now;
                return;
            }
            m_WaveOut = handle;
            for (int i = 0; i < kOutputBuffers; ++i)
            {
                m_OutputData[i].assign(kBlockFrames * 2, 0);
                WAVEHDR& h = m_OutputHeaders[i];
                h = WAVEHDR{};
                h.lpData = reinterpret_cast<LPSTR>(m_OutputData[i].data());
                h.dwBufferLength = kBlockFrames * 4;
                waveOutPrepareHeader(m_WaveOut, &h, sizeof(h));
                Mix(SettingsSnapshot(), m_OutputData[i].data());
                waveOutWrite(m_WaveOut, &h, sizeof(h));
            }
            Game::logMsg("[VR][Voice] playback open (default device, 48 kHz stereo, %d ms queued)", kOutputBuffers * 10);
        }

        void CloseOutput()
        {
            if (m_WaveOut)
            {
                waveOutReset(m_WaveOut);
                for (WAVEHDR& h : m_OutputHeaders)
                    if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(m_WaveOut, &h, sizeof(h));
                waveOutClose(m_WaveOut);
                m_WaveOut = nullptr;
            }
            m_TimerClock = false;
        }

        void CloseAll()
        {
            CloseCapture();
            CloseOutput();
            m_ToneActive = false;
            if (m_Tx.Transmitting())
            {
                Transmitter fresh;
                m_Tx = fresh;
            }
            g_Transmitting.store(false);
            m_Vad.Reset();
            m_Preroll.clear();
            m_CaptureRetryMs = 0.0;
            m_OutputRetryMs = 0.0;
        }

        void ProcessOutput(const Settings& s)
        {
            if (m_WaveOut)
            {
                for (int i = 0; i < kOutputBuffers; ++i)
                {
                    WAVEHDR& h = m_OutputHeaders[i];
                    if (!(h.dwFlags & WHDR_DONE)) continue;
                    h.dwFlags &= ~WHDR_DONE;
                    Mix(s, m_OutputData[i].data());
                    waveOutWrite(m_WaveOut, &h, sizeof(h));
                }
                return;
            }
            if (!m_TimerClock) return;
            const double now = NowMs();
            if (now - m_TimerMixMs > 500.0) m_TimerMixMs = now - 10.0;   // after a stall
            std::int16_t block[kBlockFrames * 2];
            while (now - m_TimerMixMs >= 10.0)
            {
                Mix(s, block);
                m_TimerMixMs += 10.0;
            }
        }

        // One 10 ms stereo block of every talker.
        void Mix(const Settings& s, std::int16_t* out)
        {
            float mix[kBlockFrames * 2] = {};
            const double now = NowMs();
            const int local = g_LocalPlayer.load();
            const double toneHz = ToneHz(s);
            for (int p = 1; p <= kMaxPlayers; ++p)
            {
                Speaker& sp = g_Speakers[static_cast<std::size_t>(p)];
                float targetL, targetR;
                {
                    std::lock_guard<std::mutex> lock(g_Mutex);
                    if (!sp.used) continue;
                    while (sp.pcm.size() < static_cast<std::size_t>(kBlockFrames))
                    {
                        const JitterBuffer::Output o = sp.jitter.Next(now);
                        if (o.action == JitterBuffer::Action::None || o.action == JitterBuffer::Action::End) break;
                        if (!DecodeInto(sp, p, o, s)) break;
                    }
                    targetL = sp.targetLeft;
                    targetR = sp.targetRight;
                    if (sp.pcm.size() >= static_cast<std::size_t>(kBlockFrames)) sp.lastAudioMs = now;
                    g_Stats.lastJitterTargetMs = sp.jitter.TargetMs();
                }
                if (sp.pcm.size() < static_cast<std::size_t>(kBlockFrames))
                {
                    sp.pcm.clear();
                    continue;
                }
                const float gainL = targetL * s.volume, gainR = targetR * s.volume;
                for (int i = 0; i < kBlockFrames; ++i)
                {
                    const float t = float(i + 1) / float(kBlockFrames);
                    const float l = sp.appliedLeft + (gainL - sp.appliedLeft) * t;
                    const float r = sp.appliedRight + (gainR - sp.appliedRight) * t;
                    const float v = sp.pcm[static_cast<std::size_t>(i)] / 32768.0f;
                    mix[i * 2] += v * l;
                    mix[i * 2 + 1] += v * r;
                }
                sp.appliedLeft = gainL;
                sp.appliedRight = gainR;
                if (p == local && toneHz > 0.0)
                {
                    std::lock_guard<std::mutex> lock(g_Mutex);
                    if (g_LoopbackPcm.size() < static_cast<std::size_t>(kSampleRate * 2))
                        g_LoopbackPcm.insert(g_LoopbackPcm.end(), sp.pcm.begin(), sp.pcm.begin() + kBlockFrames);
                }
                sp.pcm.erase(sp.pcm.begin(), sp.pcm.begin() + kBlockFrames);
            }
            for (int i = 0; i < kBlockFrames * 2; ++i)
                out[i] = static_cast<std::int16_t>(std::lround(SoftLimit(mix[i]) * 32767.0f));
            if (!s.testDump.empty())
            {
                if (!m_MixDump) m_MixDump = std::fopen((s.testDump + "-mix.s16").c_str(), "wb");
                if (m_MixDump) std::fwrite(out, sizeof(std::int16_t), kBlockFrames * 2, m_MixDump);
                if (++m_DumpFlush % 100 == 0 && m_MixDump) std::fflush(m_MixDump);
            }
            std::lock_guard<std::mutex> lock(g_Mutex);
            ++g_Stats.mixedBlocks;
        }

        // Called with g_Mutex held.
        bool DecodeInto(Speaker& sp, int player, const JitterBuffer::Output& o, const Settings& s)
        {
            if (!sp.decoder)
            {
                int error = 0;
                sp.decoder = opus_decoder_create(kSampleRate, 1, &error);
                if (!sp.decoder) return false;
            }
            std::int16_t pcm[kFrameSamples];
            int n = 0;
            switch (o.action)
            {
            case JitterBuffer::Action::Decode:
                if (o.frame.flags & kFlagStart) { opus_decoder_ctl(sp.decoder, OPUS_RESET_STATE); sp.agc.Reset(); }
                n = opus_decode(sp.decoder, o.frame.data, o.frame.size, pcm, kFrameSamples, 0);
                ++g_Stats.decoded;
                break;
            case JitterBuffer::Action::DecodeFec:
                n = opus_decode(sp.decoder, o.frame.data, o.frame.size, pcm, kFrameSamples, 1);
                ++g_Stats.fec;
                break;
            case JitterBuffer::Action::Conceal:
                n = opus_decode(sp.decoder, nullptr, 0, pcm, kFrameSamples, 0);
                ++g_Stats.concealed;
                break;
            default:
                return false;
            }
            if (n != kFrameSamples) return false;
            sp.agc.Process(pcm, n, s.agc, s.agcMaxGain);
            sp.pcm.insert(sp.pcm.end(), pcm, pcm + n);
            if (!s.testDump.empty())
            {
                if (!sp.dump) sp.dump = std::fopen((s.testDump + "-speaker-" + std::to_string(player) + ".s16").c_str(), "wb");
                if (sp.dump) std::fwrite(pcm, sizeof(std::int16_t), static_cast<std::size_t>(n), sp.dump);
            }
            return true;
        }

        void PublishDeviceStats()
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_Stats.captureOpen = m_WaveIn != nullptr || m_ToneActive;
            g_Stats.outputOpen = m_WaveOut != nullptr;
            g_Stats.transmitting = m_Tx.Transmitting();
        }

        OpusEncoder* m_Encoder = nullptr;
        int m_EncBitrate = -1, m_EncLoss = -1;
        bool m_EncFec = false;
        Vad m_Vad;
        Transmitter m_Tx;
        std::deque<std::vector<std::int16_t>> m_Preroll;

        HANDLE m_CaptureEvent = nullptr, m_OutputEvent = nullptr;
        HWAVEIN m_WaveIn = nullptr;
        WAVEHDR m_CaptureHeaders[kCaptureBuffers]{};
        std::vector<std::int16_t> m_CaptureData[kCaptureBuffers];
        std::vector<std::int16_t> m_CapturePending;
        double m_CaptureRetryMs = 0.0;

        bool m_ToneActive = false;
        std::uint32_t m_ToneGeneration = 0u;
        ToneSource m_Tone;
        double m_ToneStartMs = 0.0;
        std::uint64_t m_ToneSamples = 0u;

        HWAVEOUT m_WaveOut = nullptr;
        WAVEHDR m_OutputHeaders[kOutputBuffers]{};
        std::vector<std::int16_t> m_OutputData[kOutputBuffers];
        double m_OutputRetryMs = 0.0;
        bool m_TimerClock = false;
        double m_TimerMixMs = 0.0;

        FILE* m_MixDump = nullptr;
        std::uint32_t m_DumpFlush = 0u;
    };

    AudioThread g_Audio;

    void EnsureThread()
    {
        std::call_once(g_StartOnce, []() {
            g_WakeEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            g_ThreadRun.store(true);
            g_Thread = std::thread([]() {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
                g_Audio.Run();
            });
        });
    }

    void Wake()
    {
        if (g_WakeEvent) SetEvent(g_WakeEvent);
    }

    // Virtual-key names for OpusVoicePushToTalkKey ("V", "F5", "CAPSLOCK", "MOUSE4", "0x56").
    int VirtualKeyFromName(const std::string& raw)
    {
        const std::string name = Lower(Trim(raw));
        if (name.empty() || name == "none" || name == "false" || name == "0") return 0;
        if (name.size() == 1)
        {
            const char c = name[0];
            if (c >= 'a' && c <= 'z') return 'A' + (c - 'a');
            if (c >= '0' && c <= '9') return c;
        }
        if (name.size() >= 2 && name[0] == 'f' && name.size() <= 3)
        {
            const int n = std::atoi(name.c_str() + 1);
            if (n >= 1 && n <= 24) return VK_F1 + n - 1;
        }
        if (name.rfind("0x", 0) == 0) return static_cast<int>(std::strtol(name.c_str() + 2, nullptr, 16)) & 0xFF;
        struct Named { const char* name; int vk; };
        static const Named table[] = {
            { "capslock", VK_CAPITAL }, { "tab", VK_TAB }, { "shift", VK_SHIFT }, { "lshift", VK_LSHIFT },
            { "rshift", VK_RSHIFT }, { "ctrl", VK_CONTROL }, { "lctrl", VK_LCONTROL }, { "rctrl", VK_RCONTROL },
            { "alt", VK_MENU }, { "lalt", VK_LMENU }, { "ralt", VK_RMENU }, { "space", VK_SPACE },
            { "mouse3", VK_MBUTTON }, { "mouse4", VK_XBUTTON1 }, { "mouse5", VK_XBUTTON2 },
            { "insert", VK_INSERT }, { "home", VK_HOME }, { "end", VK_END }, { "pgup", VK_PRIOR }, { "pgdn", VK_NEXT },
            { "backquote", VK_OEM_3 }, { "grave", VK_OEM_3 },
        };
        for (const Named& n : table) if (name == n.name) return n.vk;
        return 0;
    }

    bool ReadFileText(const char* path, std::string& out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }
}

// ---- stock voice filter --------------------------------------------------------------
// CClientState::ProcessVoiceData(SVC_VoiceData*) in engine.dll (the function
// that logs "ProcessVoiceData: Voice_AssignChannel failed for client %d!").
// SVC_VoiceData::m_nFromClient is at +0x54 in L4D2 (read as "mov edi,[esi+54h]"
// at +0x5C, checked before hooking).
namespace
{
    using tProcessVoiceData = bool(__thiscall*)(void* clientState, void* message);
    tProcessVoiceData g_OriginalProcessVoiceData = nullptr;
    constexpr int kVoiceFromClientOffset = 0x54;

    bool __fastcall ProcessVoiceDataDetour(void* clientState, void*, void* message)
    {
        int from = -1;
        __try { from = *reinterpret_cast<const int*>(static_cast<const char*>(message) + kVoiceFromClientOffset); }
        __except (EXCEPTION_EXECUTE_HANDLER) { from = -1; }
        if (from >= 0 && from < kMaxPlayers && OpusVoiceClient::ShouldDropStockVoice(from))
        {
            OpusVoiceClient::NoteStockVoiceDropped();
            return true;   // handled: this talker is heard through Opus
        }
        g_StockPassed.fetch_add(1u);
        return g_OriginalProcessVoiceData(clientState, message);
    }

    std::uintptr_t FindPattern(std::uintptr_t base, std::size_t size, const char* pattern)
    {
        std::vector<int> bytes;
        for (const char* p = pattern; *p;)
        {
            while (*p == ' ') ++p;
            if (!*p) break;
            if (*p == '?') { bytes.push_back(-1); while (*p && *p != ' ') ++p; continue; }
            bytes.push_back(static_cast<int>(std::strtol(p, nullptr, 16)));
            while (*p && *p != ' ') ++p;
        }
        const auto* mem = reinterpret_cast<const std::uint8_t*>(base);
        for (std::size_t i = 0; i + bytes.size() <= size; ++i)
        {
            std::size_t j = 0;
            while (j < bytes.size() && (bytes[j] < 0 || mem[i + j] == bytes[j])) ++j;
            if (j == bytes.size()) return base + i;
        }
        return 0;
    }
}

namespace OpusVoiceClient
{
    bool InstallStockVoiceFilter()
    {
        static bool attempted = false, installed = false;
        if (attempted) return installed;
        attempted = true;
        HMODULE engine = GetModuleHandleA("engine.dll");
        if (!engine) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(engine);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        std::uintptr_t target = 0;
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && !target; ++i, ++section)
            if (std::memcmp(section->Name, ".text", 5) == 0)
                target = FindPattern(base + section->VirtualAddress, section->Misc.VirtualSize,
                    "55 8B EC B8 98 10 00 00 E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 53 56 8B 75 08 8B 46 5C 57 50 68 00 10 00 00");
        const auto* code = reinterpret_cast<const std::uint8_t*>(target);
        if (!target || code[0x5C] != 0x8B || code[0x5D] != 0x7E || code[0x5E] != kVoiceFromClientOffset)
        {
            Game::logMsg("[VR][Voice] stock voice filter unavailable (ProcessVoiceData not found): Opus talkers may also be heard through the game's voice");
            return false;
        }
        if (MH_CreateHook(reinterpret_cast<LPVOID>(target), reinterpret_cast<LPVOID>(&ProcessVoiceDataDetour),
                reinterpret_cast<LPVOID*>(&g_OriginalProcessVoiceData)) != MH_OK ||
            MH_EnableHook(reinterpret_cast<LPVOID>(target)) != MH_OK)
        {
            Game::logMsg("[VR][Voice] stock voice filter: hook failed");
            return false;
        }
        installed = true;
        Game::logMsg("[VR][Voice] stock voice filter installed (engine.dll+0x%X)", static_cast<unsigned>(target - base));
        return true;
    }

    void ApplySettings(const Settings& settings)
    {
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_Settings = settings;
        }
        g_SettingsFromVR = true;
        Wake();
    }

    Settings CurrentSettings() { return SettingsSnapshot(); }

    void PollDesktopSettings()
    {
        if (g_SettingsFromVR) return;
        const double now = NowMs();
        if (now - g_DesktopSettingsCheckMs < 3000.0) return;
        g_DesktopSettingsCheckMs = now;
        std::string a, b;
        ReadFileText("VR\\config.txt", a);
        ReadFileText("VR\\config2.txt", b);
        const std::string stamp = a + "\n" + b;
        if (stamp == g_DesktopSettingsStamp) return;
        g_DesktopSettingsStamp = stamp;
        const Settings s = ParseSettingsText(stamp);
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_Settings = s;
        }
        Wake();
    }

    void OnServerFeatures(std::uint32_t mask)
    {
        // Servers repeat the capability bundle a few times: the same mask on a
        // running session only re-sends the hello (no device restart).
        g_LastFeatureAckMs = NowMs();
        g_FeaturesValid = true;
        if (mask == g_ServerFeatures && g_Negotiation.Active())
        {
            g_Negotiation.Refresh();
            return;
        }
        g_ServerFeatures = mask;
        g_Negotiation.Restart();
        g_Active.store(false);
        g_TransmitAllowed.store(false);
        g_Peers.store(0u);
    }

    void OnSessionReset()
    {
        g_ServerFeatures = 0u;
        g_FeaturesValid = false;
        g_Negotiation.Reset();
        g_Active.store(false);
        g_TransmitAllowed.store(false);
        g_Peers.store(0u);
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (Speaker& sp : g_Speakers) { sp.jitter.Reset(); sp.used = false; }
        g_Outgoing.clear();
        Wake();
    }

    void OnAck(std::uint32_t version)
    {
        static int s_AckLogs = 0;
        if (s_AckLogs < 12) { ++s_AckLogs; Game::logMsg("[VR][Voice] l4d2vr_voice_ack %u (hello flags %d)", version, g_Negotiation.SentFlags()); }
        const bool was = g_Negotiation.Active();
        g_Negotiation.OnAck(version);
        if (!was && g_Negotiation.Active())
            Game::logMsg("[VR][Voice] server relays Opus voice (protocol %u): voice chat is 48 kHz Opus with players who have the mod; others keep the game's own voice",
                version);
    }

    void OnPeers(std::uint32_t lo, std::uint32_t hi)
    {
        static int s_PeerLogs = 0;
        if (s_PeerLogs < 12) { ++s_PeerLogs; Game::logMsg("[VR][Voice] l4d2vr_voice_peers %x %x", lo, hi); }
        g_Negotiation.OnPeers(lo, hi);
        g_Peers.store(g_Negotiation.Peers());
    }

    void OnFrame(int player, const char* text)
    {
        if (!g_Negotiation.Active() || player < 1 || player > kMaxPlayers) return;
        const Settings s = SettingsSnapshot();
        if (!s.enabled) return;
        if (player == g_LocalPlayer.load() && !LoopbackWanted(s)) return;
        l4d2vr_voice::Frame f;
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (!DecodeFrameText(text, f)) { ++g_Stats.framesRejected; return; }
        Speaker& sp = g_Speakers[static_cast<std::size_t>(player)];
        const double now = NowMs();
        if (now - sp.lastArrivalMs < 1000.0) g_Stats.maxArrivalGapMs = std::max(g_Stats.maxArrivalGapMs, now - sp.lastArrivalMs);
        sp.jitter.Insert(f, now);
        sp.lastArrivalMs = now;
        sp.used = true;
        ++g_Stats.framesReceived;
    }

    void SetStockVoiceRecord(bool held) { g_PttStock.store(held); }
    void SetControllerPushToTalk(bool held) { g_PttController.store(held); }
    void SetSelfTestPushToTalk(int state) { g_PttSelfTest.store(state); }
    void SetSelfTestTone(double hz, double onMs, double offMs)
    {
        g_ToneOnMs.store(onMs);
        g_ToneOffMs.store(offMs);
        g_ToneHz.store(hz);
        g_ToneGeneration.fetch_add(1u);
        Wake();
    }
    void SetSelfTestLoopback(int state) { g_LoopbackSelfTest.store(state); }
    void SetSelfTestMode(int state) { g_ModeSelfTest.store(state); }
    void ResetLoopbackAnalysis()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_LoopbackPcm.clear();
    }

    bool ShouldDropStockVoice(int senderClientSlot)
    {
        return (g_Peers.load() & PlayerBit(senderClientSlot + 1)) != 0u;
    }

    void NoteStockVoiceDropped() { g_StockDropped.fetch_add(1u); }

    bool IsTalking(int player)
    {
        if (player == g_LocalPlayer.load() && !LoopbackWanted(SettingsSnapshot())) return g_Transmitting.load();
        if (player < 1 || player > kMaxPlayers) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        return NowMs() - g_Speakers[static_cast<std::size_t>(player)].lastAudioMs < kTalkingHoldMs;
    }

    Stats GetStats()
    {
        std::vector<std::int16_t> loop;
        Stats out;
        double hz;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            out = g_Stats;
            loop = g_LoopbackPcm;
            hz = ToneHz(g_Settings);
        }
        out.active = g_Negotiation.Active();
        out.transmitAllowed = g_Negotiation.TransmitAllowed();
        out.peers = g_Peers.load();
        out.stockVoiceDropped = g_StockDropped.load();
        out.stockVoicePassed = g_StockPassed.load();
        out.loopbackSamples = static_cast<std::uint32_t>(loop.size());
        // Skip the first 200 ms (codec and buffer warm-up).
        const std::size_t skip = std::min<std::size_t>(loop.size(), kSampleRate / 5);
        if (hz > 0.0 && loop.size() > skip + kSampleRate / 4)
            out.loopbackToneDb = ToneToNoiseDb(loop.data() + skip, static_cast<int>(loop.size() - skip), hz);
        return out;
    }

    void Frame(const FrameInputs& in)
    {
        const Settings s = SettingsSnapshot();
        // CreateMove (and so Frame) stops while not in a game: a long gap with
        // no feature ack since means a new server or map; wait for its ack.
        const double frameNow = NowMs();
        if (g_LastFrameMs > 0.0 && frameNow - g_LastFrameMs > 3000.0 && g_LastFeatureAckMs < g_LastFrameMs)
            OnSessionReset();
        g_LastFrameMs = frameNow;
        if (!in.inGame)
        {
            if (g_Negotiation.Active() || g_ServerFeatures) OnSessionReset();
            return;
        }
        if (!s.enabled && !g_Negotiation.Active() && g_Negotiation.SentFlags() <= 0) return;
        EnsureThread();
        g_LocalPlayer.store(in.localPlayer);

        // Desktop push-to-talk key (in addition to the game's own voice key).
        const int vk = VirtualKeyFromName(s.pushToTalkKey);
        g_PttKey.store(vk != 0 && in.gameWindowFocused && (GetAsyncKeyState(vk) & 0x8000) != 0);

        ClientNegotiation::Inputs n;
        n.enabled = s.enabled;
        n.serverFeature = g_FeaturesValid && (in.serverFeatures & 0x2000u) != 0u;   // l4d2vr_features::OpusVoice
        n.transmit = ToneHz(s) > 0.0 || !g_CaptureFailed.load();
        n.loopback = LoopbackWanted(s);
        n.nowMs = NowMs();
        const int hello = g_Negotiation.Update(n);
        if (hello >= 0 && in.serverCommand)
        {
            char line[64];
            std::snprintf(line, sizeof(line), "%s %u %d", kHelloCommand, kProtocolVersion, hello);
            in.serverCommand(line, true);
            static int s_HelloLogs = 0;
            if (s_HelloLogs < 12) { ++s_HelloLogs; Game::logMsg("[VR][Voice] sent %s (server features 0x%X)", line, in.serverFeatures); }
            if (hello == 0) Game::logMsg("[VR][Voice] Opus voice off (OpusVoiceEnabled=false): the game's own voice is used");
        }
        const bool active = g_Negotiation.Active();
        const bool transmit = g_Negotiation.TransmitAllowed();
        if (active != g_Active.load() || transmit != g_TransmitAllowed.load())
        {
            g_Active.store(active);
            g_TransmitAllowed.store(transmit);
            if (!active) g_Peers.store(0u);
            Wake();
        }

        // Send what the audio thread encoded (unreliable string commands).
        std::deque<std::string> outgoing;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            outgoing.swap(g_Outgoing);
        }
        if (active && in.serverCommand)
        {
            char line[24 + kMaxFrameTextChars];
            for (const std::string& text : outgoing)
            {
                std::snprintf(line, sizeof(line), "%s %s", kUpCommand, text.c_str());
                in.serverCommand(line, false);
            }
        }

        // Talker positions -> gains.
        const double now = NowMs();
        for (int p = 1; p <= kMaxPlayers; ++p)
        {
            Vec3 head{};
            bool haveHead = false;
            {
                std::lock_guard<std::mutex> lock(g_Mutex);
                const Speaker& sp = g_Speakers[static_cast<std::size_t>(p)];
                if (!sp.used || now - sp.lastArrivalMs > 3000.0) continue;
            }
            if (p != in.localPlayer && in.speakerHead) haveHead = in.speakerHead(p, head);
            const Gains g = haveHead ? SpatialGains(in.listener, in.listenerYawDeg, head, s) : Gains{ 1.0f, 1.0f };
            std::lock_guard<std::mutex> lock(g_Mutex);
            Speaker& sp = g_Speakers[static_cast<std::size_t>(p)];
            sp.targetLeft = g.left;
            sp.targetRight = g.right;
        }

        if (now - g_LastStatsLogMs > 5000.0)
        {
            g_LastStatsLogMs = now;
            const Stats st = GetStats();
            if (st.framesSent || st.framesReceived)
                Game::logMsg("[VR][Voice] stats: active=%d tx=%d capture=%d output=%d sent=%u (%u B, %u spurts) received=%u decoded=%u fec=%u plc=%u mixed=%u stockDropped=%u peers=%llx jitter=%dms maxGap=%.0fms",
                    st.active ? 1 : 0, st.transmitAllowed ? 1 : 0, st.captureOpen ? 1 : 0, st.outputOpen ? 1 : 0,
                    st.framesSent, st.bytesSent, st.talkspurtsSent, st.framesReceived, st.decoded, st.fec, st.concealed,
                    st.mixedBlocks, st.stockVoiceDropped, static_cast<unsigned long long>(st.peers), st.lastJitterTargetMs, st.maxArrivalGapMs);
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_Stats.maxArrivalGapMs = 0.0;
        }
    }

    void Shutdown()
    {
        if (g_ThreadRun.exchange(false))
        {
            Wake();
            if (g_Thread.joinable()) g_Thread.join();
        }
    }
}

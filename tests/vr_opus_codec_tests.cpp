// Opus encode/decode round trip with the vendored libopus 1.4 and the mod's
// voice settings: 48 kHz mono, VOIP, 32 kbps VBR, in-band FEC, 20 ms frames
// capped at kMaxOpusBytes. Then the whole receive path (frames as text ->
// jitter buffer -> decode / FEC / PLC) under packet loss.
#include "../L4D2VR/vr_opus_voice.h"

#include <opus.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_Checks = 0;
static void CheckImpl(bool result, int line, const char* what)
{
    ++g_Checks;
    if (!result) { std::fprintf(stderr, "Check failed at line %d: %s\n", line, what); std::abort(); }
}
#define Check(result) CheckImpl((result), __LINE__, #result)

using namespace l4d2vr_voice;

namespace
{
    // A voice-like test signal: a 180 Hz glottal-ish harmonic stack with
    // formant-like weights and a 4 Hz syllable envelope, high-passed noise
    // "fricatives" (speech has energy up to 10+ kHz; without it Opus rightly
    // keeps to wide band), plus a 440 Hz marker tone the analysis finds after
    // decoding.
    void MakeSignal(std::vector<std::int16_t>& out, int samples)
    {
        out.resize(static_cast<std::size_t>(samples));
        const double pi = 3.14159265358979;
        std::uint32_t rng = 1u;
        double hp = 0.0, prev = 0.0;
        for (int i = 0; i < samples; ++i)
        {
            const double t = double(i) / kSampleRate;
            const double env = 0.55 + 0.45 * std::sin(2.0 * pi * 4.0 * t);
            double v = 0.0;
            for (int h = 1; h <= 20; ++h)
            {
                const double f = 180.0 * h;
                const double w = (f > 500.0 && f < 900.0) || (f > 1300.0 && f < 1900.0) ? 1.0 : 0.25;
                v += w / h * std::sin(2.0 * pi * f * t + h * 0.7);
            }
            rng = rng * 1664525u + 1013904223u;
            const double noise = (static_cast<int>(rng >> 16) % 2001 - 1000) / 1000.0;
            hp = 0.6 * (hp + noise - prev);
            prev = noise;
            const double fricative = std::fmod(t, 0.5) < 0.12 ? 0.5 * hp : 0.02 * hp;
            v = 0.35 * env * v + 0.25 * std::sin(2.0 * pi * 440.0 * t) + fricative;
            out[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0, 1.0) * 20000.0));
        }
    }

    OpusEncoder* MakeEncoder(int bitrate, bool fec, int lossPercent)
    {
        int error = 0;
        OpusEncoder* enc = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
        Check(enc && error == OPUS_OK);
        Check(opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate)) == OPUS_OK);
        Check(opus_encoder_ctl(enc, OPUS_SET_VBR(1)) == OPUS_OK);
        Check(opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10)) == OPUS_OK);
        Check(opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) == OPUS_OK);
        Check(opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(fec ? 1 : 0)) == OPUS_OK);
        Check(opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(lossPercent)) == OPUS_OK);
        Check(opus_encoder_ctl(enc, OPUS_SET_DTX(0)) == OPUS_OK);
        return enc;
    }

    // Best normalized cross-correlation over the codec delay.
    double Correlation(const std::vector<std::int16_t>& a, const std::vector<std::int16_t>& b, int from, int length, int& bestLag)
    {
        double best = -1.0;
        bestLag = 0;
        for (int lag = 0; lag <= 1200; lag += 1)
        {
            double ab = 0, aa = 0, bb = 0;
            for (int i = from; i < from + length; ++i)
            {
                const double x = a[static_cast<std::size_t>(i)], y = b[static_cast<std::size_t>(i + lag)];
                ab += x * y; aa += x * x; bb += y * y;
            }
            const double c = ab / std::sqrt(std::max(aa * bb, 1.0));
            if (c > best) { best = c; bestLag = lag; }
        }
        return best;
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("%s\n", opus_get_version_string());
    const int frames = 150;   // 3 s
    std::vector<std::int16_t> input;
    MakeSignal(input, (frames + 4) * kFrameSamples);

    // ---- encode: sizes, bandwidth, bitrate ----------------------------------------
    OpusEncoder* enc = MakeEncoder(32000, true, 5);
    std::vector<std::vector<unsigned char>> packets;
    int bytesTotal = 0, maxBytes = 0, fullbandOrSwb = 0;
    for (int f = 0; f < frames; ++f)
    {
        unsigned char data[kMaxOpusBytes];
        const int n = opus_encode(enc, input.data() + f * kFrameSamples, kFrameSamples, data, kMaxOpusBytes);
        Check(n > 0 && n <= kMaxOpusBytes);
        packets.emplace_back(data, data + n);
        bytesTotal += n;
        maxBytes = std::max(maxBytes, n);
        const int bw = opus_packet_get_bandwidth(data);
        fullbandOrSwb += bw == OPUS_BANDWIDTH_FULLBAND || bw == OPUS_BANDWIDTH_SUPERWIDEBAND;
        Check(opus_packet_get_samples_per_frame(data, kSampleRate) == kFrameSamples);
    }
    const double kbps = bytesTotal * 8.0 / (frames * 0.02) / 1000.0;
    std::printf("32 kbps VBR+FEC (5%% loss): average %.1f kbps, largest frame %d bytes, %d/%d frames SWB/FB\n", kbps, maxBytes, fullbandOrSwb, frames);
    Check(kbps > 20.0 && kbps < 40.0);
    // Wideband or better on every frame (stock L4D2 voice: 11 kHz narrow band).
    for (const auto& p : packets) Check(opus_packet_get_bandwidth(p.data()) >= OPUS_BANDWIDTH_WIDEBAND);
    Check(fullbandOrSwb > frames / 2);

    // ---- decode, lossless --------------------------------------------------------------
    int error = 0;
    OpusDecoder* dec = opus_decoder_create(kSampleRate, 1, &error);
    Check(dec && error == OPUS_OK);
    std::vector<std::int16_t> clean;
    for (const auto& p : packets)
    {
        std::int16_t pcm[kFrameSamples];
        Check(opus_decode(dec, p.data(), static_cast<opus_int32>(p.size()), pcm, kFrameSamples, 0) == kFrameSamples);
        clean.insert(clean.end(), pcm, pcm + kFrameSamples);
    }
    int lag = 0;
    const double corr = Correlation(input, clean, kFrameSamples * 10, kFrameSamples * 100, lag);
    const double toneDb = ToneToNoiseDb(clean.data() + kFrameSamples * 10, kFrameSamples * 100, 440.0);
    std::printf("lossless: correlation %.3f at a %d-sample delay, 440 Hz marker %.1f dB over off-tone bins\n", corr, lag, toneDb);
    Check(corr > 0.75);   // waveform match (noise bands are coded perceptually, not sample by sample)
    Check(toneDb > 20.0);

    // ---- the receive path: text frames -> jitter buffer -> decode with 10% loss ----
    auto receive = [&](bool useFec, int& fecCount, int& plcCount) {
        int err = 0;
        OpusDecoder* d = opus_decoder_create(kSampleRate, 1, &err);
        Check(d && err == OPUS_OK);
        JitterBuffer jb;
        Transmitter tx;
        std::vector<std::int16_t> out;
        double now = 0.0;
        int sent = 0;
        for (int tick = 0; tick < frames + 20; ++tick, now += 20.0)
        {
            if (sent < frames)
            {
                Transmitter::Step step = tx.Update(true, true, true, 0);
                Check(step.encode);
                Frame f;
                tx.Stamp(f, false);
                f.size = static_cast<std::uint8_t>(packets[static_cast<std::size_t>(sent)].size());
                std::memcpy(f.data, packets[static_cast<std::size_t>(sent)].data(), f.size);
                char text[kMaxFrameTextChars + 1];
                Check(EncodeFrameText(f, text, sizeof(text)));
                Frame g;
                Check(DecodeFrameText(text, g));
                // Every 10th frame (and one double loss) is lost upstream.
                const bool lost = (sent % 10 == 7) || sent == 51 || sent == 52;
                if (!lost) jb.Insert(g, now + 30.0 + (sent % 3) * 7.0);
                ++sent;
            }
            else if (sent == frames)
            {
                Check(tx.Update(false, false, true, 0).sendEnd);
                Frame end;
                tx.Stamp(end, true);
                jb.Insert(end, now + 30.0);
                ++sent;
            }
            const JitterBuffer::Output o = jb.Next(now + 30.0);
            std::int16_t pcm[kFrameSamples];
            int n = 0;
            switch (o.action)
            {
            case JitterBuffer::Action::Decode:
                if (o.frame.flags & kFlagStart) opus_decoder_ctl(d, OPUS_RESET_STATE);
                n = opus_decode(d, o.frame.data, o.frame.size, pcm, kFrameSamples, 0);
                break;
            case JitterBuffer::Action::DecodeFec:
                if (useFec) { n = opus_decode(d, o.frame.data, o.frame.size, pcm, kFrameSamples, 1); ++fecCount; }
                else { n = opus_decode(d, nullptr, 0, pcm, kFrameSamples, 0); ++plcCount; }
                break;
            case JitterBuffer::Action::Conceal:
                n = opus_decode(d, nullptr, 0, pcm, kFrameSamples, 0);
                ++plcCount;
                break;
            default:
                break;
            }
            if (n > 0) { Check(n == kFrameSamples); out.insert(out.end(), pcm, pcm + n); }
        }
        opus_decoder_destroy(d);
        return out;
    };
    int fec = 0, plc = 0, plcOnlyFec = 0, plcOnly = 0;
    const std::vector<std::int16_t> withFec = receive(true, fec, plc);
    const std::vector<std::int16_t> withoutFec = receive(false, plcOnlyFec, plcOnly);
    Check(withFec.size() >= static_cast<std::size_t>(frames * kFrameSamples));
    int lagFec = 0, lagPlc = 0;
    const double cFec = Correlation(input, withFec, kFrameSamples * 10, kFrameSamples * 120, lagFec);
    const double cPlc = Correlation(input, withoutFec, kFrameSamples * 10, kFrameSamples * 120, lagPlc);
    std::printf("10%% loss: FEC recovered %d frames, PLC %d; correlation %.3f with FEC, %.3f with PLC only\n", fec, plc, cFec, cPlc);
    Check(fec >= 14 && plc <= 2);                 // single losses come back through FEC
    Check(cFec > 0.7 && cFec >= cPlc);
    Check(ToneToNoiseDb(withFec.data() + kFrameSamples * 10, kFrameSamples * 120, 440.0) > 15.0);

    // ---- bitrate settings cover the advertised range ------------------------------------
    for (int bitrate : { 12000, 24000, 32000, 48000, 64000 })
    {
        OpusEncoder* e = MakeEncoder(bitrate, true, 10);
        int largest = 0;
        for (int f = 0; f < 50; ++f)
        {
            unsigned char data[kMaxOpusBytes];
            const int n = opus_encode(e, input.data() + f * kFrameSamples, kFrameSamples, data, kMaxOpusBytes);
            Check(n > 0 && n <= kMaxOpusBytes);
            largest = std::max(largest, n);
        }
        std::printf("  %2d kbps: largest frame %d bytes\n", bitrate / 1000, largest);
        opus_encoder_destroy(e);
    }

    opus_decoder_destroy(dec);
    opus_encoder_destroy(enc);
    std::printf("vr_opus_codec_tests: %d checks passed\n", g_Checks);
    return 0;
}

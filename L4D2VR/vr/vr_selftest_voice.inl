// Self-test steps for Opus voice chat (vr_opus_voice.h, vr_opus_voice_client.cpp).
//
//   opusvoice          On a server with OpusVoice (dedicated plugin or this
//       mod's listen host): a 440 Hz test tone replaces the microphone and the
//       server loops this client's own voice back. Checks the handshake, that
//       push-to-talk sends ~50 frames/s, that they come back through the server,
//       decode with little concealment, that the decoded audio still carries the
//       440 Hz tone (Goertzel, >= 20 dB over off-tone bins), that releasing the
//       button ends the talkspurt and stops sending, and that voice activation
//       opens and closes on a pulsed tone. cfg=OpusVoiceTestDump=<prefix> also
//       writes the decoded audio (<prefix>-speaker-<n>.s16, <prefix>-mix.s16).
//   opusvoicefallback  On a server without OpusVoice: holding push-to-talk
//       sends no Opus frames and the game's own voice path is left alone.
//   opusvoicestock     After opusvoice (this client is an Opus talker): the
//       game's own voice, looped back by the server (voice_loopback 1, input
//       from voice_input.wav), must be dropped by the stock voice filter.

bool VR::RunVoiceSelfTestStep(const std::string& step, float t, C_BasePlayer* player, bool& handled)
{
    (void)player;
    handled = step == "opusvoice" || step == "opusvoicefallback" || step == "opusvoicestock";
    if (!handled)
        return false;
    bool stageFiredThisFrame = false;
    auto at = [&](int stage, float when) -> bool
    {
        if (stageFiredThisFrame || m_SelfTestStage != stage || t < when)
            return false;
        m_SelfTestStage = stage + 1;
        stageFiredThisFrame = true;
        return true;
    };
    static OpusVoiceClient::Stats s_Base{};
    static OpusVoiceClient::Stats s_Mark{};
    char detail[320];
    const std::uint32_t features = m_ServerFeatureMask.load(std::memory_order_acquire);
    const int local = m_Game && m_Game->m_EngineClient ? m_Game->m_EngineClient->GetLocalPlayer() : 0;
    auto restore = []() {
        OpusVoiceClient::SetSelfTestPushToTalk(-1);
        OpusVoiceClient::SetSelfTestLoopback(-1);
        OpusVoiceClient::SetSelfTestMode(-1);
        OpusVoiceClient::SetSelfTestTone(0.0, 0.0, 0.0);
    };

    if (step == "opusvoicestock")
    {
        auto cmd = [&](const char* text) { if (m_Game) m_Game->ClientCmd_Unrestricted(text); };
        if (at(0, 0.0f))
        {
            OpusVoiceClient::SetSelfTestPushToTalk(0);   // Opus silent: only the game's voice runs
            s_Base = OpusVoiceClient::GetStats();
            cmd("voice_enable 1; voice_loopback 1; voice_inputfromfile 1; +voicerecord");
        }
        if (at(1, 4.0f))
            cmd("-voicerecord");
        if (at(2, 5.5f))
        {
            const OpusVoiceClient::Stats s = OpusVoiceClient::GetStats();
            const std::uint32_t dropped = s.stockVoiceDropped - s_Base.stockVoiceDropped;
            const std::uint32_t passed = s.stockVoicePassed - s_Base.stockVoicePassed;
            std::snprintf(detail, sizeof(detail), "game voice packets from player %d: dropped %u, passed %u; peers=%llx opus sent %u",
                local, dropped, passed, static_cast<unsigned long long>(s.peers), s.framesSent - s_Base.framesSent);
            SelfTestReport("game voice of an Opus talker is dropped", dropped > 0 && passed == 0, detail);
            cmd("voice_loopback 0; voice_inputfromfile 0");
            restore();
            return true;
        }
        return false;
    }

    if (step == "opusvoicefallback")
    {
        if (at(0, 0.0f))
        {
            OpusVoiceClient::SetSelfTestTone(440.0, 0.0, 0.0);
            OpusVoiceClient::SetSelfTestLoopback(1);
            OpusVoiceClient::SetSelfTestMode(0);
            OpusVoiceClient::SetSelfTestPushToTalk(1);
            s_Base = OpusVoiceClient::GetStats();
        }
        if (at(1, 5.0f))
        {
            const OpusVoiceClient::Stats s = OpusVoiceClient::GetStats();
            std::snprintf(detail, sizeof(detail), "server features 0x%X (OpusVoice %d) active=%d sent=%u received=%u",
                features, (features & l4d2vr_features::OpusVoice) ? 1 : 0, s.active ? 1 : 0,
                s.framesSent - s_Base.framesSent, s.framesReceived - s_Base.framesReceived);
            SelfTestReport("opus voice stays off without the server feature",
                !(features & l4d2vr_features::OpusVoice) && !s.active && s.framesSent == s_Base.framesSent, detail);
            restore();
            return true;
        }
        return false;
    }

    // ---- opusvoice ----
    if (at(0, 0.0f))
    {
        OpusVoiceClient::SetSelfTestTone(440.0, 0.0, 0.0);
        OpusVoiceClient::SetSelfTestLoopback(1);
        OpusVoiceClient::SetSelfTestMode(0);
        OpusVoiceClient::SetSelfTestPushToTalk(0);
    }
    if (at(1, 4.0f))
    {
        const OpusVoiceClient::Stats s = OpusVoiceClient::GetStats();
        std::snprintf(detail, sizeof(detail), "features 0x%X active=%d transmit=%d peers=%llx local=%d capture=%d output=%d",
            features, s.active ? 1 : 0, s.transmitAllowed ? 1 : 0, static_cast<unsigned long long>(s.peers), local,
            s.captureOpen ? 1 : 0, s.outputOpen ? 1 : 0);
        SelfTestReport("opus voice negotiated", (features & l4d2vr_features::OpusVoice) && s.active && s.transmitAllowed &&
            (s.peers & l4d2vr_voice::PlayerBit(local)) != 0u, detail);
        OpusVoiceClient::ResetLoopbackAnalysis();
        s_Base = OpusVoiceClient::GetStats();
        OpusVoiceClient::SetSelfTestPushToTalk(1);
    }
    if (at(2, 8.0f))
    {
        OpusVoiceClient::SetSelfTestPushToTalk(0);
        const OpusVoiceClient::Stats s = OpusVoiceClient::GetStats();
        const std::uint32_t sent = s.framesSent - s_Base.framesSent;
        const std::uint32_t received = s.framesReceived - s_Base.framesReceived;
        const std::uint32_t decoded = s.decoded - s_Base.decoded;
        const std::uint32_t repaired = (s.fec - s_Base.fec) + (s.concealed - s_Base.concealed);
        const std::uint32_t mixed = s.mixedBlocks - s_Base.mixedBlocks;
        std::snprintf(detail, sizeof(detail), "4 s held: sent %u frames (%u B, %.1f kbit/s), %u spurt(s)",
            sent, s.bytesSent - s_Base.bytesSent, (s.bytesSent - s_Base.bytesSent) * 8.0 / 4.0 / 1000.0,
            s.talkspurtsSent - s_Base.talkspurtsSent);
        SelfTestReport("opus voice push-to-talk sends 50 frames/s", sent >= 185 && sent <= 215 &&
            s.talkspurtsSent - s_Base.talkspurtsSent == 1, detail);
        std::snprintf(detail, sizeof(detail), "received %u of %u, decoded %u, FEC+PLC %u, mixed %u blocks, jitter target %d ms",
            received, sent, decoded, repaired, mixed, s.lastJitterTargetMs);
        SelfTestReport("opus voice comes back through the server", received + 10 >= sent && decoded + 20 >= received &&
            repaired * 10 <= decoded + 10 && mixed >= 300, detail);
        std::snprintf(detail, sizeof(detail), "440 Hz %.1f dB over off-tone bins in %.2f s of decoded audio",
            s.loopbackToneDb, s.loopbackSamples / 48000.0);
        SelfTestReport("opus voice tone survives the round trip", s.loopbackToneDb >= 20.0 && s.loopbackSamples >= 48000u, detail);
        s_Mark = s;
    }
    if (at(3, 9.0f))
        s_Mark = OpusVoiceClient::GetStats();   // the 160 ms tail and the End frame are out
    if (at(4, 10.5f))
    {
        const OpusVoiceClient::Stats s = OpusVoiceClient::GetStats();
        std::snprintf(detail, sizeof(detail), "transmitting=%d talking=%d sent after release %u", s.transmitting ? 1 : 0,
            OpusVoiceClient::IsTalking(local) ? 1 : 0, s.framesSent - s_Mark.framesSent);
        SelfTestReport("opus voice stops on release", !s.transmitting && !OpusVoiceClient::IsTalking(local) &&
            s.framesSent == s_Mark.framesSent, detail);
        // Voice activation on a pulsed tone (400 ms on / 600 ms off).
        OpusVoiceClient::SetSelfTestMode(1);
        OpusVoiceClient::SetSelfTestTone(440.0, 400.0, 600.0);
        s_Mark = s;
    }
    if (at(5, 16.5f))
    {
        const OpusVoiceClient::Stats s = OpusVoiceClient::GetStats();
        const std::uint32_t spurts = s.talkspurtsSent - s_Mark.talkspurtsSent;
        const std::uint32_t sent = s.framesSent - s_Mark.framesSent;
        std::snprintf(detail, sizeof(detail), "6 s of 1 s pulses: %u talkspurts, %u frames", spurts, sent);
        SelfTestReport("opus voice activation follows speech", spurts >= 4 && spurts <= 7 && sent >= 120 && sent <= 260, detail);
        const OpusVoiceClient::Stats total = OpusVoiceClient::GetStats();
        Game::logMsg("[VR][SelfTest] INFO opus voice totals: sent=%u received=%u decoded=%u fec=%u plc=%u stockDropped=%u output=%d",
            total.framesSent, total.framesReceived, total.decoded, total.fec, total.concealed, total.stockVoiceDropped,
            total.outputOpen ? 1 : 0);
        restore();
        return true;
    }
    return false;
}

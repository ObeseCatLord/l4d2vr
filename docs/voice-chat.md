# Opus voice chat (2.0.5)

L4D2's own voice chat uses a fixed codec (Miles/Speex at 11 kHz; there is no `sv_voicecodec`
to change it), which is why it sounds grainy and is hard to understand. With 2.0.5, players who
have the mod talk to each other with **48 kHz Opus** instead, heard from where each talker stands.
Players without the mod are unaffected and still hear you (and you them) through the game's voice.

## Where it works

| Server | Mod players hear each other | Players without the mod |
|---|---|---|
| Dedicated server with the L4D2VR plugin 2.0.5+ (feature `OpusVoice`, 0x2000) | Opus | game voice, both ways |
| Listen server hosted by a mod player (2.0.5+) | Opus | game voice, both ways |
| Any other server (no plugin, older plugin) | game voice | game voice |

VR and desktop players with the mod both use it. Nothing changes for anyone on plain servers.

## Talking

* **Push to talk** (default, `OpusVoiceMode=ptt`), any of:
  * the game's own voice key (`+voicerecord`, whatever you bound it to);
  * the VR voice combo (`VoiceRecordCombo`, default Crouch+Reload);
  * the **VoicePushToTalk** controller button: the **left menu button** on Quest/Touch, Quest Pro,
    Quest 3, Pico 4, Vive Cosmos, Vive Focus 3 and HP Reverb G2; the **right trackpad press** on
    Valve Index (WMR and Vive wands have no free button: use the combo);
  * an extra desktop key (`OpusVoicePushToTalkKey`, for example `V`, `CAPSLOCK`, `MOUSE4`).

  Every push-to-talk also keys the game's own voice, so players without the mod hear you. Opus
  keeps sending 160 ms after you let go, so the last syllable is not cut off.
* **Voice activation** (`OpusVoiceMode=vad`): you are sent whenever you speak
  (`OpusVoiceVadSensitivity`), with 60 ms of pre-roll and a 300 ms hangover. Only mod players hear
  voice-activated speech; the game's voice still needs a push-to-talk.
* **Microphone test**: `OpusVoiceLoopback=true` sends your voice back to you through the server,
  as others hear it.

The microphone is the Windows default recording device (Proton: the PulseAudio/PipeWire default
source). Playback uses the default output device, which is also the one the game plays on.

## Settings (`VR/config.txt`, also in the config panel under *Voice Chat*)

| Key | Default | Meaning |
|---|---|---|
| `OpusVoiceEnabled` | `true` | Opus voice with mod players (false: the game's voice only) |
| `OpusVoiceMode` | `ptt` | `ptt` push to talk, `vad` voice activation |
| `OpusVoiceVadSensitivity` | `0.55` | 0..1, higher opens on quieter speech |
| `OpusVoiceBitrate` | `32000` | bit/s you send, 12000-64000 |
| `OpusVoiceFec` | `true` | in-band forward error correction |
| `OpusVoiceExpectedLossPercent` | `5` | loss the FEC is tuned for, 0-40 |
| `OpusVoiceMicGain` | `1.0` | microphone gain, 0-4 |
| `OpusVoiceVolume` | `1.0` | voice chat volume, 0-2 (separate from the game's volume) |
| `OpusVoiceAgc` | `true` | even out every talker to the same loudness |
| `OpusVoiceAgcMaxGain` | `6` | most a quiet talker is raised, 1-16x |
| `OpusVoicePositional` | `true` | pan and distance attenuation |
| `OpusVoiceRadioVolume` | `0.55` | level of distant talkers (and the floor for near ones) |
| `OpusVoiceSpatialDistanceMeters` | `18` | distance from which a talker is a centred "radio" voice |
| `OpusVoiceLoopback` | `false` | hear yourself through the server |
| `OpusVoicePushToTalkKey` | empty | extra desktop push-to-talk key |

Testing only (not in the shipped config): `OpusVoiceTestToneHz` replaces the microphone with a
tone; `OpusVoiceTestDump=<prefix>` writes the decoded voice of each talker
(`<prefix>-speaker-<n>.s16`, 48 kHz mono s16le) and the mix (`<prefix>-mix.s16`, stereo).

## Quality

* 48 kHz mono, Opus `VOIP` application, 32 kbit/s VBR, complexity 10, 20 ms frames. Real speech
  codes as full band (20 kHz audio bandwidth); a voice without high frequencies drops to wide
  band by itself. Stock L4D2 voice: 11 kHz sampling, ~5 kHz bandwidth.
* In-band FEC (tuned for 5% loss): a frame lost on the way up is rebuilt from the next frame.
* Jitter buffer per talker: starts 60 ms behind and grows up to 300 ms when frames arrive unevenly
  (shrinking again between talkspurts). A frame that is late (nothing after it has arrived yet) is
  waited for, with Opus packet-loss concealment filling the gap, so it still plays; a frame that is
  missing while later ones are there is rebuilt with FEC from the next frame, else concealed; a
  backlog after a stall is skipped down to the target.
* Automatic gain control per talker, a soft limiter on the mix, playback ~50 ms buffered.
* Mouth to ear: about 20 ms capture + upload + server tick + relay + 60-300 ms jitter buffer +
  50 ms playback. On a listen host the buffer stays at 60 ms; through a remote dedicated server the
  reliable server-to-client channel delivers frames in bursts (the game sends packets at
  `cl_updaterate`, 20-30 per second), so the buffer grows toward its 300 ms maximum.

## Positional playback

Each talker is heard at their head (their player position + 64 units). Within 1.5 m at full
level; then a gentle roll-off (half at about 7.5 m); left/right pan with constant power, at most
70% to one side, from your head's yaw (VR) or view yaw (desktop). From
`OpusVoiceSpatialDistanceMeters` on, and never quieter than that, a talker is a centred radio voice
at `OpusVoiceRadioVolume`, so a teammate across the map stays easy to understand.

## How it travels

The mod's existing string-command channel (the same one the pose relay uses):

```
client -> server   l4d2vr_voice_hello 1 <flags>      opt in: 1 receive, 2 transmit, 4 loopback
server -> client   l4d2vr_voice_ack 1
client -> server   l4d2vr_voice_up <frame>           one 20 ms frame, unreliable (ServerCmd(.., false))
server -> client   l4d2vr_voice_rx <player> <frame>  reliable
server -> client   l4d2vr_voice_peers <lo> <hi>      players who send Opus
```

`<frame>` is base64url of a 6-byte header (version, Start/End flags, 16-bit sequence, talkspurt
id, length) and up to 160 Opus bytes; a whole `voice_up` stays under the 250 characters that
`IVEngineClient::ServerCmd` allows. The server never decodes Opus: it checks each frame, limits a
talker to 60 frames/s (30 burst), and forwards it only to mod clients that opted in **and that the
game lets hear the talker** (`IVoiceServer::GetClientListening`: team voice in versus, alltalk,
player mutes). A talker costs each listener about 5 KB/s at 32 kbit/s.

Each player is heard once: a mod client drops the game's own voice packets of players listed in
`l4d2vr_voice_peers` (a hook on engine.dll `CClientState::ProcessVoiceData`), while players
without the mod keep getting the game's voice from everyone. Why not reuse the engine's own voice
messages (`CLC_VoiceData`/`SVC_VoiceData`)? Their payload is relayed by the engine
(engine_srv.so), not the game DLL, so filtering it per recipient would mean patching the engine
on every server, and vanilla clients would decode Opus frames as noise; the string-command
channel is already proven on the plugin and the listen host.

## Measured (headless self-tests, 2026-11-03)

Test tone instead of a microphone, voice looped back through the server (`opusvoice` step):

| Server | Frames back | Concealed (FEC+PLC) | Jitter buffer | 440 Hz after the round trip |
|---|---|---|---|---|
| Local listen host | 209/209 | 0% | 60 ms | 68-74 dB over off-tone bins |
| Marmot (dedicated plugin, internet) | 209/209 | 2-8% in the first long talkspurt | 300 ms (its maximum) | 56-64 dB |

On Marmot frames arrive with gaps of up to ~270 ms (the reliable server-to-client channel at
`cl_updaterate 20`); every frame arrives, but some arrive after their playout slot while the
buffer is still growing. A two-player test with real microphones has not been done yet.

## Limits and known gaps

* Opus voice needs both players to have the mod (2.0.5+) and the server to have `OpusVoice`.
* The game's "talking" icon does not light up for a player heard through Opus (their game voice
  packets are dropped); vrmod_log.txt logs voice statistics every 5 s while voice is in use.
* Voice-activation speech reaches mod players only.
* Downstream frames use the reliable channel: a lost packet delays (rather than drops) voice, and
  frames come in bursts; on a remote server that costs up to 300 ms of buffering and some
  concealment at the start of a conversation. A possible next step is an unreliable downstream
  (a user message only mod clients receive).

## Server operators

The dedicated plugin advertises `OpusVoice` (0x2000) whenever it loads; clear the bit with
`FeatureMask` in `addons/l4d2vr_server_config.txt` to keep everyone on the game's voice. It uses
`VoiceServer002` from the engine (no server_srv.so offsets), so it also works when the gameplay
hooks are unavailable.

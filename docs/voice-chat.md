# Opus voice chat (2.0.5, fixed in 2.0.10 and 2.0.12)

L4D2's own voice chat uses a fixed codec (Miles/Speex at 11 kHz; there is no `sv_voicecodec`
to change it), which is why it sounds grainy and is hard to understand. With 2.0.5, players who
have the mod talk to each other with **48 kHz Opus** instead, heard from where each talker stands.
Players without the mod are unaffected and still hear you (and you them) through the game's voice.

## 2.0.12: why voice cut out, and what changed

A Marmot session with three VR players (2.0.11) showed three separate faults:

1. **The relay stopped delivering for 57 of 61 minutes.** It forwarded a frame only where the
   game's voice mask (`IVoiceServer::GetClientListening`) allowed it. That mask is client-driven:
   a listener's row is only built while its client answers the voice manager's `RequestState` with
   `VModEnable 1`; otherwise the whole row is false. The rows went false a few minutes in and the
   relay silently delivered nothing for the rest of the session (one log line). The relay now owns
   the rule (*Who hears whom* below) and logs every listener it cannot reach, with the reason.
2. **22% of the frames were thrown away and playback stalled every few seconds** while it worked:
   frames came as *reliable* server commands, which wait for the previous one's ack, so they
   arrived in clumps of up to a second; the 480 ms jitter buffer overflowed and skipped. Clumps
   of 50 commands also overflowed the client's command buffer (`Cbuf_AddTextWithMarkers: buffer
   overflow` 65 times in the game console). Frames now travel in the *unreliable* stream, packed
   (relay protocol 2), and the jitter buffer keeps 1.28 s and plays a backlog out slightly faster
   instead of discarding it.
3. **The Index right trackpad pressed push-to-talk under a resting thumb** (0.25 force, 39 times in
   5 minutes), and every release sent `-voicerecord`, which stops the game's own microphone even
   in open mic. The trackpad now needs 0.60 force (released below 0.40), and in the game's open
   mic the voice button and combo no longer key `+voicerecord` / `-voicerecord` at all.

Compatibility: a 2.0.12 client on a 2.0.10/2.0.11 server (or the live plugin) says hello with
protocol 2, gets `ack 1` and keeps the reliable relay; a 2.0.10/2.0.11 client on a 2.0.12 server
gets one reliable command per frame as before (now capped while its reliable stream is backed
up). Players without the mod keep the game's own voice both ways.

## Where it works

| Server | Mod players hear each other | Players without the mod |
|---|---|---|
| Dedicated server with the L4D2VR plugin 2.0.5+ (feature `OpusVoice`, 0x2000) | Opus | game voice, both ways |
| Listen server hosted by a mod player (2.0.5+) | Opus | game voice, both ways |
| Any other server (no plugin, older plugin) | game voice | game voice |

VR and desktop players with the mod both use it. Nothing changes for anyone on plain servers.

## Talking

Opus voice follows the game's own voice settings (**Options > Audio**, `OpusVoiceMode=game`, the
default since 2.0.10):

| Game setting | Opus voice |
|---|---|
| Voice communication *Disabled* (`voice_enable 0`) | off (you neither send nor hear Opus; the game's voice is off too) |
| *Push to talk* (`voice_vox 0`) | push to talk, see below |
| *Open mic* (`voice_vox 1`) | voice activation, see below (a voice button still forces it open) |
| Voice receive volume (`voice_scale`) and the master volume (`volume`) | scale Opus playback like the game's voice: `OpusVoiceVolume` x `voice_scale` x `volume` |
| Microphone sensitivity (`voice_threshold`) | how quiet speech still opens Opus open mic (`OpusVoiceVadSensitivity=game`, 2.0.11), see below |

Changes in that menu apply within half a second, in a game or not. `OpusVoiceMode=ptt` or `vad`
fixes the mode regardless of the game's setting.

**VR players** get the game's voice set once to *Open mic*, voice on and the receive volume at its
maximum (`voice_enable 1`, `voice_modenable 1`, `voice_vox 1`, `voice_scale 1`; nobody holds a
voice key in a headset) the first time they are in a game in VR with 2.0.10. It is recorded in
`vr/config_migrations_applied.txt` (`2026-11-09_vr_voice_open_mic`) and saved to `config.cfg`
right away; whatever you set in Options > Audio afterwards is kept. Desktop players keep the
game's defaults (push to talk). The game's microphone sensitivity (`voice_threshold`) is never
changed by the mod; since 2.0.11 Opus voice activation follows it (below).

* **Push to talk** (`OpusVoiceMode=ptt`, or `game` with the game on Push to talk), any of:
  * the game's own voice key (`+voicerecord`, whatever you bound it to);
  * the VR voice combo (`VoiceRecordCombo`, default Crouch+Reload);
  * the **VoicePushToTalk** controller button: the **left menu button** on Quest/Touch, Quest Pro,
    Quest 3, Pico 4, Vive Cosmos, Vive Focus 3 and HP Reverb G2; the **right trackpad press** on
    Valve Index (a firm press: 0.60 force, released below 0.40, since 2.0.12; the left trackpad's
    scoreboard too) (WMR and Vive wands have no free button: use the combo);
  * an extra desktop key (`OpusVoicePushToTalkKey`, for example `V`, `CAPSLOCK`, `MOUSE4`).

  Every push-to-talk also keys the game's own voice, so players without the mod hear you (the
  `OpusVoicePushToTalkKey` key does so since 2.0.11 by running `+voicerecord` / `-voicerecord`). Opus
  keeps sending 160 ms after you let go, so the last syllable is not cut off. With the game on
  *Open mic* (`voice_vox 1`) the buttons, the combo and the key only force Opus open (2.0.12): the
  game's open mic already sends, and the `-voicerecord` on release stopped its capture.
* **Voice activation** (`OpusVoiceMode=vad`, or `game` with the game on Open mic): you are sent
  whenever you speak (`OpusVoiceVadSensitivity`), with 100 ms of pre-roll and a 600 ms hangover
  (2.0.12; 400 ms in 2.0.11, 300 ms before), so word onsets and endings are not clipped; holding a voice button
  also sends. With the game on Open mic, players without the mod hear you
  through the game's open mic as well; with `vad` and the game on Push to talk, only mod players do.
* **Microphone test**: `OpusVoiceLoopback=true` sends your voice back to you through the server,
  as others hear it.

The microphone is the Windows default recording device (Proton: the PulseAudio/PipeWire default
source). Playback uses the default output device, which is also the one the game plays on.

Devices (2.0.11): with push to talk the microphone is opened on the first press and closed 45 s
after the last one (Windows no longer shows it in use all game, and a Bluetooth headset keeps its
stereo profile); with open mic it stays open. A microphone that cannot be opened is tried again
every 15 s (a headset microphone that appears later is picked up). Without a working playback
device (none, or one that stopped returning audio) other players' game voice is not dropped, and
the device is tried again every 10 s. If the stock voice filter cannot be installed (engine.dll
changed), the client does not ask for other players' Opus voice, so nobody is heard twice.
Since 2.0.12 the playback device is open only while voice arrives: it closes 5 s after the last
frame and reopens with the next one (an idle open stream showed as PipeWire underruns).

## Open mic sensitivity and quiet microphones (2.0.11)

Opus voice activation is an energy gate on 20 ms frames with an adaptive noise floor:

* The **noise floor** follows the quietest frame of the last 3 s (2.0.12; was 1.2 s, so 1.2 s of
  talking over gunfire raised it and cut soft word endings) (minimum statistics): it drops
  at once on a quieter frame and rises toward that minimum over about a second. The short pauses
  between words keep it at the room's level while you talk; steady noise (a fan, hum, a PC) has no
  pauses, so the floor rises to it and it never holds the gate open (noise that suddenly appears
  can open the gate for a few seconds at most, once).
* The gate **opens** after two frames above `max(noise floor x ratio, absolute floor)` and closes
  after 600 ms below 55% of that. With sensitivity `s` (0..1): ratio `5 - 2s` (5x .. 3x) and
  absolute floor `400 x 0.05^s` RMS (400 .. 20; 66 at 0.6), in 16-bit sample units. Speech at
  RMS 150 opens over a room noise of up to ~40 RMS at the default.
* `OpusVoiceVadSensitivity=game` (the default since 2.0.11; the old shipped `0.55` is migrated
  once, any other number you set is kept): the game's **Microphone sensitivity** slider in
  Options > Audio decides. That slider is `voice_threshold`, 1..2000; the engine default 2000 is
  the slider's least sensitive end and lower is more sensitive. (engine.dll's open mic treats a
  captured buffer as silence when `voice_threshold` is above both its peak-to-peak range and its
  highest sample, with no noise tracking: at 2000 it needs speech of RMS ~330, at 65 almost
  anything opens it.) Opus maps it logarithmically: 2000 -> 0.6 (the built-in default), 20 and
  below -> 1.0; for example 200 -> 0.8, 65 -> 0.9. A number 0..1 overrides the slider. Without the
  game's cvars (not read yet) the sensitivity is 0.6.
* **Microphone AGC** (`OpusVoiceMicAgc=true`): frames that are sent are raised toward speech at
  RMS ~3000 (about -21 dBFS), at most 4x, never lowered, ramped and limited so nothing clips.
  The level is learnt from speech frames only (the gate open and the frame above its close
  level), so background noise never raises the gain; the noise between words while you talk is
  raised by the same gain as your voice. The gate itself always looks at the signal before the
  AGC (after `OpusVoiceMicGain`). Listeners' own AGC (`OpusVoiceAgc`) still evens out talkers.
* **Log**: every 10 s while the microphone runs (and something happened or the levels moved, or
  once a minute) `vrmod_log.txt` gets a line like

  `[VR][OpusVoice] mic: 500 frames, level avg 41, speech avg 230 peak 610 (120 frames), noise floor 28, openAt 90 (sensitivity 0.90, game voice_threshold 65.0), gate open 31.4%, 3 talkspurts, 0 clipped frames, mic agc 3.10x, mode open mic`

  *level avg* is the RMS of all frames, *speech* the frames the gate called speech, *openAt* the
  level that opens the gate right now. If you are not heard, compare *speech avg* with *openAt*;
  *clipped frames* counts frames at full scale (lower `OpusVoiceMicGain` or the system mic level).
  The game voice settings line also shows `voice_threshold` and the resulting sensitivity.

## Settings (`VR/config.txt`, also in the config panel under *Voice Chat*)

| Key | Default | Meaning |
|---|---|---|
| `OpusVoiceEnabled` | `true` | Opus voice with mod players (false: the game's voice only) |
| `OpusVoiceMode` | `game` | `game` follow the game's Push to talk / Open mic (2.0.10; was `ptt`, migrated once), `ptt` push to talk, `vad` voice activation |
| `OpusVoiceVadSensitivity` | `game` | `game` = the game's Microphone sensitivity (`voice_threshold`, 2.0.11; was `0.55`, migrated once), or 0..1, higher opens on quieter speech |
| `OpusVoiceBitrate` | `32000` | bit/s you send, 12000-64000 |
| `OpusVoiceFec` | `true` | in-band forward error correction |
| `OpusVoiceExpectedLossPercent` | `5` | loss the FEC is tuned for, 0-40 |
| `OpusVoiceMicGain` | `1.0` | microphone gain, 0-4 |
| `OpusVoiceMicAgc` | `true` | raise a quiet microphone (up to 4x) before sending, learnt from speech only (2.0.11) |
| `OpusVoiceVolume` | `1.0` | voice chat volume, 0-2, times the game's `voice_scale` and `volume` |
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
* In-band FEC: a frame lost on the way is rebuilt from the next frame. The encoder is tuned for
  `OpusVoiceExpectedLossPercent` (5%) and, since 2.0.12, for the loss measured on the voice the
  client receives (every 5 s, up to 20%).
* Jitter buffer per talker (2.0.12): starts 60 ms behind and grows up to 300 ms when frames arrive
  unevenly (shrinking again between talkspurts), with room for 1.28 s (was 480 ms). A frame that is
  late (nothing after it has arrived yet) is waited for, with Opus packet-loss concealment filling
  the gap, so it still plays; a frame that is missing while later ones are there is rebuilt with
  FEC from the next frame, else concealed. A backlog after a stall is no longer skipped: while more
  than the target is buffered, one frame per 200 ms is decoded and not played, and only right after
  a quiet frame (a pause between words); only a backlog 800 ms beyond the target is cut. A second
  copy of a frame (the relay's redundancy) counts as a duplicate, not as late.
* Automatic gain control per talker, a soft limiter on the mix, playback ~50 ms buffered.
* Mouth to ear: about 20 ms capture + upload + server tick + relay + 60-300 ms jitter buffer +
  50 ms playback. With relay protocol 2 (2.0.12) frames come with every server packet
  (`cl_updaterate`, 20-30 per second) and a lost packet costs one or two frames (FEC, the
  redundant copy or concealment) instead of stalling everything behind it, so the buffer stays
  near its minimum. Through a 2.0.11 server the reliable channel still delivers in bursts.

## Positional playback

Each talker is heard at their head (their player position + 64 units). Within 1.5 m at full
level; then a gentle roll-off (half at about 7.5 m); left/right pan with constant power, at most
70% to one side, from your head's yaw (VR) or view yaw (desktop). From
`OpusVoiceSpatialDistanceMeters` on, and never quieter than that, a talker is a centred radio voice
at `OpusVoiceRadioVolume`, so a teammate across the map stays easy to understand.

## How it travels

The mod's existing string-command channel (the same one the pose relay uses):

```
client -> server   l4d2vr_voice_hello <protocol> <flags>   opt in: 1 receive, 2 transmit, 4 loopback
server -> client   l4d2vr_voice_ack <protocol>             1 = reliable relay, 2 = packed/unreliable (2.0.12)
client -> server   l4d2vr_voice_up <frame>                 one 20 ms frame, unreliable (ServerCmd(.., false))
server -> client   l4d2vr_voice_rx <player> <frame>        protocol 1: reliable, one frame per command
server -> client   l4d2vr_voice_rx <player> <frame> [<frame> [<frame>]]
                                                           protocol 2: unreliable stream, once per server frame
server -> client   l4d2vr_voice_peers <lo> <hi>            players who send Opus
```

2.0.12 clients say hello with protocol 2; the server acks 2 when it can write to that client's
unreliable stream, else 1 (2.0.10/2.0.11 servers always ack 1, and 2.0.10/2.0.11 clients say 1).
Protocol 2 writes the rx line as a `NET_StringCmd` (type 5, 6-bit type field) with
`INetChannel::SendData(msg, bReliable = false)` on the listener's net channel
(`IVEngineServer::GetPlayerNetInfo`): it goes out with the next datagram or is lost, and the client
runs it exactly like the reliable command. The engine functions are checked by their code bytes
before the first call (engine_srv.so: `SendData` slot 43; engine.dll: slot 42; see
`server_core/vr_net_channel.h`); anything unexpected keeps the reliable path and is logged. A line
holds up to 3 frames and stays under 500 characters (the tokenizer takes 512); the first line of a
flush repeats the talker's previous frame when it fits (dropped on arrival as a duplicate if the
original came); one listener gets at most 1000 bytes per server frame (the rest is dropped and
concealed). Protocol 1 listeners get one reliable command per frame, but none while their reliable
stream already holds 1.5 KB (the 2.0.11 client's command buffer overflowed on the burst after a
stall; dropping there keeps the other server commands intact).

`<frame>` is base64url of a 6-byte header (version, Start/End flags, 16-bit sequence, talkspurt
id, length) and up to 160 Opus bytes; a whole `voice_up` stays under the 250 characters that
`IVEngineClient::ServerCmd` allows. The server never decodes Opus: it checks each frame, limits a
talker to 60 frames/s (bursts of 100 since 2.0.12, was 50), and forwards it only to mod clients
that opted in **and may hear the talker** (below). A talker costs each listener about 5 KB/s at
32 kbit/s (about 8 KB/s with protocol 2's redundant copies).

### Who hears whom (2.0.12)

2.0.5-2.0.11 used the game's voice mask alone. Its rows are client-driven state (built only while
a client answers the voice manager with `VModEnable 1`; `vban` mutes clear single bits) that the
relay cannot see or repair, so an all-false row meant no Opus voice at all, silently. Now, for a
listener and a talker:

1. the game's mask says yes -> heard;
2. different teams (from `PlayerInfoManager002`), `sv_alltalk` off, and the listener is not a
   spectator or unassigned (an idle "take a break" player is a spectator) -> not heard;
3. the listener's mask row hears some other player (bots count) -> the row is in use, so this
   false bit is a mute -> not heard;
4. otherwise the row is unused -> heard.

Teams and rows are re-read once a second. A host that cannot read teams (no
`PlayerInfoManager002`) applies 1, 3 and 4. With only two players on a server, rule 3 has nobody
else to look at, so a mute cannot be told from an unused row and the talker is heard. Since 2.0.11 one listener
receives at most 250 frames/s (five talkers at full rate; more are cut, the jitter buffer conceals
the gaps), hellos are limited to one per second (bursts of 4) and an opt-out hello no longer
resets a talker's rate limit.

Each player is heard once: a mod client drops the game's own voice packets of a player listed in
`l4d2vr_voice_peers` (a hook on engine.dll `CClientState::ProcessVoiceData`) **only while that
player's Opus frames are arriving** (from each frame for 1.5 s on the reliable relay, 0.3 s on the
unreliable one since 2.0.12, from an End frame for 0.4 s, the jitter buffer's tail), and never
while that talker's jitter buffer is waiting for frames that have not come (2.0.12: during a
stall the game's voice plays instead of neither). Players without the mod keep getting the game's voice from everyone, and a
mod player who talks only through the game's voice (open mic while Opus waits for push to talk,
no microphone for Opus, Opus turned off) is heard through it. When a talker starts after a pause,
the first packet or two of the game's voice can play before the first Opus frame arrives.

### What went wrong in 2.0.5-2.0.9

`l4d2vr_voice_peers` lists every mod player whose hello says it *may* transmit, and every mod
client dropped those players' game voice unconditionally. Opus shipped as push to talk
(`OpusVoiceMode=ptt`) and ignored the game's voice settings, so a player on the game's Open mic
(the obvious choice in VR) sent only the game's voice, which every other mod player dropped, and
sent Opus only while a push-to-talk button was held. Two mod players on open mic heard nothing of
each other, in either direction. Seen in a Marmot session (2026-11-10): the other player sent no
Opus frame all session (the plugin never logged a relayed frame for them) while their game voice
reached this client and was dropped (`stockDropped` rising with `sent` unchanged); this client's
Opus frames were relayed only during the occasional push-to-talk presses. Fixed in 2.0.10 by
dropping game voice only while Opus actually arrives, `OpusVoiceMode=game`, and the one-time VR
open-mic default. Why not reuse the engine's own voice
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

* Opus voice needs both players to have the mod (2.0.5+) and the server to have `OpusVoice`. A
  2.0.5-2.0.9 client still drops the game's voice of every Opus peer, so it hears an open-mic
  2.0.10 player only through Opus (which 2.0.10 sends on open mic); update everyone.
* The game's "talking" icon does not light up for a player heard through Opus (their game voice
  packets are dropped); vrmod_log.txt logs voice statistics every 5 s while voice is in use.
* `OpusVoiceMode=vad` with the game on Push to talk reaches mod players only.
* Through a 2.0.10/2.0.11 server (or the Windows dedicated plugin, which has no unreliable path)
  downstream frames still use the reliable channel: a lost packet delays (rather than drops) voice,
  and frames come in bursts.
* Relay protocol 2 has unit tests and a plugin harness, but no measurement over a real network
  yet; `relay=` in the client stats line and the `downlink` word in the server's hello line show
  which transport is in use.

## Server operators

The plugin logs, every 1500 frames of a talker, the frames relayed, the copies delivered to
listeners (and how many of them the game's mask alone would have refused) and the frames rejected
(malformed or over the rate limit: 60 frames/s, bursts of 100 since 2.0.12). Since 2.0.12 it also
logs:

* each Opus hello with the downlink it gets (`packed, unreliable stream` or `reliable` and why);
* every change of who cannot hear a talker, with the reason, both teams, `sv_alltalk`, the game
  mask bit and the listener's raw mask row, e.g.
  `player 1 Opus voice does not reach player 5: other team (sv_alltalk 0) (teams 3/2, sv_alltalk 0, game voice mask 0, receiver's mask row 30 0)`,
  and `... reaches player 5 again` when it ends;
* every 30 s while no Opus listener may hear a talker: `still no Opus listener may hear this player`;
* rejected frames once per 30 s, and frames not sent because a listener's reliable stream is
  backed up or its datagram budget is used up.

The client logs `no Opus voice from the server for N s while other players' game voice arrives`
when the relay goes quiet for 20 s while peers' game voice still comes, and its 5 s stats line
shows `relay=`, `late`, `dup`, `overflow`, `skipped`, `faster` (frames dropped by time
compression), `stalls` and the measured `loss`.

The dedicated plugin advertises `OpusVoice` (0x2000) whenever it loads; clear the bit with
`FeatureMask` in `addons/l4d2vr_server_config.txt` to keep everyone on the game's voice. It uses
`VoiceServer002` from the engine (no server_srv.so offsets), so it also works when the gameplay
hooks are unavailable (clients before 2.0.11 ignored the feature ack in that case and used the
game's voice).

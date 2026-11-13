#!/usr/bin/env python3
"""Build the client release zip (l4d2vr.zip).

Usage:
  package_client_release.py --base BASE.zip --dll d3d9.dll --version 1.3.0 --out l4d2vr.zip

BASE.zip supplies the binary assets this repository does not track (sounds,
overlays, images, UI Font Fix.vpk, ...), e.g. a previous l4d2vr.zip.
Everything the repository does track is taken from the checkout.

The release is OpenXR-only: it ships the 32-bit Khronos openxr_loader.dll and
no openvr_api.dll, SteamVR action manifest, bindings or manifest.vrmanifest
(those stay in the repository for legacy developer builds made with
/p:L4D2VRLegacyOpenVR=true). OpenVR files coming from BASE.zip are dropped, and
a d3d9.dll that imports openvr_api.dll (a legacy build) is refused.

vr/config.sample is written with every parameter commented out ("#Key=value").
On startup the mod forces each *uncommented* sample value onto the player's
config.txt, so a fully commented sample only fills in missing keys; changed
defaults for existing players belong in the one-shot config migrations.
"""
import argparse
import fnmatch
import io
import json
import re
import struct
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KEY_LINE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*\s*=")

# The OpenXR backend loads openxr_loader.dll from d3d9.dll's folder
# (thirdparty/openxr/README.md: Khronos OpenXR-SDK release-1.1.65, Win32),
# shipped with its licences (Apache-2.0; jsoncpp inside the loader).
OPENXR_RUNTIME_FILES = {
    "openxr_loader.dll": "thirdparty/openxr/bin/win32/openxr_loader.dll",
    "vr/licenses/openxr_loader-LICENSE.txt": "thirdparty/openxr/LICENSE",
    "vr/licenses/jsoncpp-LICENSE.txt": "thirdparty/openxr/LICENSE.jsoncpp",
    "vr/THIRD_PARTY_NOTICES.md": "L4D2VR/THIRD_PARTY_NOTICES.md",
}

DROP_FROM_BASE = {"SHOTGUN-SHELL-FIX.txt", "patch-shotgun-shell-release.py", "updatelog.txt"}

# OpenVR files of the releases before the OpenXR-only one. Never shipped now
# (the installer also removes them from players' game folders on update).
OPENVR_FILES = ("openvr_api.dll", "vr/manifest.vrmanifest", "vr/SteamVRActionManifest/*")
REQUIRED_FILES = ("d3d9.dll", "openxr_loader.dll", "manifest.json", "vr/config.sample", "vr/version.txt")

CHANGES = [
    "Pavlov-style body holsters: grip an item at its holster to draw it instantly.",
    "Physical reloading: magazine release button, hand-pulled AK magazine, chambering, spare magazine at the waist, dropped magazines, glowing magazine when empty.",
    "True akimbo, motion melee with weapon inertia, physical shoves, hand-thrown grenades, pills to the mouth and adrenaline stabs.",
    "Crosshair aligned with the gun sights and a dynamic spread crosshair; picture-in-picture scope lens.",
    "Loading screens and intermissions stay visible in the headset; wrist HUDs on by default; background audio.",
    "Other VR players' bodies crouch with them; desktop players with the mod see VR bodies, and the mod starts in desktop mode when no headset is found.",
    "Dedicated server plugin (Linux and Windows) with full support for every feature; plain servers use client-side fallbacks.",
    "1.2.1: items drawn from a holster are held by the grip and dropped when you let go (HolsterReleaseDrop).",
    "1.2.1: hand pills or adrenaline to a teammate by holding them against their chest.",
    "1.2.1: Half-Life 2 VR style two-handed aim with an adaptive stabilizer; the front hand no longer drifts.",
    "1.2.2: toggle grips (GripToggle): a firm squeeze takes an item, the next squeeze holsters, swaps or drops it.",
    "1.2.2: reloading tuned to Pavlov VR per gun (slide/bolt/pump pull, chamber point, magazine depth), with a forgiving magazine well and a pouch glow.",
    "1.2.2: reload hints: the spare magazine and the bolt, slide or pump glow when the reload needs them.",
    "1.2.2: point at menus with either controller and click with the trigger (MenuLaserPointerEnabled).",
    "1.2.2: right stick forward jumps and back crouches (StickJumpCrouch); grips are only for gripping.",
    "1.2.2: holsters use Pavlov VR's body layout (hips, chest pockets, back) and follow your head; look down to see them; a haptic tick on hover; either hand swaps items at any holster. Turning no longer triggers melee bashes.",
    "1.2.2: a reload takes the full process, magazine in and then the slide; the release button drops one magazine and returns its rounds to the reserve.",
    "1.2.2: spare magazines and shells are held the way they sit in the gun; glows are a soft, steady tint.",
    "1.2.2: the wrist HUD uses the game's own health bars and font with full teammate names; your own bar is the large one.",
    "1.2.2: right stick down toggles crouch; guns never play the swing or bash animation.",
    "1.2.2: the two-handed grip lets go when the support hand moves 20 cm from the foregrip; a shot the server never fired (right after a draw) no longer needs a rack or pump.",
    "1.2.2: melee and shoves are motion only (no trigger bash or shove animation); the melee grip sits closer to the base of the handle.",
    "1.2.2: Pavlov magazine slap on the AK-47, SG552 and military sniper: grab a fresh magazine with the old one still in and strike the old one out (MagazineSlapEnabled).",
    "OpenXR: the mod now talks to the headset through OpenXR only (no SteamVR/OpenVR files). Windows needs a 32-bit OpenXR runtime: SteamVR 2.17 or newer, Virtual Desktop or Meta Quest Link. Linux needs Proton Hotfix or Experimental with PROTON_USE_WOW64=1 (the installer sets both).",
    "OpenXR: controller bindings are built into the mod (SteamVR's binding screen no longer applies); menus use the controller laser and the in-mod keyboard.",
    "OpenXR: without a usable OpenXR runtime the game starts in desktop mode and vrmod_log.txt and the game console say why.",
    "2.0.0: Pavlov VR recoil per gun (same values as Pavlov's equivalent guns): the gun kicks in your hand and recovers; holding the foregrip with the other hand tames heavy recoil. The gun no longer sways or plays L4D2's firing animation in your hand (GunRigidInHand).",
    "2.0.0: no bullet spread for VR players: bullets go exactly along the iron sights, aligned on every gun; the crosshair is off by default (desktop players keep vanilla spread and crosshair).",
    "2.0.0: shotguns fire the same evenly spread pellet pattern every shot, at L4D2's own pattern size (on servers running the mod).",
    "2.0.0: ambidextrous: the hand that grabs an item holds and uses it; grips grab, triggers do magazines, slides, bolts and pumps (as in Pavlov); right stick click toggles the flashlight; no draw animation when switching.",
    "2.0.0: physical magazine slap, hand-thrown grenades that leave your hand with its real motion, items you let go drop at your hand, and a more reliable motion bash that never triggers from turning.",
    "2.0.0: dual pistols reload physically: magazine release, bring one pistol to the other hand to stack them, take the double magazine with the free hand, then rack each empty slide (DualPistolsPhysicalReloadEnabled).",
    "2.0.0: melee swings feel like Ancient Dungeon VR: the grip stays in your hand and the blade trails behind fast swings; only fast, committed swings count; hits are felt in the controller.",
    "2.0.0: survivor hand poses follow Pavlov's hand poses for holding guns, magazines, slides, grenades and items. The separate SteamVR glove models are retired.",
    "2.0.2: easier shotgun pumping: the pump cocks at 7 of its 10 cm and chambers anywhere in the last 3.5 cm, a fast rack is never missed at any frame rate, the pump is caught from further away and stays caught through a tracking blip, and holding the foregrip racks it too (ManualPump... settings).",
    "2.0.2: stance recoil: guns kick less crouched, much more while jumping, and more the faster you move, following each gun's own L4D2 accuracy values (RecoilStanceEnabled and per-part scales).",
    "2.0.10: SteamVR on Windows: when SteamVR is the active OpenXR runtime but the 32-bit registration Left 4 Dead 2 reads is missing or points to another runtime (SteamVR set as the OpenXR runtime before 2.17), the mod uses SteamVR's own 32-bit runtime (steamxr_win32.json) instead of starting in desktop mode; an old SteamVR without 32-bit OpenXR is named in the log with the fix (update SteamVR, then Set SteamVR as OpenXR Runtime).",
    "2.0.10: vrmod_log.txt always goes next to left4dead2.exe and starts with the mod version, the d3d9.dll path, Windows or Proton and the render path (DXVK inside d3d9.dll, OpenXR over Vulkan), so a log from another VR mod or an old run is easy to tell apart. A slow first start no longer gives up on client.dll after 30 s (it waits up to 10 minutes).",
    "2.0.10: voice chat works again between mod players: 2.0.5-2.0.9 dropped the game's own voice of every player with the mod while their high-quality (Opus) voice only went out on push to talk, so open-mic players heard nothing of each other. The game's voice of a mod player is now dropped only while their Opus voice is actually arriving, so you always hear them one way or the other; players without the mod still hear you and you them through the game's voice.",
    "2.0.10: voice chat follows the game's own voice settings (Options > Audio): Disabled turns it off, Push to talk or Open mic chooses how you are sent (OpusVoiceMode=game, the new default; ptt and vad still fix the mode), and the voice receive volume and master volume set its volume, live, whenever you change them.",
    "2.0.10: VR players get the game's voice set once to Open mic with voice on and the receive volume at full the first time they play in VR with 2.0.10 (no button to hold in the headset); anything you change in Options > Audio afterwards is kept. Desktop players keep the game's defaults.",
    "2.0.10: server plugin: Opus voice can burst one second after a hitch (was 0.6 s, which dropped voice frames), and its log shows how many copies of each talker's voice were delivered and when the game's voice rules let no one hear a talker.",
    "2.0.9: items on your body can be grabbed anywhere on them, not just at the handle: the slung rifle from the butt to the muzzle, a holstered pistol by its barrel, a melee weapon by its blade, a kit, grenade or pills anywhere. You still end up holding the item by its grip. Where items overlap, the one your hand is actually on wins, so the long gun never steals a hip or chest grab, and a hand on the gun's foregrip, bolt or magazines is never taken by a holster.",
    "2.0.9: a light haptic tick when an empty hand first touches something it can grab (an item on your body, the gun's foregrip, bolt or pump, a spare magazine or the pouch). Once per touch, no buzzing while you rest there (GrabHoverHapticEnabled, GrabHoverHapticAmplitude, GrabHoverHapticDurationMs; also in the VR Options menu).",
    "2.0.8: gun bashes trigger on a natural swing: any fast swing of the gun in front of you shoves (sideways either way, a downward chop, an upward butt stroke or a push). The swing is measured at the gun's muzzle as well as the hand; the barrel no longer has to point along the motion, a hard wrist turn no longer cancels it, and a wind-up no longer cancels the swing's travel. Aiming, quick re-aims, turning your body, walking and reaching for holsters still never bash. One bash per 0.7 s (the game's own shove interval), and swinging back the other way re-arms it.",
    "2.0.8: you can bash with an empty gun: swings with the magazine out (waiting for a fresh magazine, or shells) never shoved in 2.0.7. Only a hand on the bolt, slide or pump pauses bashes.",
    "2.0.7: each arm comes from its own shoulder again: without the first-person body the shoulders only followed the stick turn, not where you physically face, so after turning around in your room the left hand's arm came from the right shoulder and the right hand's from the left. The shoulders now follow your torso (the head's direction through the same comfort cone the body uses), so glancing around still leaves them put.",
    "2.0.7: the crowbar is held by its flat end, not by the middle (the hand sat at the middle of the bar, its claw end in front); the fire axe is held nearer the end of its handle. The weapon slides along its own length; the hand stays on the controller and the Ancient Dungeon lag still pivots at the hand. The other melee weapons were already held at the handle.",
    "2.0.6: reloads work again with the gun hand on the controller: the magazine well, bolt, pump and foregrip grab points, the shotgun's flashlight, the muzzle and the scope now move with the drawn gun (2.0.5 moved only the picture of the gun, so a magazine at the drawn well did not go in and the flashlight shone from the old place).",
    "2.0.6: the right hand sits on your real hand: its default offset now mirrors the left hand's (NativeViewmodelRightHandPoseOffsetMeters=0,-0.08,0.1; 0,0,0 put the wrist 10 cm in front of your hand), changed once in existing configs that still have 0,0,0; guns and melee weapons follow the same hand position.",
    "2.0.6: melee weapons are held like in Ancient Dungeon VR again: the hand stays on the controller, at the handle, and the weapon trails in it during fast swings, pivoting at your hand (2.0.5 swung hand and weapon together about a point in front of the hand).",
    "2.0.5: VR Options menu for the headset: hold the pause button (left B on Index, Y on Quest, menu on Vive/WMR) or point at the VR Options button above the main or pause menu (also F8 or vroptions). Big pages for Comfort, Hands & Body, Weapons, Grenades, HUD, Audio and Controls, plus Advanced with every setting; changes apply at once and are saved to vr/config.txt; desktop players get the same page in a window.",
    "2.0.5: each page can be reset to its defaults, and 'Reset all VR options' (confirmed in VR) backs up vr/config.txt to vr/config.txt.bak-<time>, restores the shipped defaults live and lists anything that needs a restart; the game's own video, audio and key settings are never touched.",
    "2.0.5: physics throw assistance (GrenadeThrowAssist, strength 0.5): in GrenadeThrowMode=physics a hand throw that is already heading toward where your hand points is gently steered onto that spot; throws elsewhere, drops and tosses stay your own. Strength 0 to 1 on the Grenades page.",
    "2.0.5: Recenter and set height on the Comfort page (seated play: press it seated).",
    "2.0.4: the shotgun pump needs the off hand's trigger: hold the foregrip, squeeze that hand's trigger and pump. The foregrip's grip alone no longer racks it (ManualPumpForegripRacks, now off).",
    "2.0.4: pump shotguns fire shell after shell; the pump is only needed to chamber a shell loaded after the gun ran dry (ManualPumpEveryShot=true brings back a pump after every shot).",
    "2.0.4: the support hand stays on the pump shotgun's foregrip through each shot and while it pumps; the gun keeps aiming from both hands while the pump waits.",
    "2.0.4: scopes work: configs from before the physical overhaul kept ScopeEnabled=false, so scopes never rendered; it is switched on once. The lens shows the zoomed picture whenever your eye is behind the scope within ScopeEyeboxDeg (now 15 degrees, was 8) of its axis; before, the eye had to sit within a few millimetres of the axis.",
    "2.0.4: grenades are thrown like in the original L4D2VR mod by default (GrenadeThrowMode=arc): holding one shows the arc from your hand, aim with the hand, pull and let go of the trigger and the game's own throw lands where the arc shows, on any server. The Pavlov-style hand throw is GrenadeThrowMode=physics, and now takes your hand's peak speed in the last 80 ms with a 2.5x boost (was 1.5x; a dedicated server needs the new plugin for that boost).",
    "2.0.4: the SCAR's 3-round burst kicks like Pavlov's burst rifle (the M16) instead of a semi-auto marksman rifle, about 5x less; two-handed it barely climbs.",
    "2.0.4: reloads closer to Pavlov: the pistol, magnum, M16, SCAR and hunting rifle lock open when empty (take the slide or bolt and let go to chamber); pistol, magnum, hunting rifle and scout magazines and shotgun shells go in as soon as they are lined up in the well; magazines seat at 95% of their depth; the longest bolts pull 11.4 cm.",
    "2.0.4: grenades and pills sit visibly on your chest: the chest pockets moved 10 cm forward and 4 cm down (they were under your chin, out of view) and take a slightly wider reach.",
    "2.0.4: an item in your left hand is always held by a mirrored left hand, on every hand setting (also with NativeViewmodelHandsOnly off or arm cropping on); the right hand is never moved onto the left controller (ItemHandMirror retired).",
    "2.0.4: chest sling: letting go of a rifle, SMG, shotgun or sniper near your body hangs it on your chest with both hands free; grab its handle or foregrip with either hand to take it (that hand becomes the trigger hand); let go at arm's length to drop it (ChestSlingEnabled).",
    "2.0.4: grabbing closer to Pavlov: the support hand catches the foregrip from further away (VrHandsTwoHandedGripTargetBoxScale 1.5), and a slung gun is taken anywhere along its body.",
    "2.0.4: the weapon idle poses learned by older versions are relearned once: cached ones put the hand on the auto shotgun and SPAS 21 cm away from the controller.",
    "2.0.4: wrist HUDs come back: they were hidden whenever the runtime reported a controller as valid but not tracked; they now also keep the last picture while a menu is open and fall back to the simple wrist panels if the game HUD cannot be captured; both wrist HUDs are switched back on once for existing configs.",
    "2.0.4: no more debug boxes around the magazine and bolt while two-handing (MagazineBoxDebugEnabled and ShowInventoryAnchors off, also for existing configs).",
    "2.0.4: the full first-person body is now off by default (FirstPersonBodyEnabled; turned off once for existing configs). When you turn it on, your body's arms reach your VR hands (two-bone IK from the body's own shoulders, natural elbows, one hand per side, a small stretch instead of a detached hand) and your torso turns to follow your head (after a 30 degree deadband, at once when a hand goes far out to the side); other VR players' bodies turn the same way (FirstPersonBodyArmIK, FirstPersonBodyYawDeadbandDeg).",
    "2.0.5: every gun's hand sits on your controller (GunHandOnController): the drawn hand came from each gun's idle animation, so the magnum's hand was 9 cm off, the Uzi's and MAC-10's 13 cm behind and the military sniper's 17 cm ahead; now every gun is within a millimetre of where your empty hand sits, in both hand modes and in either hand. Only the gun's position moves, so the sights keep their line.",
    "2.0.5: the aim laser (when you turn it on) starts at your controller like in the original L4D2VR mod instead of running along the gun's sights, and still ends where your bullet lands (VanillaLaserPlacement); bullets keep following the sights.",
    "2.0.5: an item in your left hand is held by the survivor's own left arm and hand (ItemHandLeftArmModel) instead of the right arm drawn mirrored; the right arm is your free hand.",
    "2.0.5: updating keeps proper defaults: hand settings carried over from older builds (classic viewmodel with cropped arms, arm bend and trim, old freeze-pose offsets, inverted and pre-curled finger curls, glove curls, the left-handed viewmodel offset, VR hands or physical reloads off, viewmodel adjust on) are switched to the current defaults once, but only where your config still has the old default; values you changed are kept.",
    "2.0.5: the code and config-panel defaults now match the shipped config (the panel used to write e.g. cropped arms and VR hands off for keys missing from your config), and a new install no longer runs the update migrations on its second start.",
    "2.0.5: holsters follow Forefront's layout (decoded from the game): 60 cm below your head and turning with it every frame (HolsterYawFollowSpeed 0, was 20 for existing configs too). Long gun on a sling across your chest (grip on your dominant side, barrel toward the other hip), pistol or melee on your dominant hip hanging from its grip, grenade on your dominant upper chest, pills/adrenaline on your other hip, spare magazines on your off-hand upper chest. The medkit, defibrillator and upgrade packs stay on your back over the off shoulder. A squeeze takes the nearest item whose grab area your hand is in.",
    "2.0.5: empty hands, as in Forefront and Pavlov (HolsterEmptyHands, on): putting an item away in its holster leaves that hand empty instead of switching weapons; with both hands empty you hold nothing (the item stays on your body and cannot fire). Squeeze at a holster with an empty hand to draw into that hand. Letting go of a pistol, magnum or melee weapon near your body puts it back on your hip; at arm's length it drops, and a fast swing still throws a melee weapon.",
    "2.0.5: the magazine pouch stays on your off-hand chest when the other hand holds the gun (reach across for it).",
    "2.0.5: voice chat in high quality: with other players who have the mod, on servers with the L4D2VR plugin or a modded host, voice is 48 kHz Opus (32 kbit/s, error correction, jitter buffer and packet-loss concealment) instead of the game's grainy 11 kHz voice, heard from where each talker stands, with even loudness per talker. Talk with the game's voice key, the voice combo or the new push-to-talk button (Quest: left menu button, Index: right trackpad press); voice activation is optional (OpusVoiceMode=vad). Players without the mod still hear you through the game's own voice (OpusVoice... settings).",
    "2.0.6: no more hand switching: a held gun or item never moves to your other hand. Your other hand's grip on it is only the foregrip (support grip); anywhere else on the gun it does nothing. The hand-off (squeezing the held item with the free hand) and the release transfer (letting go while the other hand held the foregrip) are gone. To use a gun in the other hand, put it on the sling (or in its holster) and take it with that hand.",
    "2.0.6: Pavlov's toggle grip for everyone (switched on once): squeeze to take an item, it stays in your hand without holding the grip; squeeze that hand's grip again to let go: at its holster it is put away (a long gun near your body hangs on the chest sling as before), anywhere else it drops at your hand. Letting go of the grip never drops anything. A squeeze mid-swing throws a melee weapon. GripToggle=false keeps the old hold-to-grip behaviour.",
    "2.0.11: open mic picks up quiet microphones (a Quest over WiVRn, any mic speaking at RMS ~150-400): the voice activation opens at 3-5x the room's noise level instead of 3.2-10x and from a much lower absolute level, keeps 100 ms before a word and 400 ms after it, and learns steady background noise (fans, hum) from the pauses between words, so that noise never holds the microphone open.",
    "2.0.11: the game's Options > Audio Microphone sensitivity slider (voice_threshold) now sets how sensitive open mic is for the high-quality voice too (OpusVoiceVadSensitivity=game, the new default; a number 0..1 still fixes it; the old shipped 0.55 is migrated once).",
    "2.0.11: a quiet microphone is raised up to 4x before it is sent, learnt from your speech only so the noise between words is not pumped up (OpusVoiceMicAgc), and vrmod_log.txt shows your microphone every 10 s ([VR][OpusVoice] mic: level, speech level, noise floor, open threshold, how often the microphone was open, talkspurts, clipped frames).",
    "2.0.11: holsters: squeezing a holster with your free hand while incapacitated (or during a two-pistol reload) no longer moves the pistol to that hand, and a free-hand draw that did not happen gives the item back to the hand that held it; a free-hand draw works even while the other hand still squeezes its grip.",
    "2.0.11: putting the held item away at its own holster always puts it away, even where the slung rifle reaches over the hip (it used to grab the rifle instead); the light buzz at your own holster no longer repeats while the hand rests at its edge or you turn your head.",
    "2.0.11: after the pause menu, the SteamVR/Quest dashboard, a mounted gun or pressing Use with a grenade in the hand, the next squeeze lets the held item go again (it used to do nothing until you holstered and redrew it).",
    "2.0.11: with the Pavlov holster layout off, letting go of a long gun near the chest drops it instead of hanging it on a sling you could not take it back from (which also blocked firing until a restart).",
    "2.0.11: arms and hands no longer vanish for a moment after every stutter or when one controller loses tracking briefly (they keep their last pose for up to half a second); a gun in the left hand keeps its mirrored shape meanwhile.",
    "2.0.11: smoother frames in hordes: each drawn model no longer builds several strings, takes a lock and searches its name every time; the wrist HUD reuses its buffers. With multicore rendering, reload and pose commands are sent from the game's main thread, and rare render-thread deadlocks and lock hand-overs are fixed.",
    "2.0.11: grenades thrown with GrenadeThrowMode=arc land where the arc shows on high maps too, and other players see your head pitch smoothly: the view pitch now reaches a 2.0.11 server or host exactly at any map height (it was rounded by up to 3 degrees above 168 units). Older servers keep the old encoding.",
    "2.0.11: hosting: a player who joins a slot someone else used (or rejoins after a crash) can reload again, and nobody's reloads, recoil or melee stop working after a map change (old command numbers no longer block them); a host in desktop mode resets its VR guests' reload state on each map too.",
    "2.0.11: hosting: a guest's pistol reloads no longer depend on the host's own MagazineInteractionEnabled setting.",
    "2.0.11: desktop-mode players (and mouse mode) can fire dual pistols on modded servers again.",
    "2.0.11: other VR players' heads and hands move smoothly instead of pausing and jumping with every pose packet.",
    "2.0.11: shooting dual pistols, shoving and handing over pills while walking no longer turns your movement toward the hand.",
    "2.0.11: anti-cheat for hosts and server plugins: a client's hand must stay within 1.2 m of its eye and in front of walls and doors (shots, melee, throws and drops no longer start inside or behind them); listen-host physical melee has the server plugin's swing cooldown and is blocked while pinned or incapacitated; swings need real hand motion and end after 1.5 s; map props can only be pulled as the item they really are.",
    "2.0.11: server plugin: switching weapons no longer skips a weapon's fire cycle (instant deploy only removes the draw delay); a spent grenade cannot be thrown again; magazines go in at least 0.3 s after the old one came out, shells one per 0.2 s, dual-pistol reloads one per second (generous for hand reloading).",
    "2.0.11: server plugin: VR features apply only after the VR handshake and honour the FeatureMask bits; poses relayed in bursts keep the newest one; client actions no longer flood the server log; the Windows plugin reads l4d2vr_server_config.txt from left4dead2/addons/, understands MaxHandReachMeters and the other shared keys, keeps grenade aim per player and applies its use/mounted/throw aim while the command runs.",
    "2.0.11: settings saved by the config panel (desktop mirror, HUD curvature, scope options and about 190 others) or added by hand are no longer deleted from vr/config.txt at the next start; only retired keys are removed (logged), renamed scope keys (ScopeFov, ScopeMagnification, ScopeOverlay*) carry your value to their new names, and a duplicated line keeps the one that counts.",
    "2.0.11: changing VR Options during play no longer risks a crash: the config is reloaded only between frames, the reload keeps working when the file is briefly missing (no more desktop message box), and the injected cmd= runs once instead of on every click.",
    "2.0.11: desktop mode (no headset) now reads vr/config.txt, so settings like BackgroundAudio apply there too.",
    "2.0.11: the wrist HUD background no longer fades away after a few VR Options clicks; the Hand mode and Hide arms rows work again.",
    "2.0.11: config values that are not numbers (nan, a typo) keep the default and are logged; a decimal comma (TurnSpeed=0,3) is read as a decimal point; an emptied ...Overrides or CustomActionNCommand line clears it; keys are found in any case; a config saved with a BOM keeps its first line.",
    "2.0.11: VR sessions no longer save their crosshair, film grain, motion blur, fps_max and multicore settings into your config.cfg: your own values are kept and put back the next time you start without VR (desktop mode or -nohmd). -nohmd no longer edits autoexec.cfg, and -bigfonts only removes the mat_setvideomode lines it wrote.",
    "2.0.11: the VR voice defaults keep Voice: Disabled and say in the game console that voice is open mic; if the migration file cannot be written they are not applied (instead of every session).",
    "2.0.11: after a game update that moves the mod's hooks, the game starts in desktop mode with one log summary instead of a message box per signature and a half-hooked game; vr/config.txt is written safely (temp file + rename); vrmod_log.txt keeps the previous session as vrmod_log.prev.txt and shows non-ASCII install paths; the zip ships vr/disable_workshop_updates.txt so the upstream Workshop build is never offered over this fork.",
    "2.0.11: voice chat no longer goes quiet for the rest of the map after a hitch of 3 s or more (loading stall, dragging the window); the voice handshake simply restarts.",
    "2.0.11: a microphone that was not there at the start (headset microphone that appears when streaming starts, USB headset plugged in later) is picked up within 15 s; with push to talk the microphone is opened on the first press and closed 45 s after the last one.",
    "2.0.11: without a working playback device, other mod players are heard through the game's voice instead of not at all, and the device is tried again every 10 s.",
    "2.0.11: Opus voice also works on servers whose gameplay hooks are unavailable after a game update; if the mod cannot hook the game's voice receive, other players are heard once through the game's voice instead of twice. OpusVoicePushToTalkKey also keys the game's own voice, so players without the mod hear you.",
    "2.0.11: server and listen host: voice hellos are rate limited and opting out no longer resets a player's voice rate limit; each listener receives at most five talkers' worth of voice frames per second; a listen host forgets the voice state of players who left.",
    "2.0.11: OpenXR: on Index, Vive wands, Vive Cosmos, Focus 3 and Pico 4 the trigger fires at about a quarter pull again (it needed a full pull since 2.0.0).",
    "2.0.11: OpenXR with multicore rendering (mat_queue_mode): each frame reaches the headset one display frame earlier and carries the head pose it was drawn from, so the view no longer swims slightly when you turn your head.",
    "2.0.11: OpenXR: if the VR session cannot be created (for example the game runs on another GPU than the headset), the game starts in desktop mode with the reason in the log instead of freezing for minutes; if SteamVR or another runtime crashes or closes during play, VR stops cleanly with one log line and a note in the game console.",
    "2.0.11: OpenXR: menus and wrist panels no longer show the state before your last click after a short compositor hitch, and quitting the game while VR runs no longer touches DXVK's device after it is gone; when the game runs as administrator, the log says to start Steam and the game without administrator rights.",
    "2.0.11: installer 1.1.4: \"Write to Steam\" keeps your own launch options exactly as written ($VAR, ~, gamemoderun, extra commands); Steam files with unusual characters no longer stop it, and installed files are readable by other users and server processes.",
    "2.0.11: installer 1.1.4: Install / Update repairs missing or damaged mod files instead of saying \"already up to date\" (new Repair checkbox), never replaces a newer install with the older bundled l4d2vr.zip when offline, and keeps your edited haptics_config.txt / local_client_convars.nut (the new default is saved next to it as .new).",
    "2.0.11: installer 1.1.4: downloads are https-only and must have a published SHA-256 (or --allow-unverified) and resume after a dropped connection; Restore/Uninstall in the window wait for the game to be closed.",
]


def is_openvr_file(name: str) -> bool:
    lowered = name.lower()
    return any(fnmatch.fnmatchcase(lowered, pattern.lower()) for pattern in OPENVR_FILES)


def pe_imports(data: bytes) -> list:
    """DLL names in a PE file's import table (lower case); [] if unreadable."""
    try:
        if data[:2] != b"MZ":
            return []
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            return []
        sections = struct.unpack_from("<H", data, pe + 6)[0]
        optional_size = struct.unpack_from("<H", data, pe + 20)[0]
        optional = pe + 24
        magic = struct.unpack_from("<H", data, optional)[0]
        directories = optional + (96 if magic == 0x10B else 112)
        import_rva = struct.unpack_from("<I", data, directories + 8)[0]
        table = optional + optional_size
        ranges = []
        for i in range(sections):
            entry = table + 40 * i
            vsize, va, rsize, raw = struct.unpack_from("<IIII", data, entry + 8)
            ranges.append((va, max(vsize, rsize), raw))

        def offset(rva):
            for va, size, raw in ranges:
                if va <= rva < va + size:
                    return rva - va + raw
            raise ValueError(rva)

        names = []
        cursor = offset(import_rva)
        while True:
            name_rva = struct.unpack_from("<I", data, cursor + 12)[0]
            if name_rva == 0:
                break
            start = offset(name_rva)
            names.append(data[start:data.index(b"\0", start)].decode("ascii", "replace").lower())
            cursor += 20
        return names
    except (struct.error, ValueError, IndexError):
        return []


def check_package(files: dict) -> list:
    """Problems that must stop the release (empty = fine)."""
    problems = [f"missing {name}" for name in REQUIRED_FILES if name not in files]
    problems += [f"OpenVR file in an OpenXR-only release: {name}" for name in sorted(files) if is_openvr_file(name)]
    imports = pe_imports(files.get("d3d9.dll", b""))
    if not imports:
        problems.append("d3d9.dll has no readable import table (not a PE DLL?)")
    elif "openvr_api.dll" in imports:
        problems.append("d3d9.dll imports openvr_api.dll: that is a legacy OpenVR build (/p:L4D2VRLegacyOpenVR=true), "
                        "not the OpenXR-only release")
    return problems


def commented_sample(text: str) -> str:
    out = []
    for line in text.splitlines():
        out.append("#" + line if KEY_LINE.match(line) else line)
    return "\n".join(out) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, type=Path)
    ap.add_argument("--dll", required=True, type=Path)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()

    files: dict[str, bytes] = {}
    dropped = []
    with zipfile.ZipFile(args.base) as base:
        for info in base.infolist():
            if info.is_dir() or info.filename in DROP_FROM_BASE:
                continue
            if is_openvr_file(info.filename):
                dropped.append(info.filename)
                continue
            files[info.filename] = base.read(info)

    src = REPO / "L4D2VR"
    files["d3d9.dll"] = args.dll.read_bytes()
    # The 32-bit Khronos OpenXR loader (and its licences) next to d3d9.dll.
    for name, path in OPENXR_RUNTIME_FILES.items():
        files[name] = (REPO / path).read_bytes()
    for name in ("haptics_config.txt", "local_client_convars.nut"):
        files[f"vr/{name}"] = (src / name).read_bytes()
    files["vr/config.sample"] = commented_sample((src / "config.txt").read_text(encoding="utf-8")).encode("utf-8")
    files["vr/version.txt"] = args.version.encode("ascii")
    # Zip installs must not offer the upstream (OpenVR) Workshop build over this
    # fork either (2.0.11, review S27); the installer writes the same pin.
    files["vr/disable_workshop_updates.txt"] = (
        "Shipped with L4D2VR (OpenXR fork).\n"
        "While this file exists the mod does not offer to replace itself with the Steam Workshop\n"
        "build. Use the installer or a release zip to update. Delete this file to re-enable Workshop updates.\n"
    ).encode("ascii")
    for resource in sorted((REPO / "resources").rglob("*")):
        if resource.is_file():
            files[resource.relative_to(REPO / "resources").as_posix()] = resource.read_bytes()
    files["PHYSICAL-OVERHAUL.md"] = (REPO / "docs" / "physical-overhaul.md").read_bytes()
    files["manifest.json"] = json.dumps(
        {"version": args.version, "changes": CHANGES, "changesh": []}, ensure_ascii=False, indent=2).encode("utf-8")

    problems = check_package(files)
    if problems:
        raise SystemExit("refusing to package:\n  " + "\n  ".join(problems))
    if dropped:
        print(f"dropped OpenVR files from {args.base.name}: {', '.join(sorted(dropped))}")

    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as out:
        for name in sorted(files):
            out.writestr(name, files[name])
    args.out.write_bytes(buffer.getvalue())
    print(f"{args.out}: {len(files)} files")


if __name__ == "__main__":
    main()

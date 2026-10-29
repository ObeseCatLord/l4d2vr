# Physical interaction overhaul

Pavlov Shack style body holsters and reloading, Half-Life 2 VR style melee, grenades and scopes, true akimbo, consumable gestures, networked full-body IK and a wrist HUD. Every mechanic works on three kinds of server:

| Server | What you get |
|---|---|
| Listen server hosted by a VR player running this build | Full fidelity: the host's hooks apply melee sweeps along the swung blade, controller-ray bullets, hand-velocity throws, physical magazine and shell transactions. |
| Dedicated server running the `l4d2vr_server` plugin | The same, server-side. The plugin advertises what it supports with `l4d2vr_feature_ack`. |
| Any other server | Client-side approximations that only use standard input (weapon slots, view angles, buttons). Desktop players and servers see a normal client. |

Desktop players running the mod are unaffected; they can join VR servers and play alongside VR players. Without a headset the mod starts in desktop mode by itself: with no OpenVR runtime installed it never tries VR, and if VR fails to start it continues on the desktop instead of showing an error (`DesktopModeAuto=true`; `-nohmd` still runs the game fully vanilla). Desktop players with the mod see VR players' bodies.

## Holsters

Reach to a holster and squeeze the weapon-hand grip: the item is in your hand immediately, with a haptic pulse, and the hovered item glows.

Grips are toggles, as in Pavlov (`GripToggle`, on by default). One firm squeeze takes an item and your hand keeps it after you let go; on Index controllers that means a hard squeeze, not a light grip. The next squeeze lets go of it:

- inside the item's own holster, it is put away;
- over another holster, you swap to that item;
- anywhere else, it is dropped.

A squeeze that takes nothing is ignored: only an item the grip itself took (drawn from a holster or picked up with the grip) can be dropped by letting go. A gun you spawned with, picked up with Use or switched to stays in your hand however often you squeeze. With `GripToggle=false` you hold the grip to keep an item instead. The two-handed foregrip is also a toggle: squeeze once to take the front of the gun and again to let go (`VrHandsTwoHandedGripHeldMode=false`). Magazines, bolts and slides are held only while you keep squeezing.

The layout copies Pavlov VR's body inventory, read from the game's own data (`research/pavlov/inventory/SPEC.md`):

- **Root:** an inventory root hangs 75 cm below the headset. It moves back a little when you look down, and up to 60 cm back when your off hand reaches to the floor.
- **Facing:** the root turns with your head continuously (`HolsterYawFollowSpeed`, 20 as in Pavlov), and stick or snap turns move it instantly.
- **Hand reach:** your hand counts as an 11.5 cm sphere.

| Holster | Where | Contents |
|---|---|---|
| Back | 40 cm sphere behind your head (follows your head) | Primary weapon |
| Dominant hip | Upright capsule beside the root (radius 20 cm, 88 cm tall) | Pistol(s) or melee weapon |
| Chest, off-hand side (upper pocket) | Reach for the item | Grenade |
| Chest, centre (lower pocket) | Reach for the item | Medkit, defibrillator or upgrade pack |
| Chest, dominant side (upper pocket) | Reach for the item | Pills or adrenaline |
| Off-hand waist | Reach for it | Spare magazine (or a shell for shotguns) while you have reserve ammo |

Chest items go back in when your hand is anywhere in the vest, a capsule over the chest. Without the first-person body the back item isn't drawn, because nothing would hide it, but it can still be grabbed. Other settings: `HolsterPavlovLayoutEnabled`, `HolsterItemsVisible`, `HolsterInstantDraw`, `HolsterHeightScale`, `HolsterHoverHapticAmplitude` and `HolsterGrabHapticAmplitude`.

Each grip press logs where your hand was relative to the head (`[VR][Holster] grip pressed`), so the layout can be checked against your own reaches.

On servers without the plugin the server's own deploy delay before you can fire still applies; the deploy animation is skipped either way.

Dropping (`HolsterReleaseDrop`, on by default): letting go of an item you took with the grip drops it with your hand's motion, so you can toss a gun to a teammate. Letting go inside or over a holster never drops it. Items you already held before drawing aren't affected. Dropping needs a server with the plugin or a modded host; elsewhere letting go does nothing. A dropped gun lands at your feet, and L4D2 picks a weapon up again when you stand on it with that slot empty; if it comes back to your hand that way (or the host refuses the drop), your grip holds it again, so the next squeeze lets it go.

## Reloading

- **Release:** the Magazine Release button (right A by default) drops detachable magazines. The AK-47's magazine is grabbed and pulled out by hand instead (`MagazineReleaseButtonRequired`). A magazine drops once: while it is out, pressing release again does nothing, even after switching weapons. Its rounds go back to your reserve and a loaded gun keeps the round in its chamber; the fresh magazine then fills from the reserve.
- **Fresh magazine:** put your off hand on the magazine pouch at your off-hand hip (with the Pavlov holster layout; otherwise at `MagazineInteractionFreshMagazinePickupOffsetMeters`), squeeze the grip and insert it into the magazine well. The spare (or shell) sits in your hand the way it sits in the gun: hold your off hand like your gun hand (pointing along the barrel, thumb up) and bring it up a little nose-high under the well, and it lines up (Pavlov VR leans held magazines forward 15 degrees, 5 for pistols, the Uzi and the MAC-10). `ManualReloadMagazineHandRotationOffsetDeg` turns it further in the hand. Only the fresh magazine refills the gun, from your reserve; the stock reload never runs on its own, so an empty gun stays empty until a new magazine is in.
- **Magazine slap** (`MagazineSlapEnabled`, on by default; AK-47, SG552 and military sniper, like Pavlov's AK, Galil and AutoSniper): you can take the spare while the old magazine is still in; the old one glows. Strike it out with the fresh magazine, moving along the magazine and out of the well (roughly downward on a level gun; a forward-and-down AK slap counts) at 0.8 m/s on the AK-47 or 0.9 m/s on the others, measured against the gun hand so walking and turning don't count. The old magazine drops as with any release (one ejection, rounds back to the reserve), and the fresh one can go in 0.15 s later. Letting go of the grip before the slap puts the spare back. On the SG552 and sniper, the release button also drops the old magazine while you hold the spare.
- **Chamber:** if the gun was empty, pull the slide, bolt, charging handle or pump to chamber the first round. Racking only chambers a round from a seated magazine: with the empty magazine still in, or with no magazine, it does nothing. Pump shotguns need a pump after each shot (`ManualPumpEnabled`); shells can be loaded before or after the pump, one shell per insertion. A shot the game showed but the host never fired (the trigger while switching items or right after a draw) needs no pump or rack, even when the gun is drawn again later: the shells come back with the reserve unchanged. While the pump or a reload waits, the gun is held still in its resting pose in your hand (never a frame of the draw or fire animation).
- **Empty guns** never reload or eject on their own; the magazine glows instead (`MagazineEmptyGlowEnabled`).
- **Reload hints:** whatever the next step needs glows softly. That is the magazine when the gun is empty, the spare magazine once the old one is out, and the slide, bolt, charging handle or pump while the gun waits to be cycled. All of these use the `MagazineEmptyGlowEnabled` setting.
- **Released magazines** fall to the floor and disappear after a while (`MagazineDropEnabled`, `MagazineDropLifetimeSeconds`).
- **Pavlov tuning** (`ReloadPavlovTuning`, on by default): each gun uses Pavlov VR's numbers for how far you pull the slide, bolt, charging handle or pump to cock it, where the round chambers as it goes forward (or let go after a full pull and it snaps forward), and how deep a magazine must go in. Examples: the M16 cocks at 7 cm and chambers below 4.9 cm, pistols cock at 7.6 cm, and the AK at 9.5 cm; pulls are capped at 10 cm. The slide model moves only its own travel. The magazine well accepts a magazine within 10 cm and 45 degrees, facing the way it goes in (not backwards or upside down), and the spare magazine can be grabbed with the hand within 20 cm of the pouch (plus the hand's 11.5 cm radius with the Pavlov layout); a hand on the slide, bolt or pump racks it instead. Your per-weapon `MagazineInteraction...Overrides` still win.
- **Pouch glow:** while a fresh magazine is needed, the spare glows; within 35 cm of your empty off hand it glows brighter and you feel a tick.
- **Speed:** on servers with physical transactions (listen host or plugin) ammo is committed when the magazine seats, so you can reload faster than the animation. On other servers the stock reload runs when the fresh magazine seats, so its timer still applies (`PhysicalReloadOnUnsupportedServers`).

## Akimbo

With dual pistols, each trigger fires its own pistol along its own hand. On servers without the plugin each shot's view angles are aimed from your eye at the point that hand's ray hits (`DualPistolsIndependentHandsEnabled`, `DualPistolsAkimboOnAnyServer`).

## Melee and shoving

- Melee weapons lag your hand through a spring tuned per weapon class, so swings move at the regular melee animation speed. The visible weapon, the server sweep and the swing detector all use the same lagged weapon (`MeleeInertiaEnabled`).
- A swing is detected from the blade tip's speed relative to your head, so walking doesn't count; the weapon has to come to rest before the next swing. The chainsaw only needs the trigger.
- Thrusting a melee weapon forward, or pushing a gun forward along its barrel (a gun bash), shoves in that direction (`PhysicalShoveEnabled`). Reaching for a holster, drawing, or flicking the wrist is never a bash.
- The game's own shove and swing animations never play: your hand stays where your controller is, while the shove's sound and hits still happen.

## Grenades

Pull the trigger to arm (pin haptic), release to throw with your hand's motion (`ManualThrowEnabled`, `ManualThrowVelocityScale`). On servers that can't take a custom velocity, the native throw is aimed so it lands where your hand throw would have.

## Pills and adrenaline

Bring pills to your mouth, or stab adrenaline into your chest or your other forearm (`ConsumableGesturesEnabled`). The trigger still works. To give pills or adrenaline to a teammate, hold them against the teammate's chest for a moment (`ConsumableGiveEnabled`). On plugin servers the give is aimed from your eye through your hand; elsewhere the mod aims a normal secondary-attack give at the teammate.

## Two-handed weapons

Like Half-Life 2 VR, grabbing the front of a long gun points it from your rear hand to your front hand (`VrHandsTwoHandedAimStrength=1`), with roll from the rear hand. An adaptive stabilizer (`VrHandsTwoHandedStabilizerEnabled`, a One Euro filter) smooths the aim strongly while you hold still and loosens as you swing, so the front hand stays on the foregrip without the gun trailing behind. Tune it with `VrHandsTwoHandedStabilizerMinCutoffHz` (lower = steadier when still) and `VrHandsTwoHandedStabilizerBeta` (higher = less lag while moving).

## Aiming

- **Crosshair on the sights:** each gun is turned so its bore is parallel to the aim ray; looking down the sights puts the crosshair on the sight picture (`SightAutoAlignEnabled`). The correction is measured automatically per weapon model after the gun settles.
- **Dynamic spread crosshair:** an X (or plus) at the aim point whose gap grows with the weapon's current inaccuracy, like the desktop crosshair (`CrosshairEnabled`, `CrosshairStyle`, `CrosshairArmPixels`, `CrosshairThicknessPixels`, `CrosshairMinGapPixels`, `CrosshairColorRGB`, `CrosshairAlpha`).
- **Picture-in-picture scopes:** the scope image shifts with your eye's offset (parallax) and closes in from the edge outside the eyebox (`ScopePictureInPictureLens`, `ScopeParallaxStrength`, `ScopeEyeboxDeg`, `ScopeVignetteExponent`).

## Presentation

- Loading screens and chapter transitions keep updating in the headset (`LoadingFramePumpEnabled`, `LoadingFramePumpStallMs`).
- End-of-chapter stats and the scoreboard show the HUD overlay automatically (`IntermissionHudEnabled`).
- Health is on the left wrist and ammo on the right (`LeftWristHudEnabled`, `RightAmmoHudEnabled`).
- Other VR players' bodies follow their head and hands, including physical crouching (`WorldModelVRPoseEnabled`).
- Audio keeps playing when the window isn't focused (`BackgroundAudio`).

## Dedicated server plugin

Build `l4d2vr_server.so` (Linux, 32-bit) with `L4D2VR/server/build_release_linux.sh`, or `l4d2vr_server.dll` on Windows. It replaces `l4d2vr_pose_server`; never load both, or poses are relayed twice.

1. Stop the server. Move any old `l4d2vr_pose_server` files out of `left4dead2/addons`.
2. Copy `l4d2vr_server.so` (or `.dll`) and `l4d2vr_server.vdf` into `left4dead2/addons/`. Optionally copy `l4d2vr_server_config.example.txt` there as `l4d2vr_server_config.txt`.
3. Start the server. After the map loads, the log shows `L4D2VR server: map: ... features=0x3FF`. A `target X rejected` line names a feature that a game update broke; only that feature turns off.

Before trusting a new game build, run `l4d2vr_server_symcheck <path to server_srv.so>` (built by `run_server_tests.sh`).

## Controls (physical layout)

Both grips are used only for gripping: they draw from holsters and hold magazines, bolts and foregrips. On Index the grips respond to a firm, full-hand squeeze (squeeze value 0.85 right, 0.8 left). Right A releases the magazine. Push the right stick forward to jump and pull it back to crouch (hold; `StickJumpCrouch`); turning pauses while the stick is pushed mostly forward or back, and clicking the right stick also jumps. Left trigger shoves, or fires the left pistol in akimbo. Oculus Touch, Index (Knuckles) and Vive Cosmos defaults follow this layout.

## Self-test (developers)

`-l4d2vrselftest step,step,...` on the game's command line scripts head and hand poses and button presses, so the online mechanics can be checked with a simulated headset (Monado `SIMULATED_ENABLE=1` with `XRT_COMPOSITOR_NULL=1`, which needs no window). Each check logs `[VR][SelfTest] PASS|FAIL ...` to `vrmod_log.txt`, and the dedicated plugin logs the matching server side. Steps: `features`, `holster`, `togglegrip`, `holsterdrop` (hold-to-grip mode), `give`, `reach` (both hands reach every holster from a human body model at human speeds; checks draws, hover pulses and no gun bash), `emptysqueeze` (squeezing the grip in the air never drops the gun), `pickup` (grip contact pickup of a first aid kit keeps the gun), `bashanim` and `swinganim` (the strike registers and the viewmodel never shows the native shove or swing animation, `[VR][VmAnim]` lines), `throwpipe`, `throwbile`, `swing`, `shove`, `reload`, `slap` (AK-47 magazine slap), `shells`, `pills`, `adrenaline`, `crouch`, `wave`, `weapons`, `wait`, `cmd=<console command>`, `quit`. Steps that give items need `sv_cheats 1` on the server.

Render checks photograph the VR eye images instead of trusting the log:

- `shot=<name>` saves the left eye of the next completed frame (read back on the render thread before it is submitted) as `l4d2vr_selftest_<name>.png` in the game folder. `shot=<name>*N` takes N shots 0.3 s apart (`<name>_1` … `<name>_N`) to compare for flicker; `shotlr=<name>` saves both eyes (`<name>_l`, `<name>_r`).
- `look=<pitch>` holds the head pitch in degrees (positive looks down; `look=off` releases it).
- `righthand=x,y,z` and `lefthand=x,y,z` hold a hand at a point of the holster body frame in metres from the head (x forward, y toward the dominant hand, z up), e.g. `righthand=0.02,0.22,-0.7` is the hip holster; `=off` releases it. These and `look=` hold across the following steps.
- `fire=<seconds>` holds the trigger, `magrelease` presses the magazine release once, `glowmat=<material>` draws every glow with another material (`off` restores it).

For example `cmd=give rifle,cmd=give first_aid_kit,cmd=give pain_pills,cmd=use weapon_rifle,look=0,shot=fwd,look=55,shot=down,righthand=0.02,0.22,-0.7,shot=hover*3,righthand=off,fire=6,shot=empty*3,magrelease,shot=spare*3` photographs the forward view (no back items), the hip and chest holsters, the hover glow and the reload glows. Look at the images. Run the game in `gamescope --backend headless` with `-nosound` to keep it off the desktop. Without the launch option none of this runs.

The `reload`, `slap` and `shells` steps check the server transactions on their own. The hand-driven reload steps go through the whole off-hand path instead: the hand is moved onto the published boxes (bolt or slide, waist pouch, magazine well or loading port) and the grip and trigger are pressed, and each stage logs the distances it measured.

- `mage2e=<give name>` (for example `mage2e=rifle`, `mage2e=pistol`): run the gun dry, rack it with the empty magazine still in and again with the magazine out (neither may refill it), release the magazine, take a spare from the waist, seat it (refilled from the reserve), check that the empty chamber holds fire, rack the bolt or slide the Pavlov distance and let go, fire. If the host has not fired the last round the client predicted when the trigger goes up, the round stays in the gun: the step checks that no rack is demanded and the reserve is unchanged, then pulls the trigger again.
- `shellse2e=<give name>` (`pumpshotgun`, `shotgun_chrome`, `autoshotgun`, `shotgun_spas`): three times fire one, cycle the pump (pump and Chrome), take a shell from the waist and load it; each shell adds exactly one round. The second time the shell goes in before the pump is cycled.
- `magholde2e=<give name>` (any gun with a magazine or shells): takes a spare (on the AK-47 with the old magazine still in) and holds the off hand in a natural insert pose defined from the gun, not from the well: turned like the gun hand (pointing along the barrel, thumb up), raised 10 degrees nose-up, the spare on the well's insertion line 15 cm below it (a shell 26 cm below the port). It logs the spare's angle to the well (PASS within 15 degrees; a shell by its length), saves `<gun>_natural_hold`, then pushes it straight in without turning (the AK-47 slaps the old one out first) and checks that it seats (`<gun>_natural_seated`). The step holds the gun level; add `look=30` before it to see the spare and the gun.
- `slape2e`: AK-47 tactical reload: take a spare with the old magazine seated, slap the old one out, seat the spare, fire. It saves `rifle_ak47_slap_spare_in_hand` and `rifle_ak47_slap_spare_lined_up` screenshots (add `look=35` before it to see the hand and the gun).
- The `magholde2e` step also holds the spare up in view at three hand poses (the insert pose, the palm turned up, raised toward the face) and checks that the drawn fist closes around it: the fist (between the palm and the curled fingers of the drawn hand) must lie within the spare's half thickness of its long axis and in its lower three quarters (a shell anywhere along it) (`<gun>_spare_pose1..3`).
- `pumpredrawe2e=<pumpshotgun|shotgun_chrome>`: fire one, switch to the pistol and back with the pump still waiting. The redrawn gun must sit in the hand with its pump where it was before the switch (within 3 cm, `<gun>_redraw_early`, `<gun>_redraw`), the pump must cycle and the gun must fire.
- `dropreturn`: toggle grips: draw the pump shotgun from the back, hold it close to the body and squeeze to drop it. If the game puts it back in the hand (it lands at the feet), the grip must own it and the next squeeze drops it again.
- `leftang=pitch,yaw,roll` / `rightang=pitch,yaw,roll` turn a scene hand set with `lefthand=`/`righthand=` (`off` releases), e.g. to photograph a gun from the side. `handdump=<seconds>` logs the drawn off hand's bones and fist in the hand bone's frame (`[VR][HandDump]`).
- `sparereturne2e`: AK-47: take a spare with the old magazine seated and let it go. The spare goes back to the pouch, the ammo is unchanged, nothing blocks firing, and the gun fires. Follow it with `mage2e=rifle` to check that switching guns afterwards cannot grant a round.

## Headset test checklist

Test on a listen server you host, on Marmot Cafe with the plugin, and on a server without it.

- Holsters: draw and holster every slot; items visible and glowing on hover; turning keeps them in place.
- Reload every gun type: button release versus AK hand pull, catching, dropped magazines, spare magazine shown only with reserve ammo, chamber only when empty, shotgun shells and pump.
- Sights: the crosshair sits on the front sight for every gun; the crosshair gap grows while moving and firing.
- Akimbo: each trigger hits where its own pistol points.
- Melee: heavy weapons lag a little, hits land where the blade goes, no hits while walking, thrust and gun bash shove.
- Grenades: throw distance follows your arm; arc lands near the target on plain servers.
- Pills to the mouth, adrenaline to the chest and arm.
- Loading screens and chapter transitions stay live; stats show at chapter end.
- Scopes: parallax and edge darkening as your eye moves.
- A second player sees your body, including crouching.

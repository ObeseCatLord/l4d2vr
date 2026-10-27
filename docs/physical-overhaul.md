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

A squeeze that takes nothing is ignored. With `GripToggle=false` you hold the grip to keep an item instead. The two-handed foregrip is also a toggle: squeeze once to take the front of the gun and again to let go (`VrHandsTwoHandedGripHeldMode=false`). Magazines, bolts and slides are held only while you keep squeezing.

| Holster | Contents |
|---|---|
| Behind the dominant shoulder | Primary weapon |
| Behind the other shoulder | Medkit, defibrillator or upgrade pack |
| Dominant hip | Pistol(s) or melee weapon |
| Chest, off-hand side | Grenade |
| Chest, dominant side | Pills or adrenaline |
| Off-hand waist | Spare magazine (or a shell for shotguns) while you have reserve ammo |

The holsters follow your head's yaw past a dead band, and snap turns move them instantly. Settings: `HolsterPavlovLayoutEnabled`, `HolsterItemsVisible`, `HolsterInstantDraw`, `HolsterYawDeadbandDeg`, `HolsterYawSmoothingSeconds`, `HolsterHeightScale`, `Holster*OffsetMeters` (metres from the head: forward, toward the dominant side, up), `Holster*RadiusMeters`, `HolsterHoverHapticAmplitude`, `HolsterGrabHapticAmplitude`. Holstered items are drawn in both eyes whether or not the first-person body is enabled.

On servers without the plugin the server's own deploy delay before you can fire still applies; the deploy animation is skipped either way.

Dropping (`HolsterReleaseDrop`, on by default): letting go of an item you took with the grip drops it with your hand's motion, so you can toss a gun to a teammate. Letting go inside or over a holster never drops it. Items you already held before drawing aren't affected. Dropping needs a server with the plugin or a modded host; elsewhere letting go does nothing.

## Reloading

- **Release:** the Magazine Release button (right A by default) drops detachable magazines. The AK-47's magazine is grabbed and pulled out by hand instead (`MagazineReleaseButtonRequired`).
- **Fresh magazine:** grab the spare at your off-hand waist and insert it into the magazine well.
- **Chamber:** if the gun was empty, pull the slide, bolt, charging handle or pump to chamber the first round. Pump shotguns need a pump after each shot (`ManualPumpEnabled`).
- **Empty guns** never reload or eject on their own; the magazine glows instead (`MagazineEmptyGlowEnabled`).
- **Reload hints:** whatever the next step needs glows softly. That is the magazine when the gun is empty, the spare magazine once the old one is out, and the slide, bolt, charging handle or pump while the gun waits to be cycled. All of these use the `MagazineEmptyGlowEnabled` setting.
- **Released magazines** fall to the floor and disappear after a while (`MagazineDropEnabled`, `MagazineDropLifetimeSeconds`).
- **Speed:** on servers with physical transactions (listen host or plugin) ammo is committed when the magazine seats, so you can reload faster than the animation. On other servers the stock reload runs in step with your hands, so its timer still applies (`PhysicalReloadOnUnsupportedServers`).

## Akimbo

With dual pistols, each trigger fires its own pistol along its own hand. On servers without the plugin each shot's view angles are aimed from your eye at the point that hand's ray hits (`DualPistolsIndependentHandsEnabled`, `DualPistolsAkimboOnAnyServer`).

## Melee and shoving

- Melee weapons lag your hand through a spring tuned per weapon class, so swings move at the regular melee animation speed. The visible weapon, the server sweep and the swing detector all use the same lagged weapon (`MeleeInertiaEnabled`).
- A swing is detected from the blade tip's speed relative to your head, so walking doesn't count; the weapon has to come to rest before the next swing. The chainsaw only needs the trigger.
- Thrusting a melee weapon forward, or bashing with a gun, shoves in that direction (`PhysicalShoveEnabled`).

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

Both grips draw from holsters and hold magazines, bolts and foregrips; right A releases the magazine; the right stick click jumps; left trigger shoves, or fires the left pistol in akimbo. Oculus Touch, Index (Knuckles) and Vive Cosmos defaults follow this layout.

## Self-test (developers)

`-l4d2vrselftest step,step,...` on the game's command line scripts head and hand poses and button presses, so the online mechanics can be checked with a simulated headset (Monado `SIMULATED_ENABLE=1` with `XRT_COMPOSITOR_NULL=1`, which needs no window). Each check logs `[VR][SelfTest] PASS|FAIL ...` to `vrmod_log.txt`, and the dedicated plugin logs the matching server side. Steps: `features`, `holster`, `togglegrip`, `holsterdrop` (hold-to-grip mode), `give`, `throwpipe`, `throwbile`, `swing`, `shove`, `reload`, `shells`, `pills`, `adrenaline`, `crouch`, `wave`, `weapons`, `wait`, `cmd=<console command>`, `quit`. Steps that give items need `sv_cheats 1` on the server. Run the game in `gamescope --backend headless` with `-nosound` to keep it off the desktop. Without the launch option none of this runs.

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

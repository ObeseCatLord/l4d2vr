// 2.0.14 Pavlov handling feel (L4D2VR/vr_pavlov_feel.h).
#include "../L4D2VR/vr_pavlov_feel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace l4d2vr_pavlov_feel;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

static void TestEnvelopeShapes()
{
    // AUG: full strength for 26 ms, off at 29 ms, a 0.37 tick at 224 ms.
    const Envelope aug = EnvelopeFor(Effect::ShotAug);
    CHECK(Near(aug.Eval(0.0f), 1.0f) && Near(aug.Eval(0.020f), 1.0f));
    CHECK(Near(aug.Eval(0.100f), 0.0f));
    CHECK(Near(aug.Eval(0.225f), 0.37f));
    CHECK(Near(aug.Duration(), 0.227f));
    // AK: 1.0 to 36 ms, linear to 0 at 47 ms.
    const Envelope ak = EnvelopeFor(Effect::ShotAk);
    CHECK(Near(ak.Eval(0.030f), 1.0f));
    CHECK(Near(ak.Eval(0.0415f), 0.5f, 0.01f));
    CHECK(Near(ak.Eval(0.237f), 0.62f));
    // AWP: 1.0 to 40 ms, 0.5 at 69 ms, 0.89 at 200 ms, off at 205 ms.
    const Envelope awp = EnvelopeFor(Effect::ShotAwp);
    CHECK(Near(awp.Eval(0.069f), 0.5f) && Near(awp.Eval(0.200f), 0.89f));
    CHECK(Near(awp.Eval(0.206f), 0.0f));
    // Shotgun: 0.53 then echoes.
    const Envelope sg = EnvelopeFor(Effect::ShotShotgun);
    CHECK(Near(sg.Eval(0.01f), 0.53f) && Near(sg.Eval(0.07f), 0.51f) && Near(sg.Eval(0.108f), 0.44f));
    CHECK(Near(sg.Eval(0.208f), 0.17f) && Near(sg.Eval(0.300f), 0.0f));
    // Steps: the later key wins at the step time.
    const Envelope pump = EnvelopeFor(Effect::PumpPulled);
    CHECK(Near(pump.Eval(0.02f), 0.0f) && Near(pump.Eval(0.05f), 0.25f) && Near(pump.Eval(0.11f), 1.0f));
    const Envelope ammo = EnvelopeFor(Effect::AmmoGrab);
    CHECK(Near(ammo.Eval(0.05f), 0.39f) && Near(ammo.Eval(0.15f), 0.20f) && Near(ammo.Eval(0.25f), 0.0f) &&
        Near(ammo.Eval(0.35f), 0.20f));
    CHECK(Near(EnvelopeFor(Effect::GenericTick).Eval(0.022f), 0.03f));
    CHECK(Near(EnvelopeFor(Effect::GenericPulse).Eval(0.15f), 0.08f));
    CHECK(Near(EnvelopeFor(Effect::PumpReleased).Duration(), 0.06f));
    CHECK(Near(EnvelopeFor(Effect::BoltTick).Eval(0.01f), 0.5f));
    CHECK(Near(EnvelopeFor(Effect::MagSlide).Eval(0.016f), 0.17f));
    CHECK(EnvelopeFor(Effect::None).count == 0);
    for (int i = 1; i < static_cast<int>(Effect::Count); ++i)
    {
        const Envelope e = EnvelopeFor(static_cast<Effect>(i));
        CHECK(e.count >= 2 && e.count <= kMaxKeys);
        CHECK(e.Duration() > 0.0f && e.Duration() <= 0.5f);
        for (int k = 1; k < e.count; ++k)
            CHECK(e.keys[static_cast<std::size_t>(k)].t >= e.keys[static_cast<std::size_t>(k - 1)].t);
        CHECK(Near(e.Eval(e.Duration() + 0.01f), 0.0f));
    }
}

static void TestPlayerAt90Hz()
{
    // An AUG shot polled at 90 Hz: full strength first, then silence, then the
    // short 0.37 tick that falls between two frames is still sent (10 ms).
    EnvelopePlayer player;
    player.Start(Effect::ShotAug, 1.0f, 10.0);
    Pulse p{};
    CHECK(player.Poll(10.0, p));
    CHECK(Near(p.amplitude, 1.0f));
    CHECK(Near(p.durationSeconds, 0.029f, 0.002f));
    int pulses = 0;
    bool sawTick = false;
    for (int frame = 1; frame < 40; ++frame)
    {
        if (player.Poll(10.0 + frame / 90.0, p))
        {
            ++pulses;
            if (Near(p.amplitude, 0.37f))
            {
                sawTick = true;
                CHECK(p.durationSeconds >= kMinPulseSeconds);
            }
        }
    }
    CHECK(sawTick);
    CHECK(pulses >= 3 && pulses <= 5);
    CHECK(!player.Active());
    // Scale: the support hand plays at full strength (1.0), a scaled one less.
    player.Start(Effect::ShotAk, 0.5f, 0.0);
    CHECK(player.Poll(0.0, p) && Near(p.amplitude, 0.5f));
    // A new effect replaces the running one.
    player.Start(Effect::BoltTick, 1.0f, 0.001);
    CHECK(player.Current() == Effect::BoltTick);
    CHECK(player.Poll(0.002, p) && Near(p.amplitude, 0.5f));
    CHECK(!player.Poll(0.2, p) && !player.Active());
    // Before the start: nothing.
    player.Start(Effect::GenericTick, 1.0f, 5.0);
    CHECK(!player.Poll(4.9, p));
    // Zero scale never starts.
    player.Start(Effect::GenericTick, 0.0f, 0.0);
    CHECK(!player.Active());
}

static void TestShotMappingAndRate()
{
    CHECK(ShotEffectForWeapon(33) == Effect::ShotAug);   // mp5
    CHECK(ShotEffectForWeapon(9) == Effect::ShotAug);    // scar <- M16
    CHECK(ShotEffectForWeapon(1) == Effect::ShotAk);     // pistol <- M9
    CHECK(ShotEffectForWeapon(26) == Effect::ShotAk);
    CHECK(ShotEffectForWeapon(37) == Effect::ShotAk);    // m60 <- LMGA
    CHECK(ShotEffectForWeapon(35) == Effect::ShotAwp && ShotEffectForWeapon(36) == Effect::ShotAwp);
    for (int id : {3, 4, 8, 11})
        CHECK(ShotEffectForWeapon(id) == Effect::ShotShotgun);
    CHECK(ShotEffectForWeapon(19) == Effect::None && ShotEffectForWeapon(20) == Effect::None);
    // At most once per half the fire interval.
    CHECK(ShotHapticDue(1.0, 0.0, false, 5));
    CHECK(!ShotHapticDue(1.04, 1.0, true, 5));            // m16 0.0875 s: 40 ms is too soon
    CHECK(ShotHapticDue(1.05, 1.0, true, 5));
    CHECK(!ShotHapticDue(1.4, 1.0, true, 3));             // pump 0.875 s
    CHECK(ShotHapticDue(1.44, 1.0, true, 3));
    CHECK(ShotHapticDue(0.5, 1.0, true, 5));              // clock went backwards: allow
    for (int id = 0; id < 40; ++id)
        CHECK(FireIntervalSeconds(id) > 0.0f);
}

static void TestHandleTicks()
{
    HandleTravelTicker ticker;
    float amp = 0.0f;
    CHECK(!ticker.Update(0.1f, 0.5f, 1.0f, amp));        // not held
    ticker.Grab(0.0f);
    CHECK(ticker.Update(0.0f, 0.5f, 1.0f, amp));         // primed: a pulse on the grab
    CHECK(Near(amp, 0.5f));
    CHECK(!ticker.Update(0.005f, 0.5f, 1.0f, amp));      // under one step
    CHECK(ticker.Update(0.011f, 0.5f, 1.0f, amp));       // 0.011 accumulated
    CHECK(Near(amp, 0.5055f));
    // A full M9 cock (input 0.38) in 0.001 steps: about 38 pulses, ramping 0.5 -> 0.69.
    ticker.Grab(0.0f);
    int pulses = 0;
    float last = 0.0f;
    for (int i = 0; i <= 380; ++i)
    {
        if (ticker.Update(static_cast<float>(i) * 0.001f, 0.5f, 1.0f, amp))
        {
            ++pulses;
            CHECK(amp >= last - 1e-6f);
            last = amp;
        }
    }
    CHECK(pulses >= 34 && pulses <= 40);
    CHECK(Near(last, 0.69f, 0.01f));
    // Moving back and forth also ticks (Pavlov accumulates |delta|).
    ticker.Grab(0.5f);
    CHECK(ticker.Update(0.5f, 0.05f, 0.7f, amp));
    CHECK(ticker.Update(0.48f, 0.05f, 0.7f, amp));
    // Past the end of travel: no pulse.
    CHECK(!ticker.Update(1.2f, 0.05f, 0.7f, amp));
    ticker.Release();
    CHECK(!ticker.Update(0.3f, 0.05f, 0.7f, amp));
}

static void TestHandleSounds()
{
    HandleSoundTracker s;
    s.Grab();
    HandleSoundEvents e = s.Update(0.05f, 0.38f);
    CHECK(!e.grab && !e.lock && !e.release);
    e = s.Update(0.11f, 0.38f);
    CHECK(e.grab && !e.lock);
    e = s.Update(0.2f, 0.38f);
    CHECK(!e.grab);                                       // once per grab
    e = s.Update(0.38f, 0.38f);
    CHECK(e.lock);
    e = s.Update(0.25f, 0.38f);
    CHECK(!e.release);
    e = s.Update(0.18f, 0.38f);
    CHECK(e.release);                                     // below half the cock point
    CHECK(!s.Ungrab());                                   // let go at 0.18: no ungrab sound
    s.Grab();
    s.Update(0.3f, 0.38f);
    CHECK(s.Ungrab());                                    // let go past 0.25
    CHECK(!s.Ungrab());                                   // only once
}

static void TestWallPulse()
{
    WallPulseTimer t;
    int pulses = 0;
    float firstAt = -1.0f;
    for (int frame = 1; frame <= 180; ++frame)            // 2 s at 90 Hz
    {
        if (t.Update(true, 1.0f / 90.0f))
        {
            if (pulses == 0)
                firstAt = static_cast<float>(frame) / 90.0f;
            ++pulses;
        }
    }
    CHECK(Near(firstAt, 0.5f, 0.02f));
    CHECK(pulses == 2);                                   // 0.5 s and 1.25 s
    CHECK(!t.Update(false, 0.1f));
    CHECK(!t.Update(true, 0.4f));                         // restarts from zero
    CHECK(t.Update(true, 0.11f));

    // Blocked when either stretch hits something or starts inside a solid.
    CHECK(!MuzzleInWall(1.0f, false, 1.0f, false));
    CHECK(MuzzleInWall(0.5f, false, 1.0f, false));
    CHECK(MuzzleInWall(1.0f, false, 0.7f, false));
    CHECK(MuzzleInWall(1.0f, false, 1.0f, true));
    CHECK(MuzzleInWall(1.0f, true, 1.0f, false));
    CHECK(MuzzleReachMeters(35) > MuzzleReachMeters(1));
    CHECK(MuzzleReachMeters(19) == 0.0f && MuzzleReachMeters(13) == 0.0f);   // melee, molotov
}

static void TestReleaseEase()
{
    ReleaseEase ease;
    CHECK(!ease.Begin(V3{ 1.0f, 0.0f, 0.0f }));             // nothing to ease
    CHECK(ease.Begin(V3{ 0.9f, 0.3f, 0.1f }));
    // VInterpTo at speed 5 from a ~19 degree offset: most of it gone in 0.2 s,
    // done within a second at 90 Hz, monotonic.
    float prevX = ease.Direction().x;
    int frames = 0;
    float at02 = 0.0f;
    while (ease.Active() && frames < 200)
    {
        const V3 d = ease.Step(1.0f / 90.0f);
        CHECK(Near(Length(d), 1.0f));
        CHECK(d.x >= prevX - 1e-6f);
        prevX = d.x;
        ++frames;
        if (frames == 18)
            at02 = std::acos(std::fmin(1.0f, d.x)) * 57.29578f;
    }
    CHECK(!ease.Active());
    CHECK(frames > 20 && frames < 120);
    CHECK(at02 > 3.0f && at02 < 12.0f);
    // VInterpTo itself.
    const V3 v = VInterpTo(V3{ 0.0f, 0.0f, 0.0f }, V3{ 1.0f, 0.0f, 0.0f }, 0.1f, 5.0f);
    CHECK(Near(v.x, 0.5f));
    CHECK(Near(VInterpTo(V3{}, V3{ 2.0f, 0.0f, 0.0f }, 1.0f, 5.0f).x, 2.0f));
    CHECK(Near(VInterpTo(V3{}, V3{ 2.0f, 0.0f, 0.0f }, 1.0f, 0.0f).x, 2.0f));
    ease.Begin(V3{ 0.0f, 1.0f, 0.0f });
    ease.Cancel();
    CHECK(!ease.Active());
    const V3 rest = ease.Step(0.01f);
    CHECK(Near(rest.x, 1.0f));
}

static void TestMagBoltRelease()
{
    CHECK(MagBoltRelease(false, true, true, 30, true) == MagButtonAction::None);
    CHECK(MagBoltRelease(true, true, true, 30, true) == MagButtonAction::ReleaseBolt);
    CHECK(MagBoltRelease(true, true, true, 0, true) == MagButtonAction::EjectMagazine);    // empty magazine
    CHECK(MagBoltRelease(true, true, false, 30, true) == MagButtonAction::EjectMagazine);  // no magazine seated
    CHECK(MagBoltRelease(true, false, true, 30, true) == MagButtonAction::EjectMagazine);  // slide forward
    CHECK(MagBoltRelease(true, false, true, 30, false) == MagButtonAction::None);          // no detachable magazine
    CHECK(MagBoltRelease(true, true, true, 5, false) == MagButtonAction::ReleaseBolt);

    CHECK(HkRackRelease(false, false) == HkRackResult::Nothing);
    CHECK(HkRackRelease(true, false) == HkRackResult::LockedOpen);
    CHECK(HkRackRelease(true, true) == HkRackResult::Chambered);
}

static void TestMagazineSlideCue()
{
    MagazineSlideCue cue;
    MagazineSlideEvents e = cue.Update(0.30f, 0.10f);
    CHECK(!e.start && !e.half);
    e = cue.Update(0.09f, 0.10f);
    CHECK(e.start && !e.half);
    e = cue.Update(0.08f, 0.10f);
    CHECK(!e.start && !e.half);
    e = cue.Update(0.04f, 0.10f);
    CHECK(!e.start && e.half);
    e = cue.Update(0.11f, 0.10f);                         // inside the 20 % margin: no re-arm
    CHECK(!e.start && !e.half);
    e = cue.Update(0.02f, 0.10f);
    CHECK(!e.start && !e.half);
    e = cue.Update(0.13f, 0.10f);                         // left: re-arms
    e = cue.Update(0.03f, 0.10f);                         // straight in: start and half at once
    CHECK(e.start && e.half);
    cue.Reset();
    CHECK(!cue.Update(0.5f, 0.0f).start);
}

static void TestSoundsAndLockedGrab()
{
    // Taken locked open (at the cock point): no grab or lock sound, but the
    // way back below half the cock point is the release.
    HandleSoundTracker s;
    s.Grab(0.38f, 0.38f);
    HandleSoundEvents e = s.Update(0.37f, 0.38f);
    CHECK(!e.grab && !e.lock && !e.release);
    e = s.Update(0.1f, 0.38f);
    CHECK(e.release);
    for (int id : {1, 32, 2, 7, 33, 5, 26, 9, 34, 6, 10, 37})
        CHECK(HandleLockSound(id) != nullptr);
    for (int id : {3, 4, 8, 11, 35, 36, 19})
        CHECK(HandleLockSound(id) == nullptr);
    CHECK(LightForegripFoley(1) && LightForegripFoley(33) && !LightForegripFoley(26) && !LightForegripFoley(3));
    CHECK(ForegripGrabSound(1) != ForegripGrabSound(26));
    CHECK(ForegripGrabVolume(1) < ForegripGrabVolume(26));
}

int main()
{
    TestEnvelopeShapes();
    TestPlayerAt90Hz();
    TestShotMappingAndRate();
    TestHandleTicks();
    TestHandleSounds();
    TestWallPulse();
    TestReleaseEase();
    TestMagBoltRelease();
    TestMagazineSlideCue();
    TestSoundsAndLockedGrab();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_pavlov_feel tests passed\n");
    return EXIT_SUCCESS;
}

#include "../L4D2VR/vr_hand_throw.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace l4d2vr_hand_throw;

static int checks = 0;
static void Require(bool ok, const char* what)
{
    ++checks;
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

static void WireRoundTrip()
{
    for (float move : { 0.0f, 450.0f, -450.0f, 37.4f, -0.6f, 511.0f, -511.0f, 900.0f })
        for (float v : { 0.0f, 1.0f, -1.0f, 812.3f, -2047.0f, 2047.0f, 5000.0f })
        {
            const float packed = PackMove(move, v);
            float m = 0.0f, u = 0.0f;
            Require(UnpackMove(packed, m, u), "packed movement decodes");
            Require(m == QuantizedMove(move), "movement survives as whole units");
            Require(Near(u, std::clamp(std::round(v), -2047.0f, 2047.0f), 0.0f), "velocity survives to 1 unit/s");
        }
    float m = 0.0f, u = 0.0f;
    Require(!UnpackMove(450.0f, m, u) && m == 0.0f, "ordinary movement is not a payload");
    Require(!UnpackMove(std::nanf(""), m, u), "NaN is rejected");
    Require(!UnpackMove(4096.5f, m, u), "fractions are rejected");
    Require(!UnpackMove(-5000.0f, m, u), "negative values are rejected");

    for (Kind kind : { Kind::Molotov, Kind::PipeBomb, Kind::VomitJar })
        for (bool left : { false, true })
            for (float vz : { 0.0f, -1023.0f, 1023.0f, 333.3f, -4000.0f })
            {
                int ws = 0, wst = 0;
                PackVertical(vz, kind, left, ws, wst);
                Require(ws >= 1 && ws <= 2047, "weaponselect fits MAX_EDICT_BITS and is never 0");
                Require(wst >= 0 && wst <= 63, "weaponsubtype fits 6 bits");
                float z = 0.0f; Kind k = Kind::None; bool l = false;
                Require(UnpackVertical(ws, wst, z, k, l) && k == kind && l == left, "kind and hand decode");
                Require(z == std::clamp(std::round(vz), -1023.0f, 1023.0f), "vertical velocity decodes");
            }
    float z = 0.0f; Kind k = Kind::None; bool l = false;
    Require(!UnpackVertical(0, 0x21, z, k, l), "weaponselect 0 is not a payload");
    Require(!UnpackVertical(100, 0x01, z, k, l), "subtype without marker is rejected");
    Require(!UnpackVertical(100, 0x20, z, k, l), "subtype without a grenade kind is rejected");
    Require(!UnpackVertical(100, 0x28, z, k, l), "reserved subtype bits are rejected");

    // Full field decode.
    float fwd = PackMove(300.0f, 120.0f), side = PackMove(-150.0f, -45.0f);
    int ws = 0, wst = 0;
    PackVertical(260.0f, Kind::PipeBomb, false, ws, wst);
    const Payload p = DecodeFields(fwd, side, ws, wst);
    Require(p.valid && p.kind == Kind::PipeBomb && !p.leftHand, "payload decodes");
    Require(fwd == 300.0f && side == -150.0f && ws == 0 && wst == 0, "the game sees the real movement and no weapon select");
    Require(p.velocity.x == 120.0f && p.velocity.y == -45.0f && p.velocity.z == 260.0f, "velocity decodes");
    fwd = 200.0f; side = 0.0f; ws = 5; wst = 0x22;
    const Payload bad = DecodeFields(fwd, side, ws, wst);
    Require(!bad.valid && fwd == 0.0f && side == 0.0f && ws == 0 && wst == 0,
        "a malformed payload never reaches the game as movement or weapon selection");
    Require(KindFromWeaponId(13) == Kind::Molotov && KindFromWeaponId(14) == Kind::PipeBomb &&
        KindFromWeaponId(25) == Kind::VomitJar && KindFromWeaponId(1) == Kind::None, "weapon ids");
    Require(WeaponIdFromKind(Kind::VomitJar) == 25 && WeaponIdFromKind(Kind::None) == 0, "kinds");
}

static void Compose()
{
    const V3 v = ComposeVelocity(V3(100.0f, 0.0f, 50.0f), V3(200.0f, 0.0f, 0.0f), 3.0f, 1400.0f);
    Require(Near(v.x, 500.0f, 0.01f) && Near(v.z, 150.0f, 0.01f), "hand motion is scaled, the body's is not");
    const V3 c = ComposeVelocity(V3(1000.0f, 0.0f, 0.0f), V3(), 3.0f, 1400.0f);
    Require(Near(c.Length(), 1400.0f, 0.1f), "the server clamp holds");
    const V3 d = ComposeVelocity(V3(), V3(0.0f, 0.0f, -20.0f), 3.0f, 1400.0f);
    Require(Near(d.z, -20.0f, 0.01f), "a drop with no hand motion falls with the body");
    Require(ComposeVelocity(V3(std::nanf(""), 0.0f, 0.0f), V3(), 3.0f, 1400.0f).Length() == 0.0f, "NaN is rejected");
    Require(ComposeVelocity(V3(10.0f, 0.0f, 0.0f), V3(), 3.0f, 0.0f).Length() == 0.0f, "max 0 disables");

    // The default boost (2.0.4: 2.5 on the hand's peak): a 5 m/s overhand
    // throw (39.37 units/m) leaves at 12.5 m/s along the swing; a slow
    // 0.3 m/s release only drops.
    Require(kDefaultThrowBoost >= 2.0f && kDefaultThrowBoost <= 3.0f, "Pavlov-like boost");
    const float upm = 39.3701f;
    const V3 overhand = ComposeVelocity(V3(5.0f * upm * 0.8f, 0.0f, 5.0f * upm * 0.6f), V3(), kDefaultThrowBoost, 1400.0f);
    Require(Near(overhand.Length(), 5.0f * kDefaultThrowBoost * upm, 0.5f), "5 m/s hand -> 12.5 m/s grenade");
    Require(Near(overhand.z / overhand.x, 0.75f, 0.001f), "direction is the hand's");
    const V3 slow = ComposeVelocity(V3(0.3f * upm, 0.0f, 0.0f), V3(), kDefaultThrowBoost, 1400.0f);
    Require(slow.Length() < 1.0f * upm, "a slow release is a gentle drop");

    std::array<float, 4> t{ 0.0f, 1.0f / 30.0f, 2.0f / 30.0f, 3.0f / 30.0f };
    std::array<V3, 4> o{ V3(0, 0, 0), V3(7, 0, 0), V3(14, 0, 0), V3(21, 0, 0) };
    V3 pv{};
    Require(OriginVelocity(t, o, 4, pv) && Near(pv.x, 210.0f, 0.5f), "origin velocity");
    // Two commands decoded in one packet share an origin: the fit still recovers the speed.
    o = { V3(0, 0, 0), V3(0, 0, 0), V3(14, 0, 0), V3(14, 0, 0) };
    Require(OriginVelocity(t, o, 4, pv) && pv.x > 150.0f && pv.x < 260.0f, "batched origins");
    Require(!OriginVelocity(t, o, 1, pv), "one sample is not a velocity");
}

static void Tracker()
{
    VelocityTracker tr;
    V3 v{};
    Require(!tr.Estimate(0.0, 0.08f, 0.5f, v), "no samples");
    // 90 Hz, hand moving 6 m/s forward and 2 m/s up.
    double t = 10.0;
    for (int i = 0; i < 30; ++i, t += 1.0 / 90.0)
        tr.Add(t, V3(6.0f * static_cast<float>(t - 10.0), 0.0f, 2.0f * static_cast<float>(t - 10.0)));
    const double now = t - 1.0 / 90.0;
    Require(tr.Estimate(now, 0.08f, 0.5f, v) && Near(v.x, 6.0f, 0.01f) && Near(v.z, 2.0f, 0.01f), "constant velocity");

    // Accelerate then slow down before release: the peak blend keeps the throw strong.
    VelocityTracker swing;
    double ts = 0.0;
    float x = 0.0f;
    for (int i = 0; i < 36; ++i, ts += 1.0 / 90.0)
    {
        const float speed = i < 30 ? 0.3f * static_cast<float>(i) : std::max(0.0f, 9.0f - 1.5f * static_cast<float>(i - 30));
        x += speed / 90.0f;
        swing.Add(ts, V3(x, 0.0f, 0.0f));
    }
    const double release = ts - 1.0 / 90.0;
    V3 plain{}, blended{};
    Require(swing.Estimate(release, 0.08f, 0.0f, plain) && swing.Estimate(release, 0.08f, 0.5f, blended),
        "swing estimates");
    Require(blended.x > plain.x && blended.x > 6.0f, "peak blend favours the fast part of the window");

    // Holding still: a drop.
    VelocityTracker still;
    for (int i = 0; i < 20; ++i)
        still.Add(1.0 + i / 90.0, V3(0.3f, 0.2f, -0.3f));
    Require(still.Estimate(1.0 + 19.0 / 90.0, 0.08f, 0.5f, v) && v.Length() < 1e-3f, "a still hand gives no velocity");

    // Repeated or older frames are ignored; a tracking gap restarts.
    VelocityTracker gap;
    gap.Add(1.0, V3(0, 0, 0));
    gap.Add(1.0, V3(5, 0, 0));
    Require(gap.Count() == 1, "same timestamp ignored");
    gap.Add(2.0, V3(10, 0, 0));
    Require(gap.Count() == 1, "a gap restarts the history");
    gap.Add(2.0 + 1.0 / 90.0, V3(10.05f, 0, 0));
    Require(gap.Estimate(2.0 + 1.0 / 90.0, 0.08f, 0.0f, v) && Near(v.x, 4.5f, 0.05f), "two frames after a gap");

    // Window clamp and samples in the future of `now` are not used.
    Require(tr.Estimate(now - 0.1, 0.08f, 0.0f, v) && Near(v.x, 6.0f, 0.01f), "estimate in the past");

    const V3 w = TrackingToWorld(V3(1.0f, 0.0f, 1.0f), 90.0f, 43.2f);
    Require(Near(w.x, 0.0f, 0.01f) && Near(w.y, 43.2f, 0.01f) && Near(w.z, 43.2f, 0.01f), "tracking to world");
}

// Recorded-style hand motion (90 Hz, tracking space, metres; Source axes x
// forward, z up) with tracking jitter: the hand winds up, accelerates through
// the swing, peaks and decelerates; the trigger is let go near the peak and
// noticed a frame or two later. Returns the release time.
struct SwingSpec
{
    float peakSpeed;        // m/s at the fastest point
    float upFraction;       // share of the swing's direction that is up (overhand 0.5, underhand 0.7)
    float accelSeconds;     // wind-up to peak
    float releaseAfterPeak; // trigger let go this long after the peak (+ detection lag)
};
static double RecordSwing(VelocityTracker& tr, const SwingSpec& s, V3& peakDirection)
{
    const double dt = 1.0 / 90.0;
    const float fz = s.upFraction, fx = std::sqrt(1.0f - fz * fz);
    peakDirection = V3(fx, 0.0f, fz);
    V3 p(-0.25f, 0.20f, -0.10f);
    double t = 100.0;
    unsigned seed = 12345u;
    auto jitter = [&]() { seed = seed * 1103515245u + 12345u; return (static_cast<float>((seed >> 16) & 0x7FFF) / 32767.0f - 0.5f) * 0.0016f; };
    // A still moment, then the swing (smooth-step speed up to the peak), then a
    // fast stop (the arm's follow-through brakes in ~0.12 s).
    const int still = 20, up = static_cast<int>(s.accelSeconds * 90.0f), down = 14;
    double release = 0.0;
    for (int i = 0; i < still + up + down; ++i, t += dt)
    {
        float speed = 0.0f;
        if (i >= still && i < still + up)
        {
            const float u = static_cast<float>(i - still + 1) / static_cast<float>(up);
            speed = s.peakSpeed * u * u * (3.0f - 2.0f * u);
        }
        else if (i >= still + up)
        {
            const float u = static_cast<float>(i - still - up + 1) / static_cast<float>(down);
            speed = s.peakSpeed * std::max(0.0f, 1.0f - u);
        }
        // The swing arcs: the direction turns 20 degrees through it.
        const float bend = (i >= still) ? 0.35f * (static_cast<float>(i - still) / static_cast<float>(up + down) - 0.5f) : 0.0f;
        const V3 dir(fx * std::cos(bend) - fz * std::sin(bend), 0.0f, fx * std::sin(bend) + fz * std::cos(bend));
        p = p + dir * (speed * static_cast<float>(dt));
        tr.Add(t, V3(p.x + jitter(), p.y + jitter(), p.z + jitter()));
        if (i == still + up - 1)
            release = t + s.releaseAfterPeak;
    }
    return release;
}

static float LaunchUnitsPerSecond(const V3& handMps)
{
    // What the server spawns: hand velocity in world units x the boost.
    const float upm = 43.2f; // the plugin's UnitsPerMeter
    return ComposeVelocity(handMps * upm, V3(), kDefaultThrowBoost, 1400.0f).Length();
}

static void RecordedThrows()
{
    // A natural overhand throw: 5.5 m/s at the peak, let go 45 ms after it
    // (the hand is at ~3 m/s by then).
    {
        VelocityTracker tr;
        V3 dir{};
        const double release = RecordSwing(tr, { 5.5f, 0.5f, 0.22f, 0.045f }, dir);
        V3 v{}, avg{};
        Require(tr.EstimateRelease(release, 0.08f, v), "overhand release estimate");
        Require(tr.Estimate(release, 0.08f, 0.5f, avg), "old estimate");
        const float launch = LaunchUnitsPerSecond(v);
        std::printf("overhand: peak 5.5 m/s -> estimate %.2f m/s (old blend %.2f m/s x1.5 = %.0f u/s), launch %.0f u/s\n",
            v.Length(), avg.Length(), avg.Length() * 43.2f * 1.5f, launch);
        Require(v.Length() > 4.8f && v.Length() < 6.2f, "the release speed is the hand's peak");
        Require(launch > 500.0f && launch < 750.0f, "a natural overhand leaves at about the game's own throw speed");
        Require(avg.Length() * 43.2f * 1.5f < 0.6f * launch, "the old half-blend x1.5 lobbed it (the reported bug)");
        const float cosAngle = (v.x * dir.x + v.y * dir.y + v.z * dir.z) / v.Length();
        Require(cosAngle > std::cos(15.0f * 3.14159265f / 180.0f), "it flies along the swing");
    }
    // Let go late (90 ms after the peak, the hand nearly stopped): still a throw.
    {
        VelocityTracker tr;
        V3 dir{};
        const double release = RecordSwing(tr, { 5.5f, 0.5f, 0.22f, 0.09f }, dir);
        V3 v{};
        Require(tr.EstimateRelease(release, 0.08f, v), "late release estimate");
        Require(LaunchUnitsPerSecond(v) > 400.0f, "a late release still throws");
    }
    // An underhand toss: 3 m/s, mostly up.
    {
        VelocityTracker tr;
        V3 dir{};
        const double release = RecordSwing(tr, { 3.0f, 0.7f, 0.30f, 0.03f }, dir);
        V3 v{};
        Require(tr.EstimateRelease(release, 0.08f, v), "underhand estimate");
        const float launch = LaunchUnitsPerSecond(v);
        Require(launch > 250.0f && launch < 400.0f, "an underhand toss lobs a few metres");
        Require(v.z > 0.5f * v.Length(), "upward");
    }
    // A deliberate slow release: the hand drifts at 0.4 m/s and lets go.
    {
        VelocityTracker tr;
        V3 dir{};
        const double release = RecordSwing(tr, { 0.4f, 0.0f, 0.5f, 0.0f }, dir);
        V3 v{};
        Require(tr.EstimateRelease(release, 0.08f, v), "slow release estimate");
        Require(LaunchUnitsPerSecond(v) < 60.0f, "a slow release only drops it");
    }
    // Holding still: nothing.
    {
        VelocityTracker still;
        for (int i = 0; i < 20; ++i)
            still.Add(1.0 + i / 90.0, V3(0.3f, 0.2f, -0.3f));
        V3 v{};
        Require(still.EstimateRelease(1.0 + 19.0 / 90.0, 0.08f, v) && v.Length() < 1e-3f, "a still hand gives no velocity");
        VelocityTracker none;
        Require(!none.EstimateRelease(0.0, 0.08f, v), "no samples");
    }
}

static void Fsm()
{
    ReleaseInput in{};
    in.throwable = true;
    in.weapon = 0x100u;
    in.gripOwned = true;
    ReleaseFsm f;
    in.nowMs = 1000u;
    Require(f.Update(in) == Action::None, "first frame binds the weapon");

    // Hold the trigger (pin), swing, let go of the trigger: it throws however
    // short the hold was (a slow release lobs or drops it by its own velocity).
    in.triggerDown = true; in.nowMs = 1100u;
    Require(f.Update(in) == Action::PinPulled && f.Armed(), "trigger pulls the pin");
    in.nowMs = 1130u;
    Require(f.Update(in) == Action::None && f.Armed(), "held trigger stays armed");
    in.triggerDown = false; in.nowMs = 1150u;
    Require(f.Update(in) == Action::Throw && f.Thrown() && !f.Armed(), "a quick release throws too (no minimum hold)");
    in.gripReleased = true; in.nowMs = 1600u;
    Require(f.Update(in) == Action::None, "one throw per grenade: the grip release after it does nothing");
    in.gripReleased = false; in.nowMs = 1700u;
    Require(f.Update(in) == Action::None, "one throw per grenade");

    // Letting go of the grip while the trigger is still held throws.
    ReleaseFsm gt; ReleaseInput gi = in; gi.weapon = 0x150u; gi.triggerDown = false; gi.gripReleased = false; gi.nowMs = 1800u;
    gt.Update(gi);
    gi.triggerDown = true; gi.nowMs = 1810u;
    Require(gt.Update(gi) == Action::PinPulled, "press");
    gi.gripReleased = true; gi.nowMs = 2100u;
    Require(gt.Update(gi) == Action::Throw, "grip let go with the trigger held: thrown");

    // A grip release without the pin is the ordinary drop (caller keeps it).
    ReleaseFsm g; ReleaseInput d = in; d.weapon = 0x200u; d.nowMs = 2000u;
    g.Update(d);
    d.gripReleased = true;
    Require(g.Update(d) == Action::None && !g.Thrown(), "no pin: the release drops the item instead");
    Require(!g.GripReleaseThrows(true) && g.GripReleaseThrows(false), "pin requirement is configurable");
    ReleaseFsm h; ReleaseInput np = d; np.weapon = 0x300u; np.pinRequired = false; np.gripReleased = false;
    h.Update(np);
    np.gripReleased = true;
    Require(h.Update(np) == Action::Throw, "without the pin requirement a grip release throws");

    // Hold the trigger through the swing and let go of it.
    ReleaseFsm k; ReleaseInput hold = in; hold.weapon = 0x400u; hold.gripReleased = false; hold.nowMs = 3000u;
    hold.triggerDown = false;
    k.Update(hold);
    hold.triggerDown = true; hold.nowMs = 3010u;
    Require(k.Update(hold) == Action::PinPulled, "press");
    hold.triggerDown = false; hold.nowMs = 3500u;
    Require(k.Update(hold) == Action::Throw, "a held trigger released after the swing throws");

    // The grip does not hold the item: trigger press/release throws at once.
    ReleaseFsm m; ReleaseInput sel = hold; sel.weapon = 0x500u; sel.gripOwned = false; sel.nowMs = 4000u;
    m.Update(sel);
    sel.triggerDown = true; sel.nowMs = 4010u; m.Update(sel);
    sel.triggerDown = false; sel.nowMs = 4050u;
    Require(m.Update(sel) == Action::Throw, "selected grenade: release the trigger to throw");

    // A trigger held while switching to the grenade does not pull the pin.
    ReleaseFsm n; ReleaseInput sw = sel; sw.weapon = 0x600u; sw.triggerDown = true; sw.nowMs = 5000u;
    Require(n.Update(sw) == Action::None, "switch frame");
    sw.nowMs = 5100u;
    Require(n.Update(sw) == Action::None && !n.Armed(), "a held trigger needs a new press");

    // Weapon change disarms; blocked input does nothing.
    ReleaseFsm q; ReleaseInput b = in; b.weapon = 0x700u; b.gripReleased = false; b.triggerDown = false; b.nowMs = 6000u;
    q.Update(b);
    b.triggerDown = true; b.nowMs = 6010u; q.Update(b);
    Require(q.Armed(), "armed");
    b.weapon = 0x800u; b.nowMs = 6020u; q.Update(b);
    Require(!q.Armed(), "switching away disarms");
    ReleaseFsm r; ReleaseInput bl = b; bl.weapon = 0x900u; bl.triggerDown = false; r.Update(bl);
    bl.blocked = true; bl.triggerDown = true; bl.nowMs = 7000u;
    Require(r.Update(bl) == Action::None && !r.Armed(), "blocked input cannot arm");
    // Armed, then blocked (a menu): releasing there does not throw and disarms.
    ReleaseFsm u; ReleaseInput ub = b; ub.weapon = 0x950u; ub.triggerDown = false; ub.blocked = false; ub.nowMs = 7100u;
    u.Update(ub);
    ub.triggerDown = true; ub.nowMs = 7110u;
    Require(u.Update(ub) == Action::PinPulled, "armed");
    ub.blocked = true; ub.nowMs = 7120u;
    Require(u.Update(ub) == Action::None && u.Armed(), "a blocked frame keeps the pin pulled");
    ub.triggerDown = false; ub.nowMs = 7130u;
    Require(u.Update(ub) == Action::None && !u.Armed(), "a release while blocked never throws");
    // Every one of ten deliberate hold-swing-release throws releases, whatever the hold time.
    for (int i = 0; i < 10; ++i)
    {
        ReleaseFsm t; ReleaseInput ti = in; ti.weapon = 0xB00u + static_cast<std::uintptr_t>(i);
        ti.gripReleased = false; ti.triggerDown = false; ti.nowMs = 9000u;
        t.Update(ti);
        ti.triggerDown = true; ti.nowMs = 9016u;
        Require(t.Update(ti) == Action::PinPulled, "pin");
        ti.triggerDown = false; ti.nowMs = 9016u + 16u * static_cast<std::uint64_t>(i + 1);
        Require(t.Update(ti) == Action::Throw, "release throws");
    }

    // The server did not take the throw (e.g. stale): after a while it can be thrown again.
    ReleaseFsm s; ReleaseInput st = in; st.weapon = 0xA00u; st.gripReleased = false; st.triggerDown = false; st.nowMs = 8000u;
    s.Update(st);
    st.triggerDown = true; st.nowMs = 8010u; s.Update(st);
    st.triggerDown = false; st.gripReleased = true; st.nowMs = 8100u;
    Require(s.Update(st) == Action::Throw, "throw");
    st.gripReleased = false; st.nowMs = 8100u + ReleaseFsm::kThrownResetMs + 10u;
    s.Update(st);
    Require(!s.Thrown() && !s.Armed(), "an unanswered throw resets");
}

int main()
{
    WireRoundTrip();
    Compose();
    Tracker();
    RecordedThrows();
    Fsm();
    std::printf("vr_hand_throw_tests: %d checks passed\n", checks);
    return 0;
}

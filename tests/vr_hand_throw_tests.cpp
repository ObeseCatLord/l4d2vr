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

static void Fsm()
{
    ReleaseInput in{};
    in.throwable = true;
    in.weapon = 0x100u;
    in.gripOwned = true;
    ReleaseFsm f;
    in.nowMs = 1000u;
    Require(f.Update(in) == Action::None, "first frame binds the weapon");

    // Pavlov: tap the trigger (pin), swing, let go of the grip.
    in.triggerDown = true; in.nowMs = 1100u;
    Require(f.Update(in) == Action::PinPulled && f.Armed(), "trigger pulls the pin");
    in.triggerDown = false; in.nowMs = 1150u;
    Require(f.Update(in) == Action::None, "a quick tap only pulls the pin while the grip holds the grenade");
    in.gripReleased = true; in.nowMs = 1600u;
    Require(f.Update(in) == Action::Throw && f.Thrown(), "letting go of the grip throws");
    in.gripReleased = false; in.nowMs = 1700u;
    Require(f.Update(in) == Action::None, "one throw per grenade");

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

    // Hold the trigger through the swing and let go of it (old habit).
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
    Fsm();
    std::printf("vr_hand_throw_tests: %d checks passed\n", checks);
    return 0;
}

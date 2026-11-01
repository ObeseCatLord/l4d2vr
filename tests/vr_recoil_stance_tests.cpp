#include "../L4D2VR/vr_recoil_stance.h"
#include "../L4D2VR/vr_recoil.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

// Stance recoil: vanilla L4D2's crouch / air / movement spread per gun, as a
// recoil multiplier, and the recoil model's use of it.
static void CheckImpl(bool result, int line, const char* what)
{
    if (!result) { std::fprintf(stderr, "Check failed at line %d: %s\n", line, what); std::abort(); }
}
#define Check(result) CheckImpl((result), __LINE__, #result)

using namespace l4d2vr_recoil_stance;

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static const int kGuns[] = { 1, 32, 2, 7, 33, 5, 26, 9, 34, 37, 6, 10, 35, 36, 3, 8, 4, 11, 21 };

static PlayerState Ground(float speed, bool crouched = false) { PlayerState p; p.onGround = true; p.crouched = crouched; p.horizontalSpeed = speed; return p; }
static PlayerState Air(float speed = 0.0f) { PlayerState p; p.onGround = false; p.horizontalSpeed = speed; return p; }

static float PeakPitch(int weaponId, bool twoHanded, int shots, float stanceMul, float* heat = nullptr)
{
    const l4d2vr_recoil::Profile* profile = l4d2vr_recoil::ProfileForWeapon(weaponId);
    l4d2vr_recoil::State s;
    l4d2vr_recoil::Inputs in{};
    in.twoHanded = twoHanded;
    in.stanceMul = stanceMul;
    s.Reset(profile, in);
    float peak = 0.0f, maxHeat = 0.0f;
    float sinceShot = 1.0e9f;
    int fired = 0;
    for (int tick = 0; tick < 400; ++tick)
    {
        if (fired < shots && sinceShot >= profile->fireRate)
        {
            s.Fire();
            ++fired;
            sinceShot = 0.0f;
        }
        in.triggerHeld = fired < shots;
        const l4d2vr_recoil::Offset o = s.Tick(1.0f / 90.0f, in);
        sinceShot += 1.0f / 90.0f;
        peak = std::fmax(peak, std::fabs(o.pitchDeg));
        maxHeat = std::fmax(maxHeat, s.Heat());
    }
    if (heat) *heat = maxHeat;
    return peak;
}

int main()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // The pump shotgun: 0.8 standing, 0 ducking, 2.5 in air, 1.5 movement, 220 speed.
    WeaponSpread pump{};
    Check(VanillaSpread(3, pump));
    Check(Near(pump.standing, 0.8f) && Near(pump.ducking, 0.0f) && Near(pump.inAir, 2.5f) &&
        Near(pump.movement, 1.5f) && Near(pump.maxSpeed, 220.0f));
    const Factors pf = Derive(pump);
    Check(Near(pf.crouch, 0.6f));                                   // ducking spread 0: the floor, not zero
    Check(Near(pf.air, 1.0f + 2.0f * 2.125f / 3.125f));              // 2.36x
    Check(Near(pf.moveFull, 1.0f + 0.8f * 1.875f / 3.875f));         // 1.39x

    // Standing still is exactly 1 for every gun (the Pavlov values as they are).
    for (int id : kGuns)
    {
        WeaponSpread s{};
        Check(VanillaSpread(id, s));
        Check(s.Valid());
        Check(Target(s, Ground(0.0f)) == 1.0f);
        const Factors f = Derive(s);
        // Vanilla's order per gun, within the clamps.
        Check(f.crouch >= 0.6f - 1e-6f && f.crouch < 1.0f);
        Check(f.air > 1.5f && f.air <= 3.0f);
        Check(f.moveFull > 1.3f && f.moveFull <= 1.8f);
        Check(f.air > f.moveFull);
        // Gradual with speed, saturating at MaxPlayerSpeed.
        float last = 1.0f;
        for (int k = 1; k <= 10; ++k)
        {
            const float m = Target(s, Ground(s.maxSpeed * 0.1f * static_cast<float>(k)));
            Check(m > last);
            last = m;
        }
        Check(Near(last, f.moveFull));
        Check(Near(Target(s, Ground(s.maxSpeed * 3.0f)), f.moveFull));
        Check(Near(Target(s, Ground(s.maxSpeed * 0.5f)), 1.0f + 0.5f * (f.moveFull - 1.0f)));
        // Crouched and moving: both apply.
        Check(Near(Target(s, Ground(s.maxSpeed * 0.5f, true)), f.crouch * (1.0f + 0.5f * (f.moveFull - 1.0f))));
        // In the air the stance spread is replaced: speed does not add.
        Check(Near(Target(s, Air(0.0f)), f.air));
        Check(Near(Target(s, Air(s.maxSpeed)), f.air));
    }

    // Per-gun ratios follow vanilla: the SMGs (ducking 0.7 of standing) gain
    // less from crouching than the rifles (0.125); snipers (in-air 15x their
    // tiny standing spread) suffer most in the air, SMGs least.
    {
        WeaponSpread smg{}, rifle{}, hunting{};
        VanillaSpread(2, smg); VanillaSpread(5, rifle); VanillaSpread(6, hunting);
        Check(Derive(smg).crouch > Derive(rifle).crouch);
        Check(Derive(hunting).air > Derive(rifle).air && Derive(rifle).air > Derive(smg).air);
        Check(Derive(hunting).moveFull > Derive(rifle).moveFull && Derive(rifle).moveFull > Derive(smg).moveFull);
    }

    // Player scales stretch each part about 1; 0 turns it off.
    {
        Tuning t{};
        t.crouchScale = 0.0f;
        Check(Target(pump, Ground(0.0f, true), t) == 1.0f);
        t = {};
        t.airScale = 2.0f;
        Check(Near(Target(pump, Air(), t), 1.0f + 2.0f * (pf.air - 1.0f)));
        t = {};
        t.moveScale = 0.0f;
        Check(Target(pump, Ground(500.0f), t) == 1.0f);
        t = {};
        t.crouchScale = 3.0f; // never below the overall minimum
        Check(Target(pump, Ground(0.0f, true), t) >= kMinMultiplier);
        t = {};
        t.crouchScale = nan;
        Check(Target(pump, Ground(0.0f, true), t) == 1.0f);
    }

    // Unusable data: no change.
    {
        WeaponSpread bad{};
        Check(!bad.Valid());
        Check(Target(bad, Air()) == 1.0f);
        bad = pump; bad.standing = 0.0f;
        Check(Target(bad, Ground(0.0f, true)) == 1.0f);
        bad = pump; bad.maxSpeed = nan;
        Check(Target(bad, Ground(100.0f)) == 1.0f);
        Check(!VanillaSpread(19, bad)); // melee
        Check(Target(pump, Ground(nan)) == 1.0f); // unknown speed: still
    }

    // Smoothing: a jump only counts after the grace (stairs are not jumps),
    // then rises quickly; landing eases back with no frame-to-frame jump.
    {
        Smoother sm;
        const Tuning t{};
        const float dt = 1.0f / 90.0f;
        float v = 1.0f;
        for (int i = 0; i < 8; ++i) v = sm.Update(pump, Air(), t, dt); // 0.089 s off the ground
        Check(v == 1.0f);
        for (int i = 0; i < 4; ++i) sm.Update(pump, Ground(0.0f), t, dt);
        Check(sm.Value() == 1.0f);
        for (int i = 0; i < 90; ++i) v = sm.Update(pump, Air(), t, dt);  // 1 s in the air
        Check(Near(v, pf.air, 0.01f));
        float prev = v, worstStep = 0.0f;
        for (int i = 0; i < 180; ++i)
        {
            v = sm.Update(pump, Ground(0.0f), t, dt);
            worstStep = std::fmax(worstStep, std::fabs(v - prev));
            Check(v <= prev + 1e-6f && v >= 1.0f - 1e-6f);
            prev = v;
        }
        Check(Near(v, 1.0f, 0.01f));
        Check(worstStep < 0.08f); // at 90 Hz, never more than ~6% of the change per frame
        // 0.1 s after landing the kick is still well above standing.
        Smoother land;
        for (int i = 0; i < 120; ++i) land.Update(pump, Air(), t, dt);
        for (int i = 0; i < 9; ++i) land.Update(pump, Ground(0.0f), t, dt);
        Check(land.Value() > 1.5f);
    }

    // Frame-rate independent: 30 Hz and 144 Hz agree after the same time.
    {
        const Tuning t{};
        Smoother a, b;
        for (int i = 0; i < 15; ++i) a.Update(pump, Ground(220.0f, false), t, 1.0f / 30.0f);
        for (int i = 0; i < 72; ++i) b.Update(pump, Ground(220.0f, false), t, 1.0f / 144.0f);
        Check(Near(a.Value(), b.Value(), 0.01f));
        Smoother c, d;
        for (int i = 0; i < 30; ++i) c.Update(pump, Air(), t, 1.0f / 30.0f);
        for (int i = 0; i < 144; ++i) d.Update(pump, Air(), t, 1.0f / 144.0f);
        Check(Near(c.Value(), d.Value(), 0.03f));
        Smoother e;
        e.Update(pump, Ground(0.0f, true), t, nan); // a bad frame time changes nothing
        Check(e.Value() == 1.0f);
        e.Reset(nan);
        Check(e.Value() == 1.0f);
    }

    // The recoil model: stance 1 is bit-for-bit the default; the multiplier
    // scales the kick and the limits together, and the heat builds faster.
    {
        Check(PeakPitch(5, true, 1, 1.0f) == PeakPitch(5, true, 1, 1.0f));
        l4d2vr_recoil::Inputs def{};
        Check(def.stanceMul == 1.0f);
        // M16, one shot two-handed: 0.6 deg, well inside the 5 deg limit.
        const float base = PeakPitch(5, true, 1, 1.0f);
        const float doubled = PeakPitch(5, true, 1, 2.0f);
        const float crouched = PeakPitch(5, true, 1, 0.6f);
        Check(base > 0.3f);
        Check(std::fabs(doubled / base - 2.0f) < 0.05f);
        Check(std::fabs(crouched / base - 0.6f) < 0.05f);
        // A long burst hits the limit: the limit scales too.
        const l4d2vr_recoil::Profile* m16 = l4d2vr_recoil::ProfileForWeapon(5);
        const float lim = m16->limits[1] * m16->grippedLimitMul[1];
        Check(PeakPitch(5, true, 30, 1.0f) <= lim + 1e-3f);
        Check(PeakPitch(5, true, 30, 0.6f) <= 0.6f * lim + 1e-3f);
        float heat1 = 0.0f, heat2 = 0.0f;
        PeakPitch(26, true, 2, 1.0f, &heat1);
        PeakPitch(26, true, 2, 1.5f, &heat2);
        Check(heat2 > heat1);
        // One-handed angle caps are never raised above 45 / 90 degrees.
        float lim3[3];
        l4d2vr_recoil::Inputs one{};
        one.stanceMul = 2.5f;
        l4d2vr_recoil::State::TargetLimits(*m16, one, lim3);
        Check(lim3[1] == 45.0f && lim3[2] == 90.0f && Near(lim3[0], 2.0f * m16->limits[0] * 2.5f));
        // Static (bolt-action) path scales its curve too.
        const float awp1 = PeakPitch(35, true, 1, 1.0f);
        const float awp2 = PeakPitch(35, true, 1, 2.0f);
        Check(awp1 > 0.0f && Near(awp2, 2.0f * awp1, 0.02f * awp1));
        // Garbage multipliers are safe.
        Check(std::isfinite(PeakPitch(5, true, 3, nan)));
    }

    // Table of the derived factors (for the docs).
    std::printf("weapon crouch air move(full)\n");
    for (int id : kGuns)
    {
        WeaponSpread s{};
        VanillaSpread(id, s);
        const Factors f = Derive(s);
        std::printf("%2d %.2f %.2f %.2f\n", id, f.crouch, f.air, f.moveFull);
    }
    std::puts("vr_recoil_stance_tests passed");
    return 0;
}

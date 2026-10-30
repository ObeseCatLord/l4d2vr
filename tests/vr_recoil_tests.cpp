#include "../L4D2VR/vr_recoil.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

// Pavlov recoil model checks. The reference peaks come from a separate
// simulation of the decoded Pavlov model (90 Hz ticks, shots FireRate apart,
// settled limits); this implementation must reproduce them.
static void CheckImpl(bool result, int line, const char* what)
{
    if (!result) { std::fprintf(stderr, "Check failed at line %d: %s\n", line, what); std::abort(); }
}
#define Check(result) CheckImpl((result), __LINE__, #result)

using namespace l4d2vr_recoil;

struct Reference { int weaponId; bool twoHanded; int shots; float back, pitch, yaw; };
static const Reference kReference[] = {
    { 1, true, 1, -1.6555f, 5.3641f, -0.0403f },  // Gun_M9
    { 1, true, 10, -1.6555f, 53.6409f, -7.5709f },  // Gun_M9
    { 1, false, 1, -3.6133f, 8.0461f, -0.0604f },  // Gun_M9
    { 1, false, 10, -3.6133f, 45.0000f, -11.3564f },  // Gun_M9
    { 32, true, 1, -5.0000f, 18.7743f, -0.1761f },  // Gun_DE
    { 32, true, 10, -5.0000f, 20.0000f, -24.1486f },  // Gun_DE
    { 32, false, 1, -10.0000f, 28.1615f, -0.2642f },  // Gun_DE
    { 32, false, 10, -10.0000f, 45.0000f, -36.2229f },  // Gun_DE
    { 2, true, 1, -2.6895f, 0.8046f, -0.0566f },  // Gun_Uzi
    { 2, true, 10, -2.6895f, 5.0000f, 2.0225f },  // Gun_Uzi
    { 2, false, 1, -5.1153f, 0.8046f, -0.0566f },  // Gun_Uzi
    { 2, false, 10, -5.2281f, 8.0461f, 2.0225f },  // Gun_Uzi
    { 7, true, 1, -2.6698f, 0.8046f, -0.0283f },  // Gun_Kriss
    { 7, true, 10, -2.6698f, 5.0000f, -0.9542f },  // Gun_Kriss
    { 7, false, 1, -5.5865f, 4.0231f, -0.1415f },  // Gun_Kriss
    { 7, false, 10, -5.6701f, 40.2307f, -4.7710f },  // Gun_Kriss
    { 33, true, 1, -2.6698f, 0.8046f, -0.0283f },  // Gun_MP5
    { 33, true, 10, -2.6698f, 5.0000f, -0.9542f },  // Gun_MP5
    { 33, false, 1, -5.5865f, 4.0231f, -0.1415f },  // Gun_MP5
    { 33, false, 10, -5.6701f, 40.2307f, -4.7710f },  // Gun_MP5
    { 5, true, 1, -2.2546f, 0.5901f, -0.0415f },  // Gun_M16
    { 5, true, 10, -2.5871f, 5.0000f, 1.2560f },  // Gun_M16
    { 5, false, 1, -5.4847f, 2.9503f, -0.2075f },  // Gun_M16
    { 5, false, 10, -5.6539f, 29.5025f, 6.2800f },  // Gun_M16
    { 26, true, 1, -3.0061f, 1.6092f, -0.0566f },  // Gun_AK47
    { 26, true, 10, -4.6949f, 15.0000f, 1.4613f },  // Gun_AK47
    { 26, false, 1, -3.0061f, 8.0461f, -0.2829f },  // Gun_AK47
    { 26, false, 10, -19.5191f, 45.0000f, 7.3066f },  // Gun_AK47
    { 9, true, 1, -1.5031f, 3.2185f, -0.3279f },  // Gun_SCAR20
    { 9, true, 10, -2.6996f, 10.0000f, -10.0000f },  // Gun_SCAR20
    { 9, false, 1, -4.9856f, 32.1846f, -3.2794f },  // Gun_SCAR20
    { 9, false, 10, -5.5895f, 45.0000f, -90.0000f },  // Gun_SCAR20
    { 34, true, 1, -2.6667f, 0.8046f, -0.0548f },  // Gun_Galil
    { 34, true, 10, -2.6699f, 8.0461f, 1.5814f },  // Gun_Galil
    { 34, false, 1, -10.6667f, 5.6323f, -0.3837f },  // Gun_Galil
    { 34, false, 10, -10.8155f, 45.0000f, 11.0701f },  // Gun_Galil
    { 6, true, 1, -1.5031f, 3.2185f, -0.3279f },  // Gun_SKS
    { 6, true, 10, -2.6952f, 10.0000f, -10.0000f },  // Gun_SKS
    { 6, false, 1, -4.9409f, 32.1846f, -3.2794f },  // Gun_SKS
    { 6, false, 10, -5.6604f, 45.0000f, -90.0000f },  // Gun_SKS
    { 10, true, 1, -3.0061f, 1.6092f, -0.0593f },  // Gun_AutoSniper
    { 10, true, 10, -4.6946f, 15.0000f, -1.8906f },  // Gun_AutoSniper
    { 10, false, 1, -3.0061f, 8.0461f, -0.2965f },  // Gun_AutoSniper
    { 10, false, 10, -17.7306f, 45.0000f, -9.4531f },  // Gun_AutoSniper
    { 35, true, 1, -4.9191f, 4.5974f, 2.3138f },  // Gun_AWP
    { 35, true, 10, -4.9191f, 4.5974f, 2.3138f },  // Gun_AWP
    { 35, false, 1, -9.8381f, 13.7923f, 6.9413f },  // Gun_AWP
    { 35, false, 10, -9.8381f, 13.7923f, 6.9413f },  // Gun_AWP
    { 36, true, 1, -7.8705f, 3.6779f, 0.9255f },  // Gun_Kar98
    { 36, true, 10, -7.8705f, 3.6779f, 0.9255f },  // Gun_Kar98
    { 36, false, 1, -15.7410f, 11.0338f, 2.7765f },  // Gun_Kar98
    { 36, false, 10, -15.7410f, 11.0338f, 2.7765f },  // Gun_Kar98
    { 3, true, 1, -9.3534f, 16.0923f, -0.2264f },  // Gun_Shotgun
    { 3, true, 10, -9.3534f, 90.0000f, -24.0861f },  // Gun_Shotgun
    { 3, false, 1, -19.4789f, 32.1846f, -0.4527f },  // Gun_Shotgun
    { 3, false, 10, -19.4789f, 45.0000f, -48.1723f },  // Gun_Shotgun
    { 4, true, 1, -7.5154f, 10.7282f, -0.1509f },  // Gun_AutoShotgun
    { 4, true, 10, -9.6687f, 82.3160f, -11.8235f },  // Gun_AutoShotgun
    { 4, false, 1, -18.7068f, 42.9127f, -0.6036f },  // Gun_AutoShotgun
    { 4, false, 10, -18.7068f, 45.0000f, -47.2939f },  // Gun_AutoShotgun
    { 37, true, 1, -3.1728f, 0.8046f, -0.0566f },  // Gun_LMGA
    { 37, true, 10, -3.1659f, 8.0461f, -1.9084f },  // Gun_LMGA
    { 37, false, 1, -11.0452f, 3.2185f, -0.2264f },  // Gun_LMGA
    { 37, false, 10, -13.6707f, 32.1846f, -7.6336f },  // Gun_LMGA
    { 21, true, 1, -2.6698f, 0.8046f, 0.1244f },  // Gun_NewtonLauncher
    { 21, true, 10, -2.6698f, 0.8238f, 0.1244f },  // Gun_NewtonLauncher
    { 21, false, 1, -5.5865f, 4.0231f, 0.6221f },  // Gun_NewtonLauncher
    { 21, false, 10, -5.5865f, 4.1188f, 0.6221f },  // Gun_NewtonLauncher
};

struct Peaks { float back = 0.0f, pitch = 0.0f, yaw = 0.0f; float settle = 0.0f; };

static Peaks Run(const Profile& p, int shots, bool twoHanded, float hz = 90.0f)
{
    State s;
    Inputs in{};
    in.twoHanded = twoHanded;
    s.Reset(&p, in);
    s.Seed(1234u);
    const float dt = 1.0f / hz;
    const float rate = std::fmax(p.fireRate, dt);
    const float seconds = 0.5f + shots * rate + 2.0f;
    float t = 0.0f, next = 0.5f;
    int fired = 0;
    Peaks peaks{};
    while (t < seconds)
    {
        const bool held = fired < shots && t >= 0.5f;
        if (fired < shots && t >= next) { s.Fire(); ++fired; next += rate; }
        in.triggerHeld = held;
        const Offset o = s.Tick(dt, in);
        t += dt;
        if (t > 0.5f)
        {
            if (std::fabs(o.backCm) > std::fabs(peaks.back)) peaks.back = o.backCm;
            if (std::fabs(o.pitchDeg) > std::fabs(peaks.pitch)) peaks.pitch = o.pitchDeg;
            if (std::fabs(o.yawDeg) > std::fabs(peaks.yaw)) peaks.yaw = o.yawDeg;
            if (std::fmax(std::fabs(o.backCm), std::fmax(std::fabs(o.pitchDeg), std::fabs(o.yawDeg))) > 0.05f)
                peaks.settle = t - 0.5f;
        }
    }
    return peaks;
}

static bool Close(float a, float b, float rel, float abs)
{
    return std::fabs(std::fabs(a) - std::fabs(b)) <= abs + rel * std::fabs(b);
}

static void TestCurveEval()
{
    const Curve y = DynamicCurve(1);
    Check(std::fabs(Eval(y, -1.0f) - 0.1f) < 1e-6f);          // constant before the first key
    Check(std::fabs(Eval(y, 0.0128f) - 1.4879f) < 1e-4f);     // exact key value
    Check(std::fabs(Eval(y, 5.0f)) < 1e-6f);                  // constant after the last key
    const float mid = Eval(y, (0.018f + 0.0195f) * 0.5f);     // linear segment
    Check(std::fabs(mid - (0.9184f + 0.3734f) * 0.5f) < 1e-4f);
    // The pitch velocity profile integrates to ~0.0272 per unit multiplier.
    double integral = 0.0;
    for (int i = 0; i < 20000; ++i) integral += Eval(y, (i + 0.5f) * (0.2f / 20000.0f)) * (0.2 / 20000.0);
    Check(std::fabs(integral - 0.0272) < 0.0006);
    for (int k = 0; k < 3; ++k) Check(std::fabs(Eval(CounterCurve(k), 1.0f) - 1.0f) < 1e-3f);
}

static void TestProfilesCoverEveryGun()
{
    const int ids[] = { 1, 32, 2, 7, 33, 5, 26, 9, 34, 6, 10, 35, 36, 3, 8, 4, 11, 37, 21 };
    for (int id : ids) Check(ProfileForWeapon(id) != nullptr);
    Check(ProfileForWeapon(0) == nullptr);
    Check(ProfileForWeapon(19) == nullptr); // melee
    Check(!ProfileForWeapon(35)->dynamic && !ProfileForWeapon(36)->dynamic);
    Check(ProfileForWeapon(8)->angleMul == ProfileForWeapon(3)->angleMul);
}

static void TestAgainstReference()
{
    for (const Reference& r : kReference)
    {
        const Profile* p = ProfileForWeapon(r.weaponId);
        Check(p != nullptr);
        const Peaks k = Run(*p, r.shots, r.twoHanded);
        // Single shots are deterministic; bursts with recovery between shots
        // depend on the random recovery jitter.
        const float rel = r.shots == 1 ? 0.01f : 0.06f;
        const bool ok = Close(k.back, r.back, rel, 0.02f) && Close(k.pitch, r.pitch, rel, 0.02f) &&
            Close(k.yaw, r.yaw, r.shots == 1 ? 0.03f : 0.25f, r.shots == 1 ? 0.02f : 0.5f);
        if (!ok)
            std::fprintf(stderr, "gun %d (%s) two=%d shots=%d: got %.3f %.3f %.3f want %.3f %.3f %.3f\n", r.weaponId,
                p->pavlovGun, r.twoHanded ? 1 : 0, r.shots, k.back, k.pitch, k.yaw, r.back, r.pitch, r.yaw);
        Check(ok);
    }
}

static void TestPistolSingleShot()
{
    // M9: ~5.4 deg of muzzle rise, the slide-back pulled toward the 2 cm
    // limit, back at rest within about 0.3 s.
    const Peaks k = Run(*ProfileForWeapon(1), 1, true);
    Check(k.pitch > 5.2f && k.pitch < 5.5f);
    Check(k.back < -1.5f && k.back > -2.01f);
    Check(std::fabs(k.yaw) < 0.1f);   // the first shot goes nearly straight up
    Check(k.settle < 0.40f);
}

static void TestRecoveryReturnsToRest()
{
    for (const Profile& p : kProfiles)
    {
        State s;
        Inputs in{};
        in.twoHanded = true;
        s.Reset(&p, in);
        s.Fire();
        Offset o{};
        for (int i = 0; i < 300; ++i) o = s.Tick(1.0f / 90.0f, in);
        Check(std::fabs(o.backCm) < 1e-3f && std::fabs(o.pitchDeg) < 1e-3f && std::fabs(o.yawDeg) < 1e-3f);
        Check(s.Heat() < 1e-3f);
    }
}

static void TestTwoHandsTameHeavyGuns()
{
    // Guns that need two hands kick far harder held in one.
    for (int id : { 9, 6, 26, 4 })
    {
        const Profile& p = *ProfileForWeapon(id);
        const Peaks two = Run(p, 1, true), one = Run(p, 1, false);
        Check(std::fabs(one.pitch) > 3.0f * std::fabs(two.pitch));
    }
    const Peaks two = Run(*ProfileForWeapon(1), 1, true), one = Run(*ProfileForWeapon(1), 1, false);
    Check(one.pitch > 1.4f * two.pitch && one.pitch < 1.6f * two.pitch);
    // Two-handed limits: an AK burst never climbs past 15 deg; one-handed it reaches 45.
    const Peaks akTwo = Run(*ProfileForWeapon(26), 30, true), akOne = Run(*ProfileForWeapon(26), 30, false);
    Check(akTwo.pitch <= 15.0f + 1e-3f && akTwo.pitch > 14.0f);
    Check(akOne.pitch <= 45.0f + 1e-3f && akOne.pitch > 44.0f);
}

static void TestBurstWandersSideways()
{
    // Heat builds over a burst: the first shot has almost no yaw, a long
    // string swings the muzzle sideways.
    const Profile& p = *ProfileForWeapon(37); // M60 -> LMGA
    State s;
    Inputs in{};
    in.twoHanded = true;
    s.Reset(&p, in);
    float firstYaw = 0.0f, maxAbsYaw = 0.0f;
    float t = 0.0f, next = 0.0f;
    int fired = 0;
    while (t < 3.0f)
    {
        if (t >= next && fired < 40) { s.Fire(); ++fired; next += p.fireRate; }
        in.triggerHeld = fired < 40;
        const Offset o = s.Tick(1.0f / 90.0f, in);
        t += 1.0f / 90.0f;
        if (fired == 1) firstYaw = std::fmax(firstYaw, std::fabs(o.yawDeg));
        maxAbsYaw = std::fmax(maxAbsYaw, std::fabs(o.yawDeg));
    }
    Check(firstYaw < 0.1f);
    Check(maxAbsYaw > 1.0f);
}

static void TestStaticBoltRifle()
{
    // AWP: a position curve over one second, 4.6 deg up and 4.9 cm back.
    const Profile& p = *ProfileForWeapon(35);
    State s;
    Inputs in{};
    in.twoHanded = true;
    s.Reset(&p, in);
    Check(std::fabs(s.Current(in).pitchDeg) < 1e-6f);
    s.Fire();
    float peakPitch = 0.0f, peakBack = 0.0f;
    Offset o{};
    for (int i = 0; i < 120; ++i)
    {
        o = s.Tick(1.0f / 90.0f, in);
        peakPitch = std::fmax(peakPitch, o.pitchDeg);
        peakBack = std::fmin(peakBack, o.backCm);
    }
    Check(peakPitch > 4.5f && peakPitch < 4.7f);
    Check(peakBack < -4.8f && peakBack > -5.0f);
    Check(std::fabs(o.pitchDeg) < 1e-3f && std::fabs(o.backCm) < 1e-3f);
}

static void TestFrameRateIndependence()
{
    // Sub-stepping keeps a kick within ~5% between 45 and 144 Hz (Pavlov's
    // own substeps have the same small dependence).
    for (int id : { 1, 26, 3, 35 })
    {
        const Profile& p = *ProfileForWeapon(id);
        const Peaks b = Run(p, 1, true, 90.0f);
        for (float hz : { 45.0f, 72.0f, 120.0f, 144.0f })
        {
            const Peaks a = Run(p, 1, true, hz);
            Check(std::fabs(a.pitch - b.pitch) <= 0.05f * std::fabs(b.pitch) + 0.02f);
            Check(std::fabs(a.back - b.back) <= 0.05f * std::fabs(b.back) + 0.02f);
        }
    }
}

struct V3 { float x, y, z; };
static void TestApplyToBasis()
{
    // Source basis: forward +x, right -y, up +z.
    V3 f{ 1, 0, 0 }, r{ 0, -1, 0 }, u{ 0, 0, 1 };
    Offset o{};
    o.pitchDeg = 10.0f;
    ApplyToBasis(o, f, r, u);
    Check(f.z > 0.0f && std::fabs(f.z - std::sin(10.0f * 0.0174533f)) < 1e-4f); // muzzle up
    Check(std::fabs(u.x + std::sin(10.0f * 0.0174533f)) < 1e-4f);
    V3 f2{ 1, 0, 0 }, r2{ 0, -1, 0 }, u2{ 0, 0, 1 };
    Offset y{};
    y.yawDeg = 10.0f;
    ApplyToBasis(y, f2, r2, u2);
    Check(f2.y < -0.17f && f2.y > -0.18f); // muzzle right (toward -y)
    Check(std::fabs(u2.z - 1.0f) < 1e-5f);
}

static void TestShotGate()
{
    ShotGate g;
    Check(g.Accept(10));
    Check(!g.Accept(10));     // a prediction replay of the same command
    Check(!g.Accept(9));
    Check(g.Accept(11));
    Check(g.Accept(0));       // unknown command numbers still count
    g.Reset();
    Check(g.Accept(5));
}

int main()
{
    TestCurveEval();
    TestProfilesCoverEveryGun();
    TestAgainstReference();
    TestPistolSingleShot();
    TestRecoveryReturnsToRest();
    TestTwoHandsTameHeavyGuns();
    TestBurstWandersSideways();
    TestStaticBoltRifle();
    TestFrameRateIndependence();
    TestApplyToBasis();
    TestShotGate();
    std::puts("vr_recoil_tests passed");
    return 0;
}

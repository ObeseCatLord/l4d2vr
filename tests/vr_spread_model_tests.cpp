#include <limits>
#if defined(VECTOR_PARANOIA) && !defined(VEC_T_NAN)
#define VEC_T_NAN std::numeric_limits<float>::quiet_NaN()
#endif
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_spread_model.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "Spread model failed at %d\n", __LINE__); std::abort(); } } while(false)
static bool Near(float a, float b, float eps = 0.0001f) { return std::fabs(a - b) <= eps; }
int main()
{
    using namespace l4d2vr_spread;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // Rifle-like script values.
    WeaponSpreadData rifle{};
    rifle.minStandingSpread = 0.5f;
    rifle.minDuckingSpread = 0.25f;
    rifle.minInAirSpread = 3.0f;
    rifle.maxMovementSpread = 2.0f;
    rifle.spreadPerShot = 0.8f;
    rifle.maxSpread = 5.0f;
    rifle.spreadDecay = 4.0f;

    SpreadModel model;
    CHECK(model.Current() == 0.0f);
    model.OnShot(); // no weapon: ignored
    CHECK(model.Accumulated() == 0.0f);

    WeaponSpreadData bad = rifle;
    bad.maxSpread = 0.0f;
    CHECK(!model.SetWeapon(bad) && !model.HasWeapon());
    bad = rifle;
    bad.spreadDecay = nan;
    CHECK(!model.SetWeapon(bad));
    bad = rifle;
    bad.minStandingSpread = -1.0f;
    CHECK(!model.SetWeapon(bad));
    CHECK(model.SetWeapon(rifle) && model.HasWeapon());

    const Stance standing{true, false, 0.0f, 220.0f};
    model.Update(standing, 0.016f);
    CHECK(Near(model.Current(), 0.5f));

    // Shots accumulate by SpreadPerShot and clamp at MaxSpread.
    model.OnShot();
    CHECK(Near(model.Accumulated(), 1.3f));
    model.OnShot();
    CHECK(Near(model.Accumulated(), 2.1f));
    for (int i = 0; i < 20; ++i) model.OnShot();
    CHECK(Near(model.Accumulated(), 5.0f) && Near(model.Current(), 5.0f));

    // Decay at SpreadDecay deg/s toward the stance floor, never below it.
    model.Update(standing, 0.5f);
    CHECK(Near(model.Accumulated(), 3.0f));
    model.Update(standing, 10.0f);
    CHECK(Near(model.Accumulated(), 0.5f) && Near(model.Current(), 0.5f));
    model.Update(standing, nan); // bad dt does not move anything
    model.Update(standing, -1.0f);
    CHECK(Near(model.Accumulated(), 0.5f));

    // Ducking is tighter than standing.
    const Stance ducking{true, true, 0.0f, 220.0f};
    model.Update(ducking, 1.0f);
    CHECK(Near(model.Current(), 0.25f));
    CHECK(model.Current() < 0.5f);

    // In air is wider, overriding ducking; landing recovers at the decay rate.
    const Stance air{false, true, 0.0f, 220.0f};
    model.Update(air, 0.016f);
    CHECK(Near(model.Current(), 3.0f));
    model.Update(standing, 0.25f);
    CHECK(Near(model.Current(), 2.0f));
    model.Update(standing, 10.0f);
    CHECK(Near(model.Current(), 0.5f));

    // Movement adds lerp(0, MaxMovementSpread, speed/maxSpeed), clamped.
    model.Update(Stance{true, false, 110.0f, 220.0f}, 0.016f);
    CHECK(Near(model.Current(), 1.5f));
    model.Update(Stance{true, false, 440.0f, 220.0f}, 0.016f);
    CHECK(Near(model.Current(), 2.5f));
    model.Update(Stance{true, false, 440.0f, 0.0f}, 0.016f); // unknown max speed: no movement term
    CHECK(Near(model.Current(), 0.5f));
    model.Update(Stance{true, false, nan, 220.0f}, 0.016f);
    CHECK(Near(model.Current(), 0.5f));
    // Accumulated recoil above the movement floor wins.
    model.Update(Stance{true, false, 110.0f, 220.0f}, 0.016f);
    model.OnShot(); // 0.5 + 0.8 = 1.3 < 1.5 movement
    CHECK(Near(model.Current(), 1.5f));
    model.OnShot(); // 2.1
    CHECK(Near(model.Current(), 2.1f));

    // Ground truth: the native per-shot spread overrides a low estimate.
    model.Update(standing, 10.0f);
    CHECK(Near(model.Accumulated(), 0.5f));
    model.OnShot(3.5f);
    CHECK(Near(model.Accumulated(), 4.3f));
    model.OnShot(1.0f); // lower native value does not pull the estimate down
    CHECK(Near(model.Accumulated(), 5.0f));
    model.Update(standing, 10.0f);
    model.OnShot(nan); // unavailable native value: estimate only
    CHECK(Near(model.Accumulated(), 1.3f));
    model.OnShot(-2.0f);
    CHECK(Near(model.Accumulated(), 2.1f));
    model.OnShot(50.0f); // still clamped to MaxSpread
    CHECK(Near(model.Accumulated(), 5.0f));

    model.Reset();
    CHECK(!model.HasWeapon() && model.Current() == 0.0f);

    // Projection helpers.
    CHECK(Near(CrosshairGapRadians(180.0f / kPi), 1.0f));
    CHECK(CrosshairGapRadians(nan) == 0.0f && CrosshairGapRadians(-1.0f) == 0.0f);
    CHECK(Near(CrosshairGapRadians(1000.0f), kMaxGapDeg * kPi / 180.0f));
    CHECK(Near(ProjectGapPixels(45.0f, 90.0f, 2000.0f), 1000.0f, 0.05f));
    CHECK(Near(ProjectGapPixels(0.0f, 90.0f, 2000.0f), 0.0f));
    CHECK(ProjectGapPixels(2.0f, 90.0f, 2000.0f) > ProjectGapPixels(1.0f, 90.0f, 2000.0f));
    CHECK(ProjectGapPixels(2.0f, 60.0f, 2000.0f) > ProjectGapPixels(2.0f, 90.0f, 2000.0f));
    CHECK(ProjectGapPixels(2.0f, nan, 2000.0f) == 0.0f && ProjectGapPixels(2.0f, 90.0f, 0.0f) == 0.0f);
    CHECK(ProjectGapPixels(2.0f, 180.0f, 2000.0f) == 0.0f);
    CHECK(Near(ProjectGapPixelsAsymmetric(45.0f, -1.0f, 1.0f, 2000.0f), 1000.0f, 0.05f));
    CHECK(Near(ProjectGapPixelsAsymmetric(45.0f, -1.2f, 0.8f, 2000.0f), 1000.0f, 0.05f));
    CHECK(ProjectGapPixelsAsymmetric(45.0f, 1.0f, -1.0f, 2000.0f) == 0.0f);
    CHECK(Near(WorldRadiusAtDistance(45.0f, 100.0f), 100.0f, 0.01f));
    CHECK(Near(WorldRadiusAtDistance(1.0f, 1000.0f), 1000.0f * std::tan(kPi / 180.0f), 0.01f));
    CHECK(WorldRadiusAtDistance(1.0f, -5.0f) == 0.0f && WorldRadiusAtDistance(nan, 5.0f) == 0.0f);
    std::puts("vr_spread_model tests passed");
    return 0;
}

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
#include "../L4D2VR/vr_scope_optics.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "Scope optics failed at %d\n", __LINE__); std::abort(); } } while(false)
int main()
{
    using namespace l4d2vr_optics;
    WeaponTracker weapon;
    CHECK(weapon.Observe(10u, 0x3005u, 35));
    const auto first = weapon.Current();
    CHECK(first.Valid() && !weapon.Observe(10u, 0x3005u, 35));
    ImageStamp image{first, 2.0f, 0u, 7u};
    CHECK(!ImageMatches(image, first, 2.0f, 7u, 100u)); // Submission is not completion.
    image.completedAt = 100u;
    CHECK(ImageMatches(image, first, 2.0f, 7u, 150u));
    CHECK(!ImageMatches(image, first, 4.0f, 7u, 150u)); // Old zoom cannot label a new view.
    CHECK(!ImageMatches(image, first, 2.0f, 8u, 150u)); // Device/queue lifecycle changed.
    CHECK(ImageMatches(image, first, 2.0f, 7u, 350u) && !ImageMatches(image, first, 2.0f, 7u, 351u)); // held 250 ms (2.0.12)
    CHECK(!ImageMatches(image, first, 2.0f, 7u, 99u));
    CHECK(weapon.Observe(10u, 0x4005u, 35));
    CHECK(!ImageMatches(image, weapon.Current(), 2.0f, 7u, 150u)); // Same type, different gun.
    CHECK(weapon.Observe(10u, 0x3005u, 1));
    CHECK(!weapon.Current().Valid());
    CHECK(weapon.Observe(10u, 0x3005u, 35));
    CHECK(!ImageMatches(image, weapon.Current(), 2.0f, 7u, 150u)); // Drawing it again needs a new render.
    CHECK(weapon.Observe(20u, 0x3005u, 35));
    CHECK(!ImageMatches(image, weapon.Current(), 2.0f, 7u, 150u));
    image = {weapon.Current(), 4.0f, 160u, 8u};
    CHECK(ImageMatches(image, weapon.Current(), 4.0f, 8u, 180u));
    CHECK(!ImageMatches(image, weapon.Current(), std::numeric_limits<float>::quiet_NaN(), 8u, 180u));
    CHECK(weapon.Observe(20u, 0xffffffffu, 35));
    CHECK(!weapon.Current().Valid());
    for (int id = 0; id < 50; ++id)
        CHECK(HasOptic(id) == (id == 6 || id == 10 || id == 34 || id == 35 || id == 36));
    const Vector lens(10.0f, 0.0f, 0.0f), axis(1.0f, 0.0f, 0.0f);
    // Eyebox (2.0.12): full within the eyebox, none past 1.5x, the eye
    // 1-80 cm behind the lens (units: 100 per metre here).
    CHECK(EvaluateEye(Vector(0,0,0), lens, axis, 1.0f, 100.0f, 4.0f).alpha == 1.0f);
    CHECK(EvaluateEye(Vector(0,1,0), lens, axis, 1.0f, 100.0f, 4.0f).alpha == 1.0f);   // 5.7 deg off: full
    // An 8 degree eyebox: full to 8, fading to 12, none past 12.
    CHECK(EvaluateEye(Vector(0,1.6f,0), lens, axis, 1.0f, 100.0f, 4.0f, 8.0f).alpha > 0.2f);  // 9.1 deg: fading
    CHECK(EvaluateEye(Vector(0,1.6f,0), lens, axis, 1.0f, 100.0f, 4.0f, 8.0f).alpha < 0.8f);
    CHECK(EvaluateEye(Vector(0,2.2f,0), lens, axis, 1.0f, 100.0f, 4.0f, 8.0f).alpha == 0.0f); // 12.4 deg: none
    // The default 30 degrees: table at 0/10/20/30/45 deg.
    volatile float defaultEyebox = kDefaultEyeboxDeg;
    CHECK(defaultEyebox == 30.0f);
    auto at = [&](float deg) {
        const float t = std::tan(deg * 3.14159265f / 180.0f) * 10.0f;
        return EvaluateEye(Vector(0,t,0), lens, axis, 1.0f, 100.0f, 4.0f);
    };
    CHECK(at(0.0f).alpha == 1.0f && at(10.0f).alpha == 1.0f && at(20.0f).alpha == 1.0f && at(29.0f).alpha == 1.0f);
    CHECK(at(37.5f).alpha > 0.4f && at(37.5f).alpha < 0.6f);
    CHECK(at(45.5f).alpha == 0.0f && at(45.5f).behind);
    CHECK(EvaluateEye(Vector(4,6.4f,0), lens, axis, 1.0f, 100.0f, 4.0f).alpha == 0.0f &&
        EvaluateEye(Vector(4,6.4f,0), lens, axis, 1.0f, 100.0f, 4.0f).behind); // the other eye at the face: dimmed
    CHECK(EvaluateEye(Vector(11,0,0), lens, axis, 1.0f, 100.0f, 4.0f).alpha == 0.0f);   // in front of the lens
    CHECK(!EvaluateEye(Vector(11,0,0), lens, axis, 1.0f, 100.0f, 4.0f).behind);
    // Relief 0.01 / 0.3 / 0.8 / 0.9 m.
    CHECK(EvaluateEye(Vector(9.5f,0,0), lens, axis, 1.0f, 100.0f, 4.0f).alpha == 0.0f); // 0.5 cm: inside the tube
    CHECK(EvaluateEye(Vector(8.9f,0,0), lens, axis, 1.0f, 100.0f, 4.0f).alpha == 1.0f); // 1.1 cm
    CHECK(EvaluateEye(Vector(-20,0,0), lens, axis, 1.0f, 100.0f, 4.0f).alpha == 1.0f);  // 30 cm
    CHECK(EvaluateEye(Vector(-69,0,0), lens, axis, 1.0f, 100.0f, 4.0f).alpha == 1.0f);  // 79 cm
    const EyeSample far = EvaluateEye(Vector(-80,0,0), lens, axis, 1.0f, 100.0f, 4.0f); // 90 cm: too far
    CHECK(far.alpha == 0.0f && far.behind);
    // Outside the eye box the lens is dimmed, never black, while the eye is behind it.
    CHECK(LensBrightness(far, kDefaultOutsideEyeboxBrightness) == kDefaultOutsideEyeboxBrightness);
    CHECK(LensBrightness(at(0.0f), 0.35f) == 1.0f);
    CHECK(LensBrightness(EvaluateEye(Vector(11,0,0), lens, axis, 1.0f, 100.0f, 4.0f), 0.35f) == 0.0f);
    CHECK(DimRenderDue(1000u, 0u) && !DimRenderDue(1050u, 1000u) && DimRenderDue(1100u, 1000u));
    // Configurable relief window (ScopeEyeReliefRangeMeters).
    CHECK(EvaluateEye(Vector(-30,0,0), lens, axis, 1.0f, 100.0f, 4.0f, 30.0f, 0.02f, 0.30f).alpha == 0.0f);
    // Replacement models keep the stock lens; overrides parse.
    bool replacement = false;
    CHECK(FindModelOrStock("models/v_models/v_huntingrifle.mdl", 4068759009u, replacement) && !replacement);
    CHECK(FindModelOrStock("models/v_models/v_huntingrifle.mdl", 12345u, replacement) && replacement);
    CHECK(!FindModelOrStock("models/v_models/v_desert_rifle.mdl", 3776611816u, replacement) && !replacement);
    LensOverride lensOverride{};
    CHECK(ParseLensOverride("V_HuntingRifle: 14.04, 0.50,48.94,0.66", lensOverride) &&
        lensOverride.model == "v_huntingrifle" && lensOverride.radius == 0.66f && lensOverride.bindCenter.z == 48.94f);
    CHECK(!ParseLensOverride("v_huntingrifle:1,2,3", lensOverride) && !ParseLensOverride(":1,2,3,4", lensOverride) &&
        !ParseLensOverride("x:1,2,3,0", lensOverride) && !ParseLensOverride("x:1,2,3,4,5", lensOverride));
    CHECK(EvaluateEye(Vector(0,0,0), lens, Vector(0,0,0), 1.0f, 100.0f, 4.0f).alpha == 0.0f);
    // A headset eye a few millimetres off the axis (the old exit-pupil gate
    // was 0.66 / 2 = 0.33 units = 7.6 mm at 43.2 units/m) still sees it.
    CHECK(EvaluateEye(Vector(0,0.5f,0.3f), Vector(4.32f,0,0), axis, 0.66f, 43.2f, 2.0f).alpha == 1.0f);
    CHECK(EvaluateEye(Vector(0,0,0), lens, axis, 1.0f, 100.0f, 4.0f, 4.0f).alpha == 1.0f);
    CHECK(EvaluateEye(Vector(0,1,0), lens, axis, 1.0f, 100.0f, 4.0f, 4.0f).alpha < 0.75f); // a tighter eyebox
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    CHECK(ResolveNearPlane(nan, 2.0f) == 2.0f);
    CHECK(ResolveNearPlane(infinity, 3.0f) == 3.0f);
    CHECK(ResolveNearPlane(0.0f, 3.0f) == 3.0f);
    CHECK(ResolveNearPlane(-1.0f, 3.0f) == 3.0f);
    CHECK(ResolveNearPlane(nan, nan) == 2.0f);
    CHECK(ResolveNearPlane(0.01f, 2.0f) == 0.1f);
    CHECK(ResolveNearPlane(100.0f, 2.0f) == 64.0f);
    CHECK(ValidClipPlanes(2.0f, 10000.0f));
    CHECK(!ValidClipPlanes(nan, 10000.0f));
    CHECK(!ValidClipPlanes(2.0f, infinity));
    CHECK(!ValidClipPlanes(0.0f, 10000.0f));
    CHECK(!ValidClipPlanes(2.0f, 2.0f));
    CHECK(!ValidClipPlanes(3.0f, 2.0f));
    Vector invalidEye(0,0,0); invalidEye.x = nan;
    CHECK(EvaluateEye(invalidEye, lens, axis, 1,100,4).alpha == 0.0f);
    CHECK(RenderFov(1,10,nan) == 0.0f);
    CHECK(RenderFov(0.037f, 10.8f, 16.0f) == 0.0f);
    CHECK(RenderFov(8.0f, 1.512f, 1.0f) == 0.0f);
    CHECK(!ValidRenderFov(0.09f) && !ValidRenderFov(120.01f) && !ValidRenderFov(nan));
    CHECK(ValidRenderFov(0.1f) && ValidRenderFov(120.0f));
    for (const float relief : {4.0f, 8.0f, 15.0f})
        for (const float magnification : {1.0f, 2.0f, 4.0f, 8.0f})
        {
            const float fov = RenderFov(1.0f, relief, magnification);
            const float recovered = 1.0f / (relief * std::tan(fov * 3.14159265358979323846f / 360.0f));
            CHECK(std::fabs(recovered - magnification) < 0.001f);
        }
    const ModelLens* stockLenses[] = {
        FindModel("models/v_models/v_huntingrifle.mdl", 4068759009u),
        FindModel("models/v_models/v_sniper_military.mdl", 1046396963u),
        FindModel("models/v_models/v_rif_sg552.mdl", 1582432951u),
        FindModel("models/v_models/v_snip_awp.mdl", 1241414224u),
        FindModel("models/v_models/v_snip_scout.mdl", 2116180069u)
    };
    for (const ModelLens* stock : stockLenses)
    {
        CHECK(stock);
        for (const float reliefMeters : {0.035f, 0.05f, 0.1f, 0.25f})
            for (const float magnification : {1.0f, 2.0f, 4.0f, 8.0f, 16.0f})
            {
                const float relief = reliefMeters * 43.2f;
                const float fov = RenderFov(stock->radius, relief, magnification);
                CHECK(ValidRenderFov(fov));
                const float recovered = stock->radius /
                    (relief * std::tan(fov * 3.14159265358979323846f / 360.0f));
                CHECK(std::fabs(recovered - magnification) < 0.001f);
            }
    }
    CHECK(Magnification(20,20,2) == 2.0f);
    CHECK(Magnification(10,20,2) > 4.0f);
    CHECK(Magnification(3,20,2) > 10.0f);
    CHECK(FindModel("models/v_models/v_snip_awp.mdl",1241414224u));
    CHECK(!FindModel("models/v_models/v_snip_awp.mdl",1u));
    CHECK(!FindModel("models/v_models/custom.mdl",1241414224u));
    CHECK(Fresh(100u,50u));
    CHECK(Fresh(300u,50u) && !Fresh(301u,50u));
    CHECK(!Fresh(49u,50u));
    CHECK(!Fresh(100u,0u));
    const Pose previous{Vector(0,0,0),Vector(1,0,0),Vector(0,-1,0),Vector(0,0,1)};
    const Pose current{Vector(5,3,0),Vector(0,1,0),Vector(1,0,0),Vector(0,0,1)};
    Vector rebased(0,0,0);
    CHECK(RebasePoint(lens,previous,current,rebased));
    CHECK((rebased-Vector(5,13,0)).Length() < 0.001f);
    CHECK(RebaseVector(axis,previous,current,rebased));
    CHECK((rebased-Vector(0,1,0)).Length() < 0.001f);
    CHECK(!RebasePoint(invalidEye,previous,current,rebased));
    // 2.0.14 Pavlov activation: within 40 cm of the lens and 5 cm of its axis, eye behind it.
    {
        const float upm = 40.0f;   // units per metre
        const Vector pavlovLens(100.0f, 0.0f, 0.0f), pavlovAxis(1.0f, 0.0f, 0.0f);
        CHECK(PavlovActivation(Vector(100.0f - 0.10f * upm, 0.0f, 0.0f), pavlovLens, pavlovAxis, upm, 0.40f, 0.05f));
        CHECK(PavlovActivation(Vector(100.0f - 0.10f * upm, 0.04f * upm, 0.0f), pavlovLens, pavlovAxis, upm, 0.40f, 0.05f));
        CHECK(!PavlovActivation(Vector(100.0f - 0.10f * upm, 0.06f * upm, 0.0f), pavlovLens, pavlovAxis, upm, 0.40f, 0.05f));
        CHECK(!PavlovActivation(Vector(100.0f - 0.45f * upm, 0.0f, 0.0f), pavlovLens, pavlovAxis, upm, 0.40f, 0.05f));
        CHECK(!PavlovActivation(Vector(100.0f + 0.10f * upm, 0.0f, 0.0f), pavlovLens, pavlovAxis, upm, 0.40f, 0.05f)); // in front of the scope
        CHECK(PavlovActivation(Vector(100.0f - 0.45f * upm, 0.3f * upm, 0.0f), pavlovLens, pavlovAxis, upm, 0.0f, 0.0f)); // limits off
        CHECK(!PavlovActivation(Vector(0.0f, 0.0f, 0.0f), pavlovLens, Vector(0.0f, 0.0f, 0.0f), upm, 0.4f, 0.05f));
        for (int id = 0; id < 50; ++id)
            CHECK((PavlovOpticFov(id) > 0.0f) == HasOptic(id));
        CHECK(std::fabs(PavlovOpticFov(35) - 5.625f) < 1e-4f && std::fabs(PavlovOpticFov(36) - 5.332f) < 1e-4f);
        CHECK(std::fabs(PavlovOpticFov(6) - 11.25f) < 1e-4f);
        // As the control FOV with the shipped range (3..20) and base 2: 8x optics ~7x, the ACOG ~3.6x.
        CHECK(Magnification(PavlovOpticFov(35), 20.0f, 2.0f) > 6.5f && Magnification(PavlovOpticFov(6), 20.0f, 2.0f) < 4.0f);
    }
    std::puts("Physical optic magnification, eye relief and model identity checks passed");
}

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
#include "../L4D2VR/vr_scope_lens.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "Scope lens failed at %d\n", __LINE__); std::abort(); } } while(false)
static bool Near(float a, float b, float eps = 0.0001f) { return std::fabs(a - b) <= eps; }
int main()
{
    using namespace l4d2vr_scope_lens;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    LensGeometry lens{};
    lens.center = Vector(10.0f, 0.0f, 0.0f);
    lens.axis = Vector(2.0f, 0.0f, 0.0f); // unnormalised on purpose
    lens.up = Vector(0.0f, 0.0f, 1.0f);
    lens.radius = 1.0f;
    lens.eyeRelief = 3.0f;
    LensParams params{};
    params.parallaxK = 1.0f;
    params.eyeboxDeg = 8.0f;
    params.baseRadius = 1.0f;
    params.vignetteExponent = 0.25f;
    CHECK(Valid(lens) && Valid(params));

    // Centred eye at the nominal eye relief: no shift, full radius.
    const LensState centred = EvaluateLens(lens, Vector(7.0f, 0.0f, 0.0f), params);
    CHECK(centred.visible && centred.shiftU == 0.0f && centred.shiftV == 0.0f);
    CHECK(Near(centred.vignetteRadius, 1.0f) && Near(centred.distance, 3.0f) && Near(centred.offAxisDeg, 0.0f));
    CHECK(Near(VignetteAlphaAt(centred, 0.0f, 0.0f), 0.0f));
    CHECK(Near(VignetteAlphaAt(centred, 1.0f, 0.0f), 1.0f) && Near(VignetteAlphaAt(centred, 0.0f, -1.2f), 1.0f));
    CHECK(Near(VignetteAlphaAt(centred, 0.5f, 0.0f), 1.0f - std::pow(0.5f, 0.25f)));
    float texU = 0.0f, texV = 0.0f;
    SampleUV(centred, 0.0f, 0.0f, texU, texV);
    CHECK(Near(texU, 0.5f) && Near(texV, 0.5f));
    SampleUV(centred, 1.0f, 1.0f, texU, texV); // lens top-right -> texture top-right (v down)
    CHECK(Near(texU, 1.0f) && Near(texV, 0.0f));
    // Moving straight back along the axis keeps it centred.
    const LensState back = EvaluateLens(lens, Vector(2.0f, 0.0f, 0.0f), params);
    CHECK(back.visible && Near(back.vignetteRadius, 1.0f) && back.shiftU == 0.0f);

    // Eye to the scope's right (Source right = forward x up = -Y here).
    const LensState right = EvaluateLens(lens, Vector(7.0f, -0.3f, 0.0f), params);
    CHECK(right.visible && Near(right.offsetU, 0.3f) && Near(right.offsetV, 0.0f));
    CHECK(Near(right.shiftU, 0.1f) && Near(right.shiftV, 0.0f)); // k * offset / eyeRelief
    CHECK(Near(right.vignetteCenterU, -0.1f));                   // image moves away from the eye side
    const float rightAngle = std::atan2(0.3f, 3.0f) * 180.0f / kPi;
    CHECK(Near(right.offAxisDeg, rightAngle) && Near(right.vignetteRadius, 1.0f - rightAngle / 8.0f));
    CHECK(right.vignetteRadius < centred.vignetteRadius);
    CHECK(VignetteAlphaAt(right, -0.2f, 0.0f) < VignetteAlphaAt(right, 0.2f, 0.0f));
    CHECK(Near(VignetteAlphaAt(right, -0.1f, 0.0f), 0.0f));
    SampleUV(right, 0.0f, 0.0f, texU, texV);
    CHECK(texU > 0.5f && Near(texV, 0.5f));
    // Eye above: shift up, vignette centre down.
    const LensState above = EvaluateLens(lens, Vector(7.0f, 0.0f, 0.3f), params);
    CHECK(above.visible && Near(above.shiftV, 0.1f) && Near(above.shiftU, 0.0f) && Near(above.vignetteCenterV, -0.1f));
    SampleUV(above, 0.0f, 0.0f, texU, texV);
    CHECK(Near(texU, 0.5f) && texV < 0.5f);
    // Eye to the left: mirrored.
    const LensState left = EvaluateLens(lens, Vector(7.0f, 0.3f, 0.0f), params);
    CHECK(Near(left.shiftU, -0.1f) && Near(left.vignetteRadius, right.vignetteRadius));
    // Parallax gain scales the shift; eye relief divides it.
    LensParams strong = params;
    strong.parallaxK = 2.0f;
    CHECK(Near(EvaluateLens(lens, Vector(7.0f, -0.3f, 0.0f), strong).shiftU, 0.2f));
    LensGeometry longRelief = lens;
    longRelief.eyeRelief = 6.0f;
    CHECK(Near(EvaluateLens(longRelief, Vector(7.0f, -0.3f, 0.0f), params).shiftU, 0.05f));

    // Beyond the eyebox: black.
    const LensState outside = EvaluateLens(lens, Vector(7.0f, -0.5f, 0.0f), params);
    CHECK(!outside.visible && outside.vignetteRadius == 0.0f && outside.offAxisDeg > 8.0f);
    CHECK(VignetteAlphaAt(outside, 0.0f, 0.0f) == 1.0f);
    // Eye level with or in front of the lens: black.
    CHECK(!EvaluateLens(lens, Vector(10.0f, 0.0f, 0.0f), params).visible);
    CHECK(!EvaluateLens(lens, Vector(12.0f, 0.0f, 0.0f), params).visible);
    // Degenerate input: black.
    CHECK(!EvaluateLens(lens, Vector(nan, 0.0f, 0.0f), params).visible);
    LensGeometry broken = lens;
    broken.up = Vector(1.0f, 0.0f, 0.0f); // parallel to the axis
    CHECK(!Valid(broken) && !EvaluateLens(broken, Vector(7.0f, 0.0f, 0.0f), params).visible);
    broken = lens;
    broken.eyeRelief = 0.0f;
    CHECK(!EvaluateLens(broken, Vector(7.0f, 0.0f, 0.0f), params).visible);
    LensParams badParams = params;
    badParams.eyeboxDeg = 0.0f;
    CHECK(!EvaluateLens(lens, Vector(7.0f, 0.0f, 0.0f), badParams).visible);

    // Falloff helpers.
    CHECK(Near(EyeboxRadius(0.0f, 8.0f, 0.9f), 0.9f) && Near(EyeboxRadius(4.0f, 8.0f, 1.0f), 0.5f));
    CHECK(EyeboxRadius(20.0f, 8.0f, 1.0f) == 0.0f && EyeboxRadius(nan, 8.0f, 1.0f) == 0.0f);
    CHECK(Near(VignetteTransmission(0.0f, 1.0f, 3.0f), 1.0f) && VignetteTransmission(1.0f, 1.0f, 3.0f) == 0.0f);
    CHECK(VignetteTransmission(0.5f, 1.0f, 0.25f) > VignetteTransmission(0.5f, 1.0f, 2.0f)); // smaller = harder edge
    CHECK(VignetteAlpha(0.2f, 0.0f, 1.0f) == 1.0f && VignetteAlpha(nan, 1.0f, 1.0f) == 1.0f);

    // Render-target cadence.
    CHECK(ShouldRenderRttThisFrame(0u, false) && ShouldRenderRttThisFrame(1u, false));
    CHECK(ShouldRenderRttThisFrame(0u, true) && !ShouldRenderRttThisFrame(1u, true) && ShouldRenderRttThisFrame(2u, true));

    // Magnified field of view.
    CHECK(Near(MagnifiedFov(90.0f, 1.0f), 90.0f) && Near(MagnifiedFov(90.0f, 0.5f), 90.0f));
    CHECK(Near(MagnifiedFov(90.0f, 2.0f), 2.0f * std::atan(0.5f) * 180.0f / kPi, 0.001f));
    CHECK(MagnifiedFov(90.0f, 8.0f) < MagnifiedFov(90.0f, 4.0f));
    CHECK(Near(MagnifiedFov(90.0f, nan), 90.0f));

    // Zoom cycling.
    ZoomCycler zoom;
    CHECK(zoom.Count() == 0u && zoom.Current() == 1.0f && zoom.Next() == 1.0f);
    const float levels[] = {2.0f, 4.0f, 8.0f};
    CHECK(zoom.SetLevels(levels, 3u) && zoom.Current() == 2.0f);
    CHECK(zoom.Next() == 4.0f && zoom.Next() == 8.0f && zoom.Next() == 2.0f);
    CHECK(zoom.Previous() == 8.0f && zoom.Previous() == 4.0f);
    CHECK(zoom.Next(false) == 8.0f && zoom.Next(false) == 8.0f);
    zoom.Reset();
    CHECK(zoom.Index() == 0u && zoom.Previous(false) == 2.0f);
    const float badLevels[] = {2.0f, 0.5f};
    CHECK(!zoom.SetLevels(badLevels, 2u) && zoom.Count() == 3u);
    CHECK(!zoom.SetLevels(levels, 0u) && !zoom.SetLevels(nullptr, 1u));
    std::puts("vr_scope_lens tests passed");
    return 0;
}

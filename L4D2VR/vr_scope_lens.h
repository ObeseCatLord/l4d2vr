#pragma once
#include "sdk/vector.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Picture-in-picture scope lens math (HL2VR style). The scope view is rendered
// to a texture and drawn on the lens; these helpers produce the per-eye shader
// constants: parallax UV shift, eyebox-limited vignette radius/centre and the
// vignette falloff, plus render-rate, FOV and zoom-cycling helpers.
//
// Lens space: origin at the lens centre, u along the scope's right, v along
// its up, both normalised so the lens rim is at radius 1. Texture space is
// uv = 0.5 + 0.5 * (u, -v) (v grows downward).
namespace l4d2vr_scope_lens
{
    inline constexpr float kPi = 3.14159265358979323846f;

    inline bool Finite(const Vector& v)
    { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
    inline float Saturate(float x) { return std::isfinite(x) ? std::clamp(x, 0.0f, 1.0f) : 0.0f; }

    struct LensGeometry
    {
        Vector center{0.0f, 0.0f, 0.0f};
        Vector axis{1.0f, 0.0f, 0.0f}; // optical axis, eyepiece -> objective (points away from the eye)
        Vector up{0.0f, 0.0f, 1.0f};   // scope up; re-orthogonalised against axis
        float radius = 1.0f;           // lens radius (world units)
        float eyeRelief = 3.0f;        // nominal eye distance behind the lens (world units)
    };

    struct LensParams
    {
        float parallaxK = 1.0f;         // UV shift gain (lens radii per unit offset/eyeRelief)
        float eyeboxDeg = 8.0f;         // off-axis angle at which the image closes completely
        float baseRadius = 1.0f;        // vignette radius on axis, in lens radii
        float vignetteExponent = 0.25f; // falloff exponent; smaller = harder edge
    };

    struct LensState
    {
        float offsetU = 0.0f, offsetV = 0.0f; // eye offset from the axis (world units, right/up)
        float distance = 0.0f;                // eye distance behind the lens along the axis
        float offAxisDeg = 0.0f;              // angle between the axis and the eye->lens-centre line
        float shiftU = 0.0f, shiftV = 0.0f;   // parallax shift (lens radii, right/up); sample = p + shift
        float vignetteCenterU = 0.0f, vignetteCenterV = 0.0f; // = -shift: the image moves away from the eye side
        float vignetteRadius = 0.0f;          // lens radii; 0 => black
        float exponent = 0.25f;
        bool visible = false;                 // false => draw the lens black
    };

    inline bool Basis(const LensGeometry& lens, Vector& forward, Vector& right, Vector& up)
    {
        if (!Finite(lens.axis) || !Finite(lens.up)) return false;
        const float axisLength = VectorLength(lens.axis);
        if (!std::isfinite(axisLength) || !(axisLength > 0.0001f)) return false;
        forward = lens.axis * (1.0f / axisLength);
        right = CrossProduct(forward, lens.up); // Source: right = forward x up
        const float rightLength = VectorLength(right);
        if (!std::isfinite(rightLength) || !(rightLength > 0.0001f)) return false;
        right *= 1.0f / rightLength;
        up = CrossProduct(right, forward);
        return Finite(up);
    }

    inline bool Valid(const LensGeometry& lens)
    {
        Vector f(0.0f, 0.0f, 0.0f), r(0.0f, 0.0f, 0.0f), u(0.0f, 0.0f, 0.0f);
        return Finite(lens.center) && std::isfinite(lens.radius) && lens.radius > 0.0f &&
            std::isfinite(lens.eyeRelief) && lens.eyeRelief > 0.0f && Basis(lens, f, r, u);
    }

    inline bool Valid(const LensParams& p)
    {
        return std::isfinite(p.parallaxK) && std::isfinite(p.eyeboxDeg) && p.eyeboxDeg > 0.0f &&
            std::isfinite(p.baseRadius) && p.baseRadius >= 0.0f && std::isfinite(p.vignetteExponent) &&
            p.vignetteExponent > 0.0f;
    }

    // r0 * (1 - clamp(offAxis / eyebox)).
    inline float EyeboxRadius(float offAxisDeg, float eyeboxDeg, float baseRadius)
    {
        if (!std::isfinite(offAxisDeg) || !std::isfinite(eyeboxDeg) || !(eyeboxDeg > 0.0f) ||
            !std::isfinite(baseRadius) || baseRadius <= 0.0f)
            return 0.0f;
        return baseRadius * (1.0f - Saturate(offAxisDeg / eyeboxDeg));
    }

    // Image transmission at distance r from the vignette centre:
    // pow(saturate(1 - r/radius), exponent): 1 at the centre, 0 at and past the rim.
    inline float VignetteTransmission(float r, float radius, float exponent)
    {
        if (!std::isfinite(r) || !std::isfinite(radius) || !(radius > 0.0f) || !std::isfinite(exponent) ||
            !(exponent > 0.0f))
            return 0.0f;
        return std::pow(Saturate(1.0f - r / radius), exponent);
    }

    // Opacity of the black vignette overlay (inverse of transmission).
    inline float VignetteAlpha(float r, float radius, float exponent)
    { return 1.0f - VignetteTransmission(r, radius, exponent); }

    inline LensState EvaluateLens(const LensGeometry& lens, const Vector& eye, const LensParams& params = LensParams{})
    {
        LensState state{};
        state.exponent = params.vignetteExponent;
        Vector forward(0.0f, 0.0f, 0.0f), right(0.0f, 0.0f, 0.0f), up(0.0f, 0.0f, 0.0f);
        if (!Finite(eye) || !Valid(lens) || !Valid(params) || !Basis(lens, forward, right, up)) return state;
        const Vector delta = eye - lens.center;
        const float along = DotProduct(delta, forward);
        state.distance = -along;
        state.offsetU = DotProduct(delta, right);
        state.offsetV = DotProduct(delta, up);
        if (!std::isfinite(state.offsetU) || !std::isfinite(state.offsetV) || !std::isfinite(along)) return state;
        const float perpendicular = std::sqrt(state.offsetU * state.offsetU + state.offsetV * state.offsetV);
        state.offAxisDeg = std::atan2(perpendicular, state.distance) * (180.0f / kPi);
        // k * offset / eyeRelief: dimensionless, expressed in lens radii.
        state.shiftU = params.parallaxK * state.offsetU / lens.eyeRelief;
        state.shiftV = params.parallaxK * state.offsetV / lens.eyeRelief;
        state.vignetteCenterU = -state.shiftU;
        state.vignetteCenterV = -state.shiftV;
        if (!(state.distance > 0.0f)) return state; // eye level with or in front of the lens
        state.vignetteRadius = EyeboxRadius(state.offAxisDeg, params.eyeboxDeg, params.baseRadius);
        state.visible = state.vignetteRadius > 0.0f;
        return state;
    }

    // Black-overlay opacity at lens-space point (u, v).
    inline float VignetteAlphaAt(const LensState& state, float u, float v)
    {
        if (!state.visible) return 1.0f;
        const float du = u - state.vignetteCenterU, dv = v - state.vignetteCenterV;
        return VignetteAlpha(std::sqrt(du * du + dv * dv), state.vignetteRadius, state.exponent);
    }

    // Texture coordinate to sample for lens-space point (u, v).
    inline void SampleUV(const LensState& state, float u, float v, float& texU, float& texV)
    {
        texU = 0.5f + 0.5f * (u + state.shiftU);
        texV = 0.5f - 0.5f * (v + state.shiftV);
    }

    // Render-target refresh: every frame, or every other frame at half rate.
    inline bool ShouldRenderRttThisFrame(std::uint64_t frameIndex, bool halfRate)
    { return !halfRate || (frameIndex & 1u) == 0u; }

    // Field of view through a scope of the given magnification:
    // 2 * atan(tan(base/2) / magnification). Magnification below 1 is treated as 1.
    inline float MagnifiedFov(float baseFovDeg, float magnification)
    {
        if (!std::isfinite(baseFovDeg) || !(baseFovDeg > 0.0f) || !(baseFovDeg < 180.0f)) return baseFovDeg;
        if (!std::isfinite(magnification) || magnification <= 1.0f) return baseFovDeg;
        const float half = std::tan(baseFovDeg * 0.5f * (kPi / 180.0f)) / magnification;
        return 2.0f * std::atan(half) * (180.0f / kPi);
    }

    // Cycles through a fixed list of magnification levels (e.g. 2x/4x/8x).
    class ZoomCycler
    {
    public:
        static constexpr std::size_t kMaxLevels = 8;

        // Levels must be finite and >= 1; returns false and keeps the old list otherwise.
        bool SetLevels(const float* levels, std::size_t count)
        {
            if (!levels || count == 0 || count > kMaxLevels) return false;
            for (std::size_t i = 0; i < count; ++i)
                if (!std::isfinite(levels[i]) || levels[i] < 1.0f) return false;
            std::copy(levels, levels + count, m_Levels.begin());
            m_Count = count;
            m_Index = 0;
            return true;
        }
        std::size_t Count() const { return m_Count; }
        std::size_t Index() const { return m_Index; }
        float Current() const { return m_Count ? m_Levels[m_Index] : 1.0f; }
        // Advances (wrapping when `wrap`, else clamping) and returns the new level.
        float Next(bool wrap = true)
        {
            if (m_Count) m_Index = m_Index + 1 < m_Count ? m_Index + 1 : (wrap ? 0 : m_Count - 1);
            return Current();
        }
        float Previous(bool wrap = true)
        {
            if (m_Count) m_Index = m_Index > 0 ? m_Index - 1 : (wrap ? m_Count - 1 : 0);
            return Current();
        }
        void Reset() { m_Index = 0; }

    private:
        std::array<float, kMaxLevels> m_Levels{};
        std::size_t m_Count = 0;
        std::size_t m_Index = 0;
    };
}

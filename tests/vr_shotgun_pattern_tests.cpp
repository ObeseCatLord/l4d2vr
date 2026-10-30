#include "../L4D2VR/vr_shotgun_pattern.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static void CheckImpl(bool result, int line, const char* what)
{
    if (!result) { std::fprintf(stderr, "Check failed at line %d: %s\n", line, what); std::abort(); }
}
#define Check(result) CheckImpl((result), __LINE__, #result)

using namespace l4d2vr_shotgun_pattern;

// Replays L4D2's pellet loop (CTerrorPlayer::FireBullet) with the overridden
// draws and returns each pellet's (right, up) offset in degrees.
static void Loop(const Weapon& w, int pellets, float* right, float* up)
{
    ShotContext shot{};
    shot.weapon = &w;
    const float ratio = w.scatterPitch / w.scatterYaw;
    for (int n = 1; n <= pellets; ++n)
    {
        right[n - 1] = up[n - 1] = 0.0f;
        if (n == 1) continue; // first pellet: no draws
        float r = 0.0f, half = 0.0f;
        Check(Override(shot, kRadiusName, -w.scatterYaw, w.scatterYaw, n, r));
        Check(Override(shot, kDirectionName, 0.0f, 180.0f, n, half));
        Check(half >= 0.0f && half < 180.0f && r >= 0.0f && r <= w.scatterYaw);
        const float theta = 2.0f * half * kPi / 180.0f;
        const float pitch = ratio * r * std::sin(theta), yaw = r * std::cos(theta);
        up[n - 1] = -pitch;
        right[n - 1] = -yaw;
    }
}

int main()
{
    for (int id : { 3, 8, 4, 11 })
    {
        const Weapon* w = WeaponFor(id);
        Check(w != nullptr && w->pellets >= 8);
        float right[16], up[16], again[16], againUp[16];
        Loop(*w, w->pellets, right, up);
        Loop(*w, w->pellets, again, againUp);
        Check(right[0] == 0.0f && up[0] == 0.0f);
        float meanRadius = 0.0f, maxYaw = 0.0f, maxPitch = 0.0f, nearest = 1e9f;
        for (int i = 1; i < w->pellets; ++i)
        {
            // fixed: the same every shot, and the same as PelletOffsetDeg
            Check(right[i] == again[i] && up[i] == againUp[i]);
            float er = 0.0f, eu = 0.0f;
            PelletOffsetDeg(*w, i, er, eu);
            Check(std::fabs(right[i] - er) < 1e-3f && std::fabs(up[i] - eu) < 1e-3f);
            // inside vanilla's ellipse (S yaw x P pitch)
            const float e = (right[i] / w->scatterYaw) * (right[i] / w->scatterYaw) + (up[i] / w->scatterPitch) * (up[i] / w->scatterPitch);
            Check(e <= 1.0f + 1e-4f);
            meanRadius += std::sqrt(e);
            maxYaw = std::fmax(maxYaw, std::fabs(right[i]));
            maxPitch = std::fmax(maxPitch, std::fabs(up[i]));
            for (int k = 0; k < i; ++k)
                nearest = std::fmin(nearest, std::hypot((right[i] - right[k]) / w->scatterYaw, (up[i] - up[k]) / w->scatterPitch));
        }
        meanRadius /= static_cast<float>(w->pellets);
        // over all pellets (centre included) the mean normalised radius is
        // vanilla's 0.5 (|r| uniform), within the centre pellet's share
        Check(std::fabs(meanRadius - 0.5f) < 0.06f);
        // the pattern reaches out toward vanilla's extents ...
        Check(maxYaw > 0.6f * w->scatterYaw && maxPitch > 0.6f * w->scatterPitch);
        // ... and no two pellets clump
        Check(nearest > 0.09f);
    }
    // the stock pellet counts
    Check(WeaponFor(3)->pellets == 10 && WeaponFor(8)->pellets == 8 && WeaponFor(4)->pellets == 11 && WeaponFor(11)->pellets == 9);
    Check(WeaponFor(5) == nullptr);

    // A modified weapon script (other scatter) keeps the vanilla draws.
    ShotContext shot{};
    shot.weapon = WeaponFor(3);
    float value = 0.0f;
    Check(!Override(shot, kRadiusName, -9.0f, 9.0f, 2, value));
    Check(!Override(shot, kDirectionName, 0.0f, 180.0f, 2, value));
    // Other draws and the first pellet are never touched.
    Check(!Override(shot, "CTerrorGun::FireBullet HorizSpread", -1.0f, 1.0f, 2, value));
    Check(!Override(shot, kRadiusName, -7.5f, 7.5f, 1, value));
    ShotContext none{};
    Check(!Override(none, kRadiusName, -7.5f, 7.5f, 2, value));
    std::puts("vr_shotgun_pattern_tests passed");
    return 0;
}

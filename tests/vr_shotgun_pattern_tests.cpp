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
static void Loop(const Weapon& w, int pellets, std::uint32_t start, float* right, float* up)
{
    ShotContext shot{};
    shot.weapon = &w;
    const float ratio = w.scatterPitch / w.scatterYaw;
    for (int n = 1; n <= pellets; ++n)
    {
        right[n - 1] = up[n - 1] = 0.0f;
        if (n == 1) continue; // first pellet: no draws
        float r = 0.0f, half = 0.0f;
        Check(Override(shot, kRadiusName, -w.scatterYaw, w.scatterYaw, n, [&]() { return static_cast<int>(start); }, r));
        Check(Override(shot, kDirectionName, 0.0f, 180.0f, n, [&]() { return 0; }, half));
        Check(half >= 0.0f && half < 180.0f && r >= 0.0f);
        const float theta = 2.0f * half * kPi / 180.0f;
        const float pitch = ratio * r * std::sin(theta), yaw = r * std::cos(theta);
        up[n - 1] = -pitch;
        right[n - 1] = -yaw;
    }
}

int main()
{
    // Pellet 0 is centred; the rest match Pavlov's example (n0 = 0).
    float r = 0.0f, u = 0.0f;
    PelletOffsetDeg(0, 0u, 20.0f, r, u);
    Check(r == 0.0f && u == 0.0f);
    PelletOffsetDeg(1, 0u, 20.0f, r, u);
    Check(std::fabs(r - 0.0f) < 0.01f && std::fabs(u - 0.0f) < 0.01f);
    PelletOffsetDeg(2, 0u, 20.0f, r, u);
    Check(std::fabs(r + 0.70f) < 0.02f && std::fabs(u + 1.23f) < 0.02f);
    PelletOffsetDeg(3, 0u, 20.0f, r, u);
    Check(std::fabs(r + 1.40f) < 0.02f && std::fabs(u - 0.40f) < 0.02f);

    for (int id : { 3, 8, 4, 11 })
    {
        const Weapon* w = WeaponFor(id);
        Check(w != nullptr);
        const float half = std::atan2(0.5f, w->evenSpread) * 180.0f / kPi;
        for (std::uint32_t start : { 0u, 17u, 32767u })
        {
            float right[16], up[16];
            Loop(*w, 11, start, right, up);
            Check(right[0] == 0.0f && up[0] == 0.0f);
            for (int i = 1; i < 11; ++i)
            {
                float er = 0.0f, eu = 0.0f;
                PelletOffsetDeg(i, start, w->evenSpread, er, eu);
                Check(std::fabs(right[i] - er) < 1e-3f && std::fabs(up[i] - eu) < 1e-3f);
                Check(std::fabs(right[i]) <= half + 1e-3f && std::fabs(up[i]) <= half + 1e-3f);
            }
        }
    }
    Check(WeaponFor(5) == nullptr);

    // A modified weapon script (other scatter) keeps the vanilla draws.
    ShotContext shot{};
    shot.weapon = WeaponFor(3);
    float value = 0.0f;
    Check(!Override(shot, kRadiusName, -9.0f, 9.0f, 2, []() { return 0; }, value));
    Check(!Override(shot, kDirectionName, 0.0f, 180.0f, 2, []() { return 0; }, value));
    // Other draws and the first pellet are never touched.
    Check(!Override(shot, "CTerrorGun::FireBullet HorizSpread", -1.0f, 1.0f, 2, []() { return 0; }, value));
    Check(!Override(shot, kRadiusName, -7.5f, 7.5f, 1, []() { return 0; }, value));
    ShotContext none{};
    Check(!Override(none, kRadiusName, -7.5f, 7.5f, 2, []() { return 0; }, value));
    std::puts("vr_shotgun_pattern_tests passed");
    return 0;
}

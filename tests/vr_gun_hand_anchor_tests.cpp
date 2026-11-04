#include "../L4D2VR/vr_gun_hand_anchor.h"
#include <cmath>
#include <cstdio>

using namespace l4d2vr_gun_hand_anchor;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool Near(const Vec3& a, const Vec3& b, float eps = 1e-4f)
{
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

int main()
{
    // The hand pose offset is (right, up, back): 10 cm back, 8 cm down, 3 cm left
    // is (forward -0.10, right -0.03, up -0.08), not (forward -0.03, ...).
    CHECK(Near(HandOffsetToForwardRightUp(Vec3{ -0.03f, -0.08f, 0.10f }), Vec3{ -0.10f, -0.03f, -0.08f }));
    CHECK(Near(HandOffsetToForwardRightUp(Vec3{}), Vec3{}));

    // Local <-> world round trip in a rotated frame (yaw 90: forward = +y).
    const Vec3 f{ 0, 1, 0 }, r{ 1, 0, 0 }, u{ 0, 0, 1 };
    const Vec3 local{ 2, 3, 4 };
    CHECK(Near(LocalToWorld(local, f, r, u), Vec3{ 3, 2, 4 }));
    CHECK(Near(WorldToLocal(LocalToWorld(local, f, r, u), f, r, u), local));

    // First measurement takes the whole residual; then it converges and stays.
    Anchor a{};
    CHECK(Correct(a, Vec3{ 4, -2, 1 }, 0.5f, 25.0f) && a.valid && Near(a.local, Vec3{ 4, -2, 1 }));
    // Simulate the loop: the drawn hand sits at (base + applied), target at 0,
    // with the measured frame using the translation from one or two frames ago.
    for (int delay = 0; delay <= 2; ++delay)
    {
        const Vec3 base{ -6, 3, -2 };   // hand relative to its target without translation
        Anchor x{};
        Vec3 history[4]{};
        for (int frame = 0; frame < 60; ++frame)
        {
            const Vec3 applied = history[delay];
            const Vec3 residual = Scale(Add(base, applied), -1.0f);
            Correct(x, residual, 0.5f, 25.0f);
            for (int i = 3; i > 0; --i) history[i] = history[i - 1];
            history[0] = x.local;
        }
        CHECK(Near(x.local, Vec3{ 6, -3, 2 }, 1e-2f));
    }
    // Never more than the limit; a bad residual changes nothing.
    Anchor b{};
    CHECK(!Correct(b, Vec3{ 30, 0, 0 }, 0.5f, 25.0f) && !b.valid);
    CHECK(!Correct(b, Vec3{ NAN, 0, 0 }, 0.5f, 25.0f) && !b.valid);
    CHECK(Correct(b, Vec3{ 20, 0, 0 }, 0.5f, 25.0f));
    CHECK(!Correct(b, Vec3{ 20, 0, 0 }, 0.5f, 25.0f) && Near(b.local, Vec3{ 20, 0, 0 }));

    // Only guns in a hand, in first person VR, with the option on.
    Gate g{};
    CHECK(Applies(g));
    Gate melee = g; melee.isGun = false; CHECK(!Applies(melee));
    Gate sling = g; sling.chestSling = true; CHECK(!Applies(sling));
    Gate empty = g; empty.emptyHands = true; CHECK(!Applies(empty));
    Gate off = g; off.enabled = false; CHECK(!Applies(off));
    Gate desk = g; desk.vrEnabled = false; CHECK(!Applies(desk));
    Gate mouse = g; mouse.mouseMode = true; CHECK(!Applies(mouse));
    Gate third = g; third.thirdPerson = true; CHECK(!Applies(third));

    if (g_failures == 0)
        std::printf("vr_gun_hand_anchor: all tests passed\n");
    return g_failures == 0 ? 0 : 1;
}

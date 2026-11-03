#include "../L4D2VR/vr_grenade_arc.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace l4d2vr_grenade_arc;

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

int main()
{
    const V3 eye(0.0f, 0.0f, 64.0f);
    // Level throw: 750 u/s along the aim from 16 units ahead of the eye,
    // gravity 0.4 x 800 = 320 u/s^2.
    Launch l = NativeLaunch(eye, V3(2.0f, 0.0f, 0.0f), V3());
    Require(l.valid, "level launch");
    Require(Near(l.origin.x, 16.0f, 1e-4f) && Near(l.origin.z, 64.0f, 1e-4f), "spawns 16 units ahead of the eye");
    Require(Near(l.velocity.x, 750.0f, 1e-3f) && Near(l.velocity.z, 0.0f, 1e-4f), "750 u/s along the aim");
    Require(Near(l.gravity, 320.0f, 1e-3f), "grenade gravity 0.4 x sv_gravity");
    // Lands on z = 0 after sqrt(2 * 64 / 320) s.
    const float t = TimeToGround(l, 0.0f);
    Require(Near(t, std::sqrt(2.0f * 64.0f / 320.0f), 1e-4f), "level landing time");
    Require(Near(PositionAt(l, t).z, 0.0f, 1e-3f) && Near(PositionAt(l, t).x, 16.0f + 750.0f * t, 1e-2f), "level landing point");

    // 45 degrees up: the classic range v^2 / g from launch height (plus the
    // extra fall to the ground) - about 1758 + 16 units.
    const float s = std::sqrt(0.5f);
    l = NativeLaunch(eye, V3(s, 0.0f, s), V3());
    const float t45 = TimeToGround(l, l.origin.z);
    Require(Near(PositionAt(l, t45).x - l.origin.x, 750.0f * 750.0f / 320.0f, 0.5f), "45 degree range on the launch plane");
    Require(TimeToGround(l, 0.0f) > t45, "landing below the launch plane comes later");

    // The body's own motion carries into the throw.
    l = NativeLaunch(eye, V3(1.0f, 0.0f, 0.0f), V3(0.0f, 200.0f, 0.0f));
    Require(Near(l.velocity.y, 200.0f, 1e-4f), "running adds its velocity");

    // The drawn line starts at the hand and joins the true path.
    const V3 hand(10.0f, -15.0f, 40.0f);
    l = NativeLaunch(eye, V3(1.0f, 0.0f, 0.3f), V3());
    const V3 d0 = DrawnPointAt(l, hand, 0.0f, 0.12f);
    Require(Near(d0.x, hand.x, 1e-4f) && Near(d0.y, hand.y, 1e-4f) && Near(d0.z, hand.z, 1e-4f), "the drawn arc starts at the hand");
    const V3 d1 = DrawnPointAt(l, hand, 0.12f, 0.12f), p1 = PositionAt(l, 0.12f);
    Require(Near(d1.x, p1.x, 1e-4f) && Near(d1.z, p1.z, 1e-4f), "and is the true path from the blend on");
    const float tg = TimeToGround(l, 0.0f);
    const V3 dg = DrawnPointAt(l, hand, tg, 0.12f), pg = PositionAt(l, tg);
    Require(Near(dg.x, pg.x, 1e-4f) && Near(dg.y, pg.y, 1e-4f), "so it lands where the grenade lands");

    // Invalid input.
    Require(!NativeLaunch(eye, V3(), V3()).valid, "no aim");
    Require(!NativeLaunch(V3(std::nanf(""), 0.0f, 0.0f), V3(1.0f, 0.0f, 0.0f), V3()).valid, "NaN eye");
    Require(!NativeLaunch(eye, V3(1.0f, 0.0f, 0.0f), V3(), 0.0f).valid, "no gravity");
    l = NativeLaunch(eye, V3(0.0f, 0.0f, 1.0f), V3());
    Require(TimeToGround(l, 1000.0f) < 0.0f, "a ceiling above the apex is never reached");
    std::printf("vr_grenade_arc_tests: %d checks passed\n", checks);
    return 0;
}

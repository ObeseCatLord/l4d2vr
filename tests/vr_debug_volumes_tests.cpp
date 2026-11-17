// 2.0.12: the local debug view of every interaction volume (vr_debug_volumes.h).
#include "../L4D2VR/vr_debug_volumes.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

static int g_checks = 0;
static void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) { std::printf("FAILED: %s\n", what); std::exit(1); }
}

struct V
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    V() = default;
    V(float a, float b, float c) : x(a), y(b), z(c) {}
    V operator+(const V& o) const { return V(x + o.x, y + o.y, z + o.z); }
    V operator-(const V& o) const { return V(x - o.x, y - o.y, z - o.z); }
    V operator*(float s) const { return V(x * s, y * s, z * s); }
};

static float Len(const V& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

int main()
{
    namespace dv = l4d2vr_debug_volumes;
    using Line = dv::Line<V>;
    // Every volume of the review table has a name and a distinct idle tint;
    // the state colours are shared.
    std::set<std::uint32_t> tints;
    for (int k = 0; k < dv::kKindCount; ++k)
    {
        const auto kind = static_cast<dv::Kind>(k);
        Check(dv::KindName(kind) != nullptr && std::strlen(dv::KindName(kind)) > 0, "every kind is named");
        tints.insert(dv::Color(kind, dv::State::Idle));
        Check(dv::Color(kind, dv::State::Inside) == 0xFF30E030u && dv::Color(kind, dv::State::WouldTake) == 0xFFFFE020u &&
            dv::Color(kind, dv::State::Refused) == 0xFFFF3030u, "state colours");
    }
    Check(static_cast<int>(tints.size()) == dv::kKindCount, "idle tints tell the types apart");
    Check(dv::kKindCount >= 21, "the review's volume list (20 rows + the hand sphere)");

    // Off: nothing is emitted, whatever is asked.
    {
        std::vector<Line> out;
        dv::Builder<V> b(out, false);
        b.Sphere(dv::Kind::HolsterZone, dv::State::Idle, V(0, 0, 0), 5.0f);
        b.Box(dv::Kind::MagWell, dv::State::Idle, V(), V(1, 0, 0), V(0, 1, 0), V(0, 0, 1), V(-1, -1, -1), V(1, 1, 1));
        b.Capsule(dv::Kind::HolsterZone, dv::State::Idle, V(), V(0, 0, 10), 2.0f);
        Check(out.empty() && b.KindsDrawn() == 0u, "disabled builder draws nothing");
    }
    // On: shapes have the right geometry.
    {
        std::vector<Line> out;
        dv::Builder<V> b(out, true);
        b.Box(dv::Kind::MagWell, dv::State::Idle, V(), V(1, 0, 0), V(0, 1, 0), V(0, 0, 1), V(-1, -2, -3), V(1, 2, 3), true);
        Check(out.size() == 12u && out[0].viewmodelLayer, "a box is 12 edges in the viewmodel layer");
        out.clear();
        b.Sphere(dv::Kind::HandSphere, dv::State::Inside, V(10, 0, 0), 4.6f, 16);
        Check(out.size() == 48u, "a sphere is three 16-segment circles");
        for (const Line& l : out)
            Check(std::fabs(Len(l.a - V(10, 0, 0)) - 4.6f) < 1e-3f && l.argb == 0xFF30E030u, "on the sphere, green");
        out.clear();
        b.Capsule(dv::Kind::HolsterZone, dv::State::Idle, V(0, 0, 0), V(0, 0, 10), 2.0f, 8);
        Check(out.size() == 4u * 8u + 4u, "capsule: 4 circles + 4 sides");
        out.clear();
        b.Cone(dv::Kind::ScopeEyebox, dv::State::Idle, V(), V(1, 0, 0), 10.0f, 0.25f, 12);
        Check(out.size() == 12u + 4u, "cone");
        out.clear();
        b.Segment(dv::Kind::SightLine, dv::State::Idle, V(0, 0, 0), V(NAN, 0, 0));
        Check(out.empty(), "non-finite lines are dropped");
        // Every kind can be drawn and is reported.
        for (int k = 0; k < dv::kKindCount; ++k)
            b.Cross(static_cast<dv::Kind>(k), dv::State::Idle, V(1, 2, 3), 1.0f);
        Check(b.KindsDrawn() == (1u << dv::kKindCount) - 1u, "every kind has a registered draw");
    }
    // The line cap holds.
    {
        std::vector<Line> out;
        dv::Builder<V> b(out, true);
        for (int i = 0; i < 1000; ++i)
            b.Sphere(dv::Kind::HolsterZone, dv::State::Idle, V(), 1.0f, 20);
        Check(out.size() == dv::Builder<V>::kMaxLines, "capped");
    }
    std::printf("vr_debug_volumes_tests: %d checks passed\n", g_checks);
    return 0;
}

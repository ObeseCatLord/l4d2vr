#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// 2.0.12 (review s12 "Interaction volumes to show in a local debug view"):
// DebugInteractionVolumes draws every volume a hand can interact with as a
// wire outline, colour-coded by type and state, in the mod's own eye pass
// (VR::DrawVrHandsForEyeImmediate -> VrHandSystem::DrawDebugLinesForEye): it
// is drawn into the local headset's eye textures only, never through the
// engine debug overlay, a world entity or anything a server or listen host
// shares, so no other player can see it.
//
// This header is the std-only part: the volume kinds, their colours and the
// wire shapes (box, sphere, capsule, cone, cross), unit-tested in
// tests/vr_debug_volumes_tests.cpp. The builder that knows where each volume
// is lives in vr_hand_vr_bridge.cpp (VR::BuildDebugInteractionVolumes).
// Vec is any type with x, y, z, operator+, operator- and operator*(float)
// (Source's Vector in the DLL).
namespace l4d2vr_debug_volumes
{
    enum class Kind : int
    {
        HolsterZone = 0,   // slot1-5 handle volumes (sling capsule, hip capsules, pockets, back sphere)
        ItemBody,          // items hanging on the body (oriented box + grab margin)
        SlingCatch,        // the long gun's put-away catch (ChestSlingDropDistanceMeters)
        Pouch,             // magazine / shell pouch (reach sphere and pickup box)
        MagWell,           // magazine well / shell port (socket and capture boxes)
        Bolt,              // bolt / slide / pump handle box
        Pump,              // manual pump fore-end travel
        Foregrip,          // two-hand grip box (Pavlov per-class box)
        HeldSpare,         // fist / pinch anchors of the drawn hands
        DualReload,        // dual-pistol reload wells and slides
        MagSlap,           // magazine slap target (the seated magazine)
        DroppedMag,        // dropped magazines (visual only)
        Pickup,            // world grip pickup volume of each hand
        ObjectPull,        // object pull ray and target
        Consumable,        // pills mouth sphere, adrenaline chest/arm targets
        MeleeBlade,        // melee blade segment
        BashTip,           // gun bash tip
        ScopeEyebox,       // scope lens disc and eye-box cone
        ThrowRelease,      // hand-throw release point and arc
        HandSphere,        // each hand's 11.5 cm grab sphere and grab point
        SightLine,         // the aim ray (folds in the old sight debug)
        Count
    };

    constexpr int kKindCount = static_cast<int>(Kind::Count);

    enum class State : int
    {
        Idle = 0,     // nothing near
        Inside,       // a hand is inside it
        WouldTake,    // a press now would take / use it
        Refused,      // a hand is inside but the press would be refused (e.g. the hand is not empty)
    };

    inline const char* KindName(Kind kind)
    {
        switch (kind)
        {
        case Kind::HolsterZone: return "holster";
        case Kind::ItemBody: return "item body";
        case Kind::SlingCatch: return "sling catch";
        case Kind::Pouch: return "pouch";
        case Kind::MagWell: return "magazine well";
        case Kind::Bolt: return "bolt/slide/pump";
        case Kind::Pump: return "pump travel";
        case Kind::Foregrip: return "foregrip";
        case Kind::HeldSpare: return "held spare";
        case Kind::DualReload: return "dual reload";
        case Kind::MagSlap: return "magazine slap";
        case Kind::DroppedMag: return "dropped magazine";
        case Kind::Pickup: return "pickup";
        case Kind::ObjectPull: return "object pull";
        case Kind::Consumable: return "consumable";
        case Kind::MeleeBlade: return "melee blade";
        case Kind::BashTip: return "bash tip";
        case Kind::ScopeEyebox: return "scope eyebox";
        case Kind::ThrowRelease: return "throw release";
        case Kind::HandSphere: return "hand sphere";
        case Kind::SightLine: return "sight line";
        default: return nullptr;
        }
    }

    // ARGB. Idle volumes keep a dim per-type tint so the types tell apart;
    // the state colours are the same for every type (green inside, yellow
    // would take, red refused).
    inline std::uint32_t Color(Kind kind, State state)
    {
        switch (state)
        {
        case State::Inside: return 0xFF30E030u;
        case State::WouldTake: return 0xFFFFE020u;
        case State::Refused: return 0xFFFF3030u;
        default: break;
        }
        switch (kind)
        {
        case Kind::HolsterZone: return 0xFFA0A0A0u;     // grey
        case Kind::ItemBody: return 0xFF7090B0u;        // steel blue
        case Kind::SlingCatch: return 0xFF505870u;      // dark slate
        case Kind::Pouch: return 0xFFC08040u;           // tan
        case Kind::MagWell: return 0xFF40C0C0u;         // cyan
        case Kind::Bolt: return 0xFFC040C0u;            // magenta
        case Kind::Pump: return 0xFFA040FFu;            // violet
        case Kind::Foregrip: return 0xFF4080FFu;        // blue
        case Kind::HeldSpare: return 0xFFFFFFFFu;       // white
        case Kind::DualReload: return 0xFF80FFC0u;      // mint
        case Kind::MagSlap: return 0xFFFF80C0u;         // pink
        case Kind::DroppedMag: return 0xFF806040u;      // brown
        case Kind::Pickup: return 0xFF60A060u;          // dull green
        case Kind::ObjectPull: return 0xFFFFA000u;      // orange
        case Kind::Consumable: return 0xFFFF6080u;      // salmon
        case Kind::MeleeBlade: return 0xFFE0E0FFu;      // pale blue
        case Kind::BashTip: return 0xFFFF8040u;         // coral
        case Kind::ScopeEyebox: return 0xFF00FF80u;     // spring green
        case Kind::ThrowRelease: return 0xFFFFFF80u;    // pale yellow
        case Kind::HandSphere: return 0xFFD0D0D0u;      // light grey
        case Kind::SightLine: return 0xFFFF2020u;       // red-ish
        default: return 0xFF808080u;
        }
    }

    template <class Vec>
    struct Line
    {
        Vec a{}, b{};
        std::uint32_t argb = 0u;
        bool viewmodelLayer = false;   // in the reprojected viewmodel space (gun parts)
    };

    template <class Vec>
    class Builder
    {
    public:
        static constexpr std::size_t kMaxLines = 8000u;

        Builder(std::vector<Line<Vec>>& out, bool enabled) : m_Out(out), m_Enabled(enabled) {}
        bool Enabled() const { return m_Enabled; }
        std::size_t Count() const { return m_Out.size(); }
        // Kinds that emitted at least one line (bit per Kind).
        std::uint32_t KindsDrawn() const { return m_Kinds; }

        void Segment(Kind kind, State state, const Vec& a, const Vec& b, bool vm = false)
        {
            if (!m_Enabled || m_Out.size() >= kMaxLines || !Finite(a) || !Finite(b))
                return;
            Line<Vec> line{};
            line.a = a;
            line.b = b;
            line.argb = Color(kind, state);
            line.viewmodelLayer = vm;
            m_Out.push_back(line);
            m_Kinds |= 1u << static_cast<int>(kind);
        }

        // Oriented box: origin + axes, local mins/maxs along them.
        void Box(Kind kind, State state, const Vec& origin, const Vec& ax, const Vec& ay, const Vec& az,
            const Vec& mins, const Vec& maxs, bool vm = false)
        {
            Vec c[8];
            for (int i = 0; i < 8; ++i)
            {
                const float x = (i & 1) ? maxs.x : mins.x;
                const float y = (i & 2) ? maxs.y : mins.y;
                const float z = (i & 4) ? maxs.z : mins.z;
                c[i] = origin + ax * x + ay * y + az * z;
            }
            static const int kEdges[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
            for (const auto& e : kEdges)
                Segment(kind, state, c[e[0]], c[e[1]], vm);
        }

        void Circle(Kind kind, State state, const Vec& center, const Vec& u, const Vec& v, float radius,
            int segments = 20, bool vm = false)
        {
            if (segments < 3 || !(radius > 0.0f))
                return;
            constexpr float kTwoPi = 6.28318530717958647692f;
            Vec prev = center + u * radius;
            for (int i = 1; i <= segments; ++i)
            {
                const float t = kTwoPi * static_cast<float>(i) / static_cast<float>(segments);
                const Vec next = center + u * (radius * std::cos(t)) + v * (radius * std::sin(t));
                Segment(kind, state, prev, next, vm);
                prev = next;
            }
        }

        // Three great circles (world axes).
        void Sphere(Kind kind, State state, const Vec& center, float radius, int segments = 20, bool vm = false)
        {
            const Vec x = Axis(0), y = Axis(1), z = Axis(2);
            Circle(kind, state, center, x, y, radius, segments, vm);
            Circle(kind, state, center, x, z, radius, segments, vm);
            Circle(kind, state, center, y, z, radius, segments, vm);
        }

        // A capsule a -> b of radius r: end spheres plus four side lines.
        void Capsule(Kind kind, State state, const Vec& a, const Vec& b, float radius, int segments = 16, bool vm = false)
        {
            Vec d = b - a;
            const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (!(len > 1e-5f))
            {
                Sphere(kind, state, a, radius, segments, vm);
                return;
            }
            d = d * (1.0f / len);
            Vec u{}, v{};
            Perpendiculars(d, u, v);
            Circle(kind, state, a, u, v, radius, segments, vm);
            Circle(kind, state, b, u, v, radius, segments, vm);
            Circle(kind, state, a, u, d, radius, segments, vm);
            Circle(kind, state, b, u, d, radius, segments, vm);
            for (int i = 0; i < 4; ++i)
            {
                const Vec o = (i == 0 ? u : i == 1 ? u * -1.0f : i == 2 ? v : v * -1.0f) * radius;
                Segment(kind, state, a + o, b + o, vm);
            }
        }

        // A cone from apex along dir (unit), length, half-angle (radians).
        void Cone(Kind kind, State state, const Vec& apex, const Vec& dir, float length, float halfAngle,
            int segments = 16, bool vm = false)
        {
            if (!(length > 0.0f) || !std::isfinite(halfAngle))
                return;
            Vec u{}, v{};
            Perpendiculars(dir, u, v);
            const Vec base = apex + dir * length;
            const float r = length * std::tan(halfAngle);
            Circle(kind, state, base, u, v, r, segments, vm);
            for (int i = 0; i < 4; ++i)
            {
                const Vec o = (i == 0 ? u : i == 1 ? u * -1.0f : i == 2 ? v : v * -1.0f) * r;
                Segment(kind, state, apex, base + o, vm);
            }
        }

        void Cross(Kind kind, State state, const Vec& p, float size, bool vm = false)
        {
            const Vec x = Axis(0) * size, y = Axis(1) * size, z = Axis(2) * size;
            Segment(kind, state, p - x, p + x, vm);
            Segment(kind, state, p - y, p + y, vm);
            Segment(kind, state, p - z, p + z, vm);
        }

    private:
        static bool Finite(const Vec& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        static Vec Axis(int i)
        {
            Vec v{};
            v.x = i == 0 ? 1.0f : 0.0f;
            v.y = i == 1 ? 1.0f : 0.0f;
            v.z = i == 2 ? 1.0f : 0.0f;
            return v;
        }
        static void Perpendiculars(const Vec& d, Vec& u, Vec& v)
        {
            Vec ref = std::fabs(d.z) < 0.9f ? Axis(2) : Axis(0);
            // u = normalize(ref x d), v = d x u
            u.x = ref.y * d.z - ref.z * d.y; u.y = ref.z * d.x - ref.x * d.z; u.z = ref.x * d.y - ref.y * d.x;
            const float lu = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
            u = lu > 1e-6f ? u * (1.0f / lu) : Axis(0);
            v.x = d.y * u.z - d.z * u.y; v.y = d.z * u.x - d.x * u.z; v.z = d.x * u.y - d.y * u.x;
        }

        std::vector<Line<Vec>>& m_Out;
        bool m_Enabled = false;
        std::uint32_t m_Kinds = 0u;
    };
}

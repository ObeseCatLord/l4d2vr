#include "../L4D2VR/vr_physical_controls.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

// Manual pump stroke (pump and chrome shotgun): the fore-end follows the hand
// between its stops, cocks at a fraction of the travel and chambers when it
// comes back near closed, at any frame rate. Units: metres.
static void CheckImpl(bool result, int line, const char* what)
{
    if (!result) { std::fprintf(stderr, "Check failed at line %d: %s\n", line, what); std::abort(); }
}
#define Check(result) CheckImpl((result), __LINE__, #result)

using namespace l4d2vr_physical;

static bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// Runs one stroke sampled from hand positions; counts the events.
struct Counts { int cocked = 0; int chambered = 0; };
static Counts Run(PumpStroke& stroke, const PumpThresholds& t, const float* hands, int n)
{
    Counts c{};
    for (int i = 0; i < n; ++i)
    {
        const unsigned e = stroke.Update(hands[i], t);
        if (e & PumpStroke::Cocked) ++c.cocked;
        if (e & PumpStroke::Chambered) ++c.chambered;
    }
    return c;
}

int main()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // Thresholds: the defaults cock at 7 cm of 10 and chamber within 3.5 cm.
    const PumpThresholds t = ResolvePumpThresholds(0.10f, 0.70f, 0.35f);
    Check(t.Valid());
    Check(Near(t.travel, 0.10f) && Near(t.cock, 0.07f) && Near(t.chamber, 0.035f));
    // Clamps: the cock point is at least 30% of the travel, the chamber point
    // at least 10% of the travel ahead of it.
    Check(Near(ResolvePumpThresholds(0.10f, 0.05f, 0.0f).cock, 0.03f));
    Check(Near(ResolvePumpThresholds(0.10f, 0.70f, 0.95f).chamber, 0.06f));
    Check(Near(ResolvePumpThresholds(0.10f, 2.0f, 0.2f).cock, 0.10f));
    Check(!ResolvePumpThresholds(0.0f, 0.7f, 0.35f).Valid());
    Check(!ResolvePumpThresholds(nan, 0.7f, 0.35f).Valid());
    Check(ResolvePumpThresholds(0.10f, nan, nan).Valid()); // NaN fractions: full pull, chamber at closed

    // Not held: nothing happens.
    {
        PumpStroke s;
        Check(s.Update(-0.2f, t) == PumpStroke::None);
        Check(!s.Held() && s.Pull() == 0.0f);
    }

    // A slow full rack (1 cm steps): cocks once on the way back at 7 cm,
    // chambers once on the way forward within 3.5 cm of closed.
    {
        PumpStroke s;
        s.Grab(0.0f, false, 0.0f);
        int cockedAt = -1, chamberedAt = -1;
        for (int i = 1; i <= 20; ++i)
        {
            const float hand = -0.01f * static_cast<float>(i <= 10 ? i : 20 - i);
            const unsigned e = s.Update(hand, t);
            if (e & PumpStroke::Cocked) { Check(cockedAt < 0); cockedAt = i; }
            if (e & PumpStroke::Chambered) { Check(chamberedAt < 0); chamberedAt = i; }
        }
        Check(cockedAt == 7);       // 7 cm back
        Check(chamberedAt == 17);   // back to 3 cm (<= 3.5 cm) from closed
        Check(!s.IsCocked());
    }

    // A short pull (6 cm) and back: no cock, no chamber.
    {
        PumpStroke s;
        s.Grab(0.0f, false, 0.0f);
        const float hands[] = { -0.02f, -0.04f, -0.06f, -0.04f, -0.02f, 0.0f };
        const Counts c = Run(s, t, hands, 6);
        Check(c.cocked == 0 && c.chambered == 0);
    }

    // The forward stroke must reach the chamber zone: stopping 5 cm short of
    // closed leaves the action open (cocked) until it goes further.
    {
        PumpStroke s;
        s.Grab(0.0f, false, 0.0f);
        const float hands[] = { -0.05f, -0.09f, -0.05f };
        Counts c = Run(s, t, hands, 3);
        Check(c.cocked == 1 && c.chambered == 0 && s.IsCocked());
        const float more[] = { -0.03f };
        c = Run(s, t, more, 1);
        Check(c.chambered == 1);
    }

    // A fast rack seen in only two frames still cycles: 8 cm back in one
    // frame, closed in the next.
    {
        PumpStroke s;
        s.Grab(0.0f, false, 0.0f);
        const float hands[] = { -0.08f, 0.0f };
        const Counts c = Run(s, t, hands, 2);
        Check(c.cocked == 1 && c.chambered == 1);
    }

    // Pulling past the rear stop is not owed back: from a 15 cm pull (the
    // fore-end stops at 10) 6.5 cm forward is enough to chamber.
    {
        PumpStroke s;
        s.Grab(0.0f, false, 0.0f);
        const float hands[] = { -0.15f, -0.10f };
        Counts c = Run(s, t, hands, 2);
        Check(c.cocked == 1 && c.chambered == 0);
        Check(Near(s.Pull(), 0.05f));
        const float more[] = { -0.085f };
        c = Run(s, t, more, 1);
        Check(c.chambered == 1);
    }

    // Pushing forward past the front stop first is not owed back either.
    {
        PumpStroke s;
        s.Grab(0.0f, false, 0.0f);
        const float hands[] = { 0.05f, -0.025f };
        const Counts c = Run(s, t, hands, 2);
        Check(c.cocked == 1);
        Check(Near(s.Pull(), 0.075f));
    }

    // Letting go keeps the fore-end where it is and its cock latch; taking it
    // again (the hand anywhere) continues the stroke from there.
    {
        PumpStroke s;
        s.Grab(0.0f, false, 0.0f);
        const float hands[] = { -0.04f, -0.08f };
        Counts c = Run(s, t, hands, 2);
        Check(c.cocked == 1);
        const float pull = s.Pull();
        const bool cocked = s.IsCocked();
        s.Release();
        Check(s.Update(0.5f, t) == PumpStroke::None); // not held: ignored
        PumpStroke again;
        again.Grab(pull, cocked, 0.30f);
        Check(again.IsCocked() && Near(again.Pull(), 0.08f));
        const float forward[] = { 0.33f, 0.35f };
        c = Run(again, t, forward, 2);
        Check(c.cocked == 0 && c.chambered == 1);
    }

    // A dropped tracking sample (NaN) is skipped; the stroke continues from
    // the last good one.
    {
        PumpStroke s;
        s.Grab(0.0f, false, 0.0f);
        const float hands[] = { -0.04f, nan, -0.08f, nan, 0.0f };
        const Counts c = Run(s, t, hands, 5);
        Check(c.cocked == 1 && c.chambered == 1);
    }
    // A grab with no valid hand position does not hold the pump.
    {
        PumpStroke s;
        s.Grab(0.0f, false, nan);
        Check(!s.Held());
    }

    // Frame-rate independence: a quick 9 cm rack (0.25 s there and back),
    // sampled at 20 to 144 Hz with any phase, cycles exactly once every time.
    // The old rule (cock only at the full 10 cm travel, chamber within 2 cm)
    // missed every one of these.
    {
        const float rates[] = { 20.0f, 30.0f, 45.0f, 60.0f, 72.0f, 90.0f, 120.0f, 144.0f };
        const float kPi = 3.14159265f;
        for (float hz : rates)
        {
            for (int phase = 0; phase < 10; ++phase)
            {
                PumpStroke s;
                s.Grab(0.0f, false, 0.0f);
                const float dt = 1.0f / hz;
                const float t0 = dt * static_cast<float>(phase) / 10.0f;
                Counts c{};
                bool oldCocked = false, oldCycled = false;
                for (float time = t0; time <= 0.45f; time += dt)
                {
                    const float u = std::fmin(time / 0.25f, 1.0f);
                    const float hand = -0.09f * 0.5f * (1.0f - std::cos(2.0f * kPi * u));
                    const unsigned e = s.Update(hand, t);
                    if (e & PumpStroke::Cocked) ++c.cocked;
                    if (e & PumpStroke::Chambered) ++c.chambered;
                    const float pull = -hand;
                    if (pull >= 0.10f) oldCocked = true;
                    if (PumpCycles::StrokeComplete(oldCocked, pull, 0.02f)) oldCycled = true;
                }
                Check(c.cocked == 1 && c.chambered == 1);
                Check(!oldCycled);
            }
        }
    }

    // A 7.5 cm rack (just past the cock point) at 30 Hz also cycles.
    {
        PumpStroke s;
        s.Grab(0.0f, false, 0.0f);
        const float hands[] = { -0.03f, -0.065f, -0.075f, -0.06f, -0.03f, -0.01f };
        const Counts c = Run(s, t, hands, 6);
        Check(c.cocked == 1 && c.chambered == 1);
    }

    std::puts("vr_pump_stroke_tests passed");
    return 0;
}

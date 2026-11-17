#include "../L4D2VR/vr_backend/reference_space_rebase.h"
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main()
{
    using namespace l4d2vr_space_rebase;
    CHECK(IsIdentityChange(0, 0, 0, 0, 0, 0, 1));
    CHECK(IsIdentityChange(0.001f, 0, 0, 0, 0, 0, -1));
    CHECK(!IsIdentityChange(0.3f, 0, 0, 0, 0, 0, 1));
    CHECK(!IsIdentityChange(0, 0, 0, 0, 0.2588f, 0, 0.9659f)); // 30 deg yaw

    // No change announced: a big step is real motion (or a hitch), never re-based.
    {
        Rebaser r;
        CHECK(!r.ShouldRebase(1000, 0.5f, 40.0f));
    }
    // Announced: small steps pass through, the jump is taken once.
    {
        Rebaser r;
        r.OnChange(1000);
        CHECK(r.Pending(1000));
        CHECK(!r.ShouldRebase(1010, 0.02f, 1.0f));
        CHECK(r.ShouldRebase(1020, 0.8f, 2.0f));
        CHECK(!r.Pending(1030));
        CHECK(!r.ShouldRebase(1030, 0.8f, 2.0f));
    }
    // A yaw-only recentre.
    {
        Rebaser r;
        r.OnChange(0);
        CHECK(r.ShouldRebase(15, 0.01f, -90.0f));
    }
    // The window ends without a jump (identity-like change that still moved nothing).
    {
        Rebaser r;
        r.OnChange(1000);
        CHECK(!r.ShouldRebase(1000 + Rebaser::kWindowMs + 1, 0.5f, 0.0f));
        CHECK(!r.Pending(1000 + Rebaser::kWindowMs + 2));
    }
    CHECK(YawStep(170.0f, -170.0f) == 20.0f);
    CHECK(YawStep(-170.0f, 170.0f) == -20.0f);
    {
        BurstLog log;
        uint32_t folded = 99;
        CHECK(log.OnEvent(1000, folded) && folded == 0);
        for (int i = 0; i < 111; ++i)
            CHECK(!log.OnEvent(1100 + i * 10, folded));
        CHECK(log.OnEvent(1000 + BurstLog::kBurstMs + 5, folded) && folded == 111);
    }
    if (g_failures == 0)
        std::printf("vr_space_rebase_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}

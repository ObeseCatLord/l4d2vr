#include "../L4D2VR/vr_frame_perf.h"
#include "../L4D2VR/vr_log_filter.h"
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main()
{
    using namespace l4d2vr_frame_perf;
    {
        // 75 Hz: 3 periods = 40 ms, floor 50 ms; one line per second.
        SlowFrameLog log;
        CHECK(!log.ShouldLog(14000, 75.0f, 1000));
        CHECK(!log.ShouldLog(45000, 75.0f, 1010));
        CHECK(log.ShouldLog(212000, 75.0f, 1100));
        CHECK(!log.ShouldLog(212000, 75.0f, 1300));
        CHECK(!log.ShouldLog(212000, 75.0f, 1500));
        CHECK(log.ShouldLog(212000, 75.0f, 2200));
        CHECK(log.TakeSlowCount() == 4);
        CHECK(!log.ShouldLog(9000000, 75.0f, 5000)); // a load, not a slow frame
        // 30 Hz: 100 ms threshold.
        SlowFrameLog low;
        CHECK(!low.ShouldLog(90000, 30.0f, 1000));
        CHECK(low.ShouldLog(110000, 30.0f, 1000));
    }
    {
        Sample before;
        Take(before);
        Add(kRenderViewUs, 190000);
        Add(kModelDraws, 914);
        Add(kModelDrawUs, 60000);
        Add(kModelEngineUs, 29000);
        Add(kWorldPoseUs, 148000);
        Add(kWorldPoseCalls, 96);
        Add(kWorldPoseSolves, 3);
        { Scope s(kHudUs); }
        AddLog(std::chrono::microseconds(1500));
        Sample s;
        Take(s);
        s.frameUs = 212000;
        CHECK(s.values[kModelDraws] == 914 && s.values[kLogLines] == 1 && s.values[kLogUs] == 1500);
        Sample empty;
        Take(empty);
        CHECK(empty.values[kModelDraws] == 0);
        char line[768];
        Format(line, sizeof(line), s, 75.0f, 2, true, true, false);
        const std::string text(line);
        CHECK(text.rfind("[VR][Perf] slow frame 212.0 ms", 0) == 0);
        CHECK(text.find("models 914: hook 31.0 ms + engine 29.0 ms") != std::string::npos);
        CHECK(text.find("worldPose 148.0 ms (96 calls, 3 solves)") != std::string::npos);
        CHECK(text.find("outside renderView 22.0 ms") != std::string::npos);
        CHECK(text.find("thirdPerson=1 observer=1 incap=0") != std::string::npos);
    }
    {
        using namespace l4d2vr_logfilter;
        RateLimiter limiter;
        static const char* kSmoke = "[VR][FX][muzzlesmoke] capture source=%s";
        static const char* kOther = "[VR][MagazineInteraction][State] %s";
        static const char* kHolsterA = "[VR][HolsterDraw] gates: %d";
        static const char* kHolsterB = "[VR][HolsterDraw] render %d";
        CHECK(limiter.Check(kOther, 1000, false).allow);
        CHECK(limiter.Check(kOther, 1001, false).allow);
        CHECK(limiter.Check(kSmoke, 1000, false).allow);
        CHECK(!limiter.Check(kSmoke, 2000, false).allow);
        CHECK(!limiter.Check(kSmoke, 3000, false).allow);
        const auto d = limiter.Check(kSmoke, 31000, false);
        CHECK(d.allow && d.suppressedBefore == 2);
        CHECK(limiter.Check(kSmoke, 31001, true).allow); // VrHandsDebugLog
        // Per call site: two HolsterDraw lines do not starve each other.
        CHECK(limiter.Check(kHolsterA, 1000, false).allow);
        CHECK(limiter.Check(kHolsterB, 1000, false).allow);
        CHECK(!limiter.Check(kHolsterA, 5000, false).allow);
        CHECK(FindRule("[VR][GripInput] weapon grip") != nullptr);
        CHECK(FindRule("[VR][MagazineInteraction][Event] x") == nullptr);
        CHECK(FindRule(nullptr) == nullptr);
    }
    if (g_failures == 0)
        std::printf("vr_frame_perf_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}

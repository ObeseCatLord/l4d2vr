#include "../L4D2VR/vr_reload_log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void Check(bool result, const char* message)
{
    if (!result)
    {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

int main()
{
    using namespace l4d2vr_reload_log;
    Check(std::strcmp(StateName(0), "Idle") == 0, "State 0 is Idle");
    Check(std::strcmp(StateName(2), "WaitingForFreshMagazine") == 0, "State 2 waits for a fresh magazine");
    Check(std::strcmp(StateName(5), "WaitingForBoltGrab") == 0, "State 5 waits for the bolt");
    Check(std::strcmp(StateName(8), "WaitingForServerShell") == 0, "State 8 waits for the shell reply");
    Check(std::strcmp(StateName(11), "HoldingFreshMagazineForSlap") == 0, "State 11 holds a spare for a slap");
    Check(std::strcmp(StateName(12), "Unknown") == 0 && std::strcmp(StateName(-1), "Unknown") == 0,
        "Out-of-range states are named safely");

    RateLimiter limiter(3u, 1000u);
    unsigned dropped = 99u;
    Check(limiter.Allow(100u, dropped) && dropped == 0u, "The first line passes");
    Check(limiter.Allow(150u, dropped) && limiter.Allow(200u, dropped), "Lines up to the limit pass");
    Check(!limiter.Allow(300u, dropped) && !limiter.Allow(400u, dropped), "A flapping state is held back");
    Check(limiter.Allow(1100u, dropped) && dropped == 2u, "The next window reports what was dropped");
    Check(limiter.Allow(1150u, dropped) && dropped == 0u, "The drop count is reported once");
    Check(limiter.Allow(50u, dropped), "A clock going backwards starts a new window");
    return 0;
}

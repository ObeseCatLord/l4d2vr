#include "../L4D2VR/vr_weapon_reserve.h"
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main()
{
    using l4d2vr_weapon::HasInfiniteReserve;
    // 2.0.12 (issue 17): pistol and magnum show the infinity glyph, nothing else does.
    CHECK(HasInfiniteReserve(1));   // pistol
    CHECK(HasInfiniteReserve(32));  // magnum
    CHECK(!HasInfiniteReserve(2));  // uzi
    CHECK(!HasInfiniteReserve(0));
    CHECK(!HasInfiniteReserve(-1));
    for (int id = 3; id < 64; ++id)
        if (id != 32)
            CHECK(!HasInfiniteReserve(id));
    static_assert(HasInfiniteReserve(32), "constexpr");
    if (g_failures == 0)
        std::printf("vr_weapon_reserve_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}

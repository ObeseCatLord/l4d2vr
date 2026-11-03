#include "../L4D2VR/vr_config_migration.h"
#include <cstdio>

using namespace l4d2vr_config_migration;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main()
{
    CHECK(ValuesMatch("1", "1.0"));
    CHECK(ValuesMatch(" -1.0 ", "-1"));
    CHECK(ValuesMatch("0.4,0.2,-0.27", "0.40, 0.2 ,-0.270"));
    CHECK(!ValuesMatch("0.4,0.2,-0.27", "0.4,0.2,-0.28"));
    CHECK(!ValuesMatch("0,0,0", "0,0"));
    CHECK(ValuesMatch("False", "false"));
    CHECK(!ValuesMatch("false", "0"));     // booleans are not numbers
    CHECK(!ValuesMatch("true", "false"));
    CHECK(ValuesMatch("SecondaryAttack+Use", " SecondaryAttack+Use"));
    CHECK(!ValuesMatch("0.05", "0.0500001x"));
    // A tuned value is never replaced; a missing key or the old shipped one is.
    CHECK(ShouldReplace(false, "", "3"));
    CHECK(ShouldReplace(true, "3.0", "3"));
    CHECK(!ShouldReplace(true, "2.5", "3"));
    std::vector<double> n;
    CHECK(!ParseNumbers("1,,2", n));
    CHECK(ParseNumbers("1e-2", n) && n.size() == 1);
    if (g_failures == 0)
        std::printf("vr_config_migration: all tests passed\n");
    return g_failures == 0 ? 0 : 1;
}

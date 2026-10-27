#include <limits>
#if defined(VECTOR_PARANOIA) && !defined(VEC_T_NAN)
#define VEC_T_NAN std::numeric_limits<float>::quiet_NaN()
#endif
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_sight_correction.h"
#include <cstdio>
#include <cstdlib>

using namespace l4d2vr_sight_correction;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void TestRecoversInwardBore()
{
    // A bore pointing 3 deg left and 1.5 deg down in the viewmodel frame, with a
    // table offset of (2, -2, 0). Aim straight along the controller.
    QAngle bent(1.5f, 3.0f, 0.0f);
    Vector bore{};
    QAngle::AngleVectors(bent, &bore, nullptr, nullptr);
    const QAngle base(2.0f, -2.0f, 0.0f);
    QAngle correction{};
    CHECK(Solve(base, bore, Vector(1.0f, 0.0f, 0.0f), correction));
    const QAngle total(base.x + correction.x, base.y + correction.y, base.z);
    const Vector drawn = BoreWorld(RotateBasis(Basis{}, total), bore);
    CHECK(AngleBetweenDeg(drawn, Vector(1.0f, 0.0f, 0.0f)) < 0.05f);
    CHECK(std::fabs(correction.y) > 0.5f);
}

static void TestAlreadyAligned()
{
    QAngle correction{};
    CHECK(Solve(QAngle(0, 0, 0), Vector(1, 0, 0), Vector(1, 0, 0), correction));
    CHECK(std::fabs(correction.x) < 0.01f && std::fabs(correction.y) < 0.01f);
}

static void TestWithRollAndPitchedAim()
{
    Vector bore{};
    QAngle::AngleVectors(QAngle(-2.0f, -4.0f, 0.0f), &bore, nullptr, nullptr);
    Vector aim{};
    QAngle::AngleVectors(QAngle(5.0f, 1.0f, 0.0f), &aim, nullptr, nullptr);
    const QAngle base(0.0f, 0.0f, 8.0f);
    QAngle correction{};
    CHECK(Solve(base, bore, aim, correction));
    const QAngle total(base.x + correction.x, base.y + correction.y, base.z);
    CHECK(AngleBetweenDeg(BoreWorld(RotateBasis(Basis{}, total), bore), aim) < 0.05f);
}

static void TestRejectsLargeOrDegenerate()
{
    QAngle correction{};
    CHECK(!Solve(QAngle(0, 0, 0), Vector(0, 0, 0), Vector(1, 0, 0), correction));
    Vector sideways(0.0f, 1.0f, 0.0f);
    CHECK(!Solve(QAngle(0, 0, 0), sideways, Vector(1, 0, 0), correction, 20.0f));
}

static void TestAverage()
{
    BoreAverage avg;
    avg.Add(Vector(1, 0.02f, 0));
    avg.Add(Vector(1, -0.02f, 0));
    avg.Add(Vector(0, 0, 0));
    CHECK(avg.count == 2);
    CHECK(std::fabs(avg.Mean().y) < 1e-4f);
}

int main()
{
    TestRecoversInwardBore();
    TestAlreadyAligned();
    TestWithRollAndPitchedAim();
    TestRejectsLargeOrDegenerate();
    TestAverage();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_sight_correction tests passed\n");
    return EXIT_SUCCESS;
}

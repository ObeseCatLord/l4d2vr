#include "../L4D2VR/server_core/vr_usercmd_codec.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_Checks = 0;
static void CheckImpl(bool ok, const char* what, int line)
{
    ++g_Checks;
    if (!ok) { std::fprintf(stderr, "usercmd codec check failed at line %d: %s\n", line, what); std::abort(); }
}
#define Check(x) CheckImpl((x), #x, __LINE__)

using namespace l4d2vr_server;

namespace
{
    // Literal transcription of the listen host's Hooks::dReadUsercmd pose
    // arithmetic (hooks_combat_network.inl) used as an oracle.
    struct OracleResult { float ax, ay, az, px, py, pz, pitch; int command; int tick; bool melee; };
    OracleResult OracleDecode(UsercmdData move)
    {
        OracleResult r{};
        move.tick_count *= -1;
        r.melee = false;
        if (move.command_number < 0) { move.command_number *= -1; r.melee = true; }
        r.ax = (float)move.mousedx / 10;
        r.ay = (float)move.mousedy / 10;
        r.px = move.viewangles[2];
        r.py = move.upmove;
        int rollEncoding = move.command_number / 10000000;
        move.command_number -= rollEncoding * 10000000;
        r.az = (float)((rollEncoding * 2) - 180);
        int decodedZInt = (int)(move.viewangles[0] / 10000);
        float decodedAngle = fabsf((float)(move.viewangles[0] - (decodedZInt * 10000)) / 10);
        decodedAngle -= 360.0f;
        r.pz = (float)decodedZInt / 10.0f;
        r.pitch = decodedAngle;
        r.command = move.command_number;
        r.tick = move.tick_count;
        return r;
    }

    UsercmdData SampleCommand(int command, int tick, float pitch, float yaw)
    {
        UsercmdData c{};
        c.command_number = command;
        c.tick_count = tick;
        c.viewangles[0] = pitch;
        c.viewangles[1] = yaw;
        c.viewangles[2] = 0.0f;
        c.forwardmove = 450.0f;
        c.sidemove = -225.0f;
        c.upmove = 0.0f;
        c.buttons = kInAttack;
        c.random_seed = 1234;
        return c;
    }

    bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

    void TestDesktopPassthrough()
    {
        UsercmdData c = SampleCommand(5000, 9000, 12.5f, 33.0f);
        c.impulse = 238;                       // a desktop "impulse 238" is not a VR intent
        c.buttons = static_cast<int>(0xFC000001u);
        c.weaponselect = 77;
        c.mousedx = 12;
        const UsercmdData before = c;
        const DecodedUsercmd d = DecodeVRUsercmd(c);
        Check(!d.vr);
        Check(std::memcmp(&before, &c, sizeof(c)) == 0);
    }

    void TestRoundTripAgainstOracle()
    {
        int cases = 0;
        for (int i = 0; i < 4000; ++i)
        {
            const float pitch = -89.0f + static_cast<float>((i * 37) % 1781) * 0.1f;
            const float yaw = -180.0f + static_cast<float>((i * 53) % 3601) * 0.1f;
            const Vec3 pos(-3000.0f + static_cast<float>((i * 97) % 60000) * 0.1f,
                -2500.0f + static_cast<float>((i * 71) % 50000) * 0.1f,
                -1500.0f + static_cast<float>((i * 13) % 30000) * 0.1f);
            const Angles ang(-89.0f + static_cast<float>((i * 11) % 1780) * 0.1f,
                -180.0f + static_cast<float>((i * 7) % 3600) * 0.1f,
                -180.0f + static_cast<float>((i * 3) % 360));
            const bool melee = (i % 5) == 0;
            const int command = 1 + (i * 7919) % 2000000;
            UsercmdData c = SampleCommand(command, 100 + i, pitch, yaw);
            EncodeVRUsercmd(c, pos, ang, melee);
            const UsercmdData wire = c;
            Check(c.tick_count < 0);
            const OracleResult oracle = OracleDecode(wire);
            const DecodedUsercmd d = DecodeVRUsercmd(c);
            Check(d.vr && d.poseValid);
            Check(d.meleeSwing == melee && oracle.melee == melee);
            Check(c.command_number == command && oracle.command == command);
            Check(c.tick_count == 100 + i && oracle.tick == 100 + i);
            // Bit-exact agreement with the listen-host arithmetic.
            Check(d.controllerAngles.x == oracle.ax && d.controllerAngles.y == oracle.ay && d.controllerAngles.z == oracle.az);
            Check(d.controllerPos.x == oracle.px && d.controllerPos.y == oracle.py && d.controllerPos.z == oracle.pz);
            Check(c.viewangles[0] == oracle.pitch);
            // And close to the encoded pose (quantisation).
            Check(Near(d.controllerAngles.x, ang.x, 0.1001f) && Near(d.controllerAngles.y, ang.y, 0.1001f));
            Check(Near(d.controllerAngles.z, ang.z, 2.001f));
            Check(d.controllerPos.x == pos.x && d.controllerPos.y == pos.y);
            Check(Near(d.controllerPos.z, pos.z, 0.1001f));
            // viewangles.x carries pos.z * 1e5 in a float: the pitch loses
            // precision above 2^24 (|pos.z| > ~168 units), exactly as on the
            // listen host. Allow one float ulp of the packed value.
            const float packed = std::fabs(wire.viewangles[0]);
            const float ulp = packed > 0.0f ? std::ldexp(1.0f, std::ilogb(packed) - 23) : 0.0f;
            Check(Near(c.viewangles[0], pitch, 0.15f + ulp / 10.0f));
            // Stripped like the listen host.
            Check(c.viewangles[2] == 0.0f && c.upmove == 0.0f && c.viewangles[1] == yaw);
            Check(c.forwardmove == 450.0f && c.sidemove == -225.0f && c.random_seed == 1234);
            ++cases;
        }
        Check(cases == 4000);
    }

    void TestImpulsesAndCarryWord()
    {
        const std::uint8_t impulses[] = { 234, 235, 236, 237, 238, 239 };
        const Intent intents[] = { Intent::GripPickup, Intent::GripRelease, Intent::BlockNativeReload,
            Intent::HolsterDrop, Intent::Shove, Intent::PillGive };
        for (int i = 0; i < 6; ++i)
        {
            UsercmdData c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.impulse = impulses[i];
            c.buttons = kInUse;
            EncodeVRUsercmd(c, Vec3(1, 2, 3), Angles(0, 0, 0), false);
            const DecodedUsercmd d = DecodeVRUsercmd(c);
            Check(d.intent == intents[i] && d.incomingImpulse == impulses[i] && c.impulse == 0u);
        }
        // Reload policy mirrors the listen host.
        {
            UsercmdData c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.impulse = 236;
            EncodeVRUsercmd(c, Vec3(), Angles(), false);
            Check(DecodeVRUsercmd(c).reload == l4d2vr_server_pistol::ReloadPolicy::Block);
            c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.impulse = 234;
            EncodeVRUsercmd(c, Vec3(), Angles(), false);
            Check(DecodeVRUsercmd(c).reload == l4d2vr_server_pistol::ReloadPolicy::Preserve);
            c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.impulse = 101;   // an ordinary impulse survives and keeps the reload gate
            EncodeVRUsercmd(c, Vec3(), Angles(), false);
            const DecodedUsercmd d = DecodeVRUsercmd(c);
            Check(d.reload == l4d2vr_server_pistol::ReloadPolicy::Preserve && c.impulse == 101 && d.intent == Intent::None);
            c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.impulse = 238;   // a gun bash with an empty gun must not start the stock reload
            EncodeVRUsercmd(c, Vec3(), Angles(), false);
            Check(DecodeVRUsercmd(c).reload == l4d2vr_server_pistol::ReloadPolicy::Preserve);
            c = SampleCommand(10, 20, 0.0f, 0.0f);
            EncodeVRUsercmd(c, Vec3(), Angles(), false);
            Check(DecodeVRUsercmd(c).reload == l4d2vr_server_pistol::ReloadPolicy::Native);
        }
        // Object pull: entity index and hint leave weaponselect/weaponsubtype.
        {
            UsercmdData c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.impulse = 242;
            c.weaponselect = 1500;
            c.weaponsubtype = 3;
            EncodeVRUsercmd(c, Vec3(), Angles(), false);
            const DecodedUsercmd d = DecodeVRUsercmd(c);
            Check(d.intent == Intent::ObjectPull && d.objectPullCommand == 242 && d.objectPullEntity == 1500 &&
                d.objectPullHint == 3 && c.impulse == 0 && c.weaponselect == 0 && c.weaponsubtype == 0);
        }
        // Hand throw: the velocity leaves forwardmove/sidemove/weaponselect/
        // weaponsubtype; the game sees the real movement and no weapon select.
        {
            UsercmdData c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.buttons = 0;
            c.impulse = kImpulseHandThrow;
            c.forwardmove = l4d2vr_hand_throw::PackMove(450.0f, 310.0f);
            c.sidemove = l4d2vr_hand_throw::PackMove(-225.0f, -40.0f);
            l4d2vr_hand_throw::PackVertical(120.0f, l4d2vr_hand_throw::Kind::Molotov, true, c.weaponselect, c.weaponsubtype);
            EncodeVRUsercmd(c, Vec3(1, 2, 3), Angles(), false);
            const DecodedUsercmd d = DecodeVRUsercmd(c);
            Check(d.intent == Intent::HandThrow && d.handThrow.valid && d.handThrow.kind == l4d2vr_hand_throw::Kind::Molotov);
            Check(d.handThrow.leftHand && d.handThrow.velocity.x == 310.0f && d.handThrow.velocity.y == -40.0f &&
                d.handThrow.velocity.z == 120.0f);
            Check(c.impulse == 0 && c.weaponselect == 0 && c.weaponsubtype == 0 && c.forwardmove == 450.0f &&
                c.sidemove == -225.0f && !IsInventoryRelease(d) && d.carryWeaponId == weapon::kNone);
            // A hand throw keeps the pose of its command.
            Check(Near(d.controllerPos.x, 1.0f, 1e-4f) && Near(d.controllerPos.z, 3.0f, 0.11f));
        }
        {
            UsercmdData c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.impulse = kImpulseHandThrow;     // garbage payload
            c.weaponselect = 77;
            EncodeVRUsercmd(c, Vec3(), Angles(), false);
            const DecodedUsercmd d = DecodeVRUsercmd(c);
            Check(d.intent == Intent::None && !d.handThrow.valid && c.impulse == 0 && c.weaponselect == 0 &&
                c.weaponsubtype == 0 && c.forwardmove == 0.0f && c.sidemove == 0.0f);
        }
        // Carry word: weapon id + 1 and pistol markers 60..63 are stripped.
        {
            UsercmdData c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.buttons = kInAttack | static_cast<int>(17u << kCarryWordShift);   // gascan (16) + 1
            EncodeVRUsercmd(c, Vec3(), Angles(), false);
            const DecodedUsercmd d = DecodeVRUsercmd(c);
            Check(d.carryWord == 17u && d.carryWeaponId == weapon::kGascan);
            Check(c.buttons == kInAttack && IsCarryRelease(d) && !IsInventoryRelease(d));
        }
        {
            UsercmdData c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.buttons = static_cast<int>(20u << kCarryWordShift);   // melee (19) + 1, no attack
            EncodeVRUsercmd(c, Vec3(), Angles(), false);
            const DecodedUsercmd d = DecodeVRUsercmd(c);
            Check(d.carryWeaponId == weapon::kMelee && IsInventoryRelease(d) && c.buttons == 0);
        }
        {
            UsercmdData c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.buttons = kInAttack | static_cast<int>(63u << kCarryWordShift);
            EncodeVRUsercmd(c, Vec3(), Angles(), false);
            const DecodedUsercmd d = DecodeVRUsercmd(c);
            Check(d.shotHand == l4d2vr_dual::Hand::Left && c.buttons == kInAttack);
        }
        {
            UsercmdData c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.buttons = static_cast<int>(60u << kCarryWordShift);
            EncodeVRUsercmd(c, Vec3(), Angles(), false);
            const DecodedUsercmd d = DecodeVRUsercmd(c);
            Check(d.dropHand == l4d2vr_dual::Hand::Right && d.carryWeaponId == weapon::kPistol && c.buttons == 0);
        }
        {
            UsercmdData c = SampleCommand(10, 20, 0.0f, 0.0f);
            c.impulse = 234;
            c.buttons = kInUse | static_cast<int>(62u << kCarryWordShift);
            EncodeVRUsercmd(c, Vec3(), Angles(), false);
            const DecodedUsercmd d = DecodeVRUsercmd(c);
            Check(d.pickupHand == l4d2vr_dual::Hand::Right && c.buttons == kInUse);
        }
    }

    void TestMalformedInput()
    {
        UsercmdData c = SampleCommand(10, 20, 0.0f, 0.0f);
        c.tick_count = INT_MIN;
        c.command_number = INT_MIN;
        c.viewangles[0] = std::nanf("");
        const DecodedUsercmd d = DecodeVRUsercmd(c);
        Check(d.vr && !d.poseValid && d.meleeSwing);
        Check(c.tick_count == INT_MAX && c.viewangles[0] == 0.0f && std::isfinite(c.viewangles[0]));
        UsercmdData big = SampleCommand(10, -5, 0.0f, 0.0f);
        big.viewangles[0] = 3.0e30f;
        const DecodedUsercmd d2 = DecodeVRUsercmd(big);
        Check(d2.vr && !d2.poseValid && big.viewangles[0] == 0.0f);
        UsercmdData inf = SampleCommand(10, -5, 0.0f, 0.0f);
        inf.upmove = INFINITY;
        const DecodedUsercmd d3 = DecodeVRUsercmd(inf);
        Check(d3.vr && !d3.poseValid && inf.upmove == 0.0f);
    }
}

int main()
{
    TestDesktopPassthrough();
    TestRoundTripAgainstOracle();
    TestImpulsesAndCarryWord();
    TestMalformedInput();
    std::printf("vr_server_usercmd_codec_tests: %d checks passed\n", g_Checks);
    return 0;
}

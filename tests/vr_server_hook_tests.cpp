// IA-32 prologue decoder/trampoline builder and ELF32 symbol reader used by
// the Linux dedicated-server plugin.
#include "../L4D2VR/server_core/vr_elf_image.h"
#include "../L4D2VR/server_core/vr_x86_prologue.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_Checks = 0;
static void CheckImpl(bool ok, const char* what, int line)
{
    ++g_Checks;
    if (!ok) { std::fprintf(stderr, "server hook check failed at line %d: %s\n", line, what); std::abort(); }
}
#define Check(x) CheckImpl((x), #x, __LINE__)

#if defined(_MSC_VER)
extern "C" __declspec(noinline) int l4d2vr_elf_test_marker(int value)
#else
extern "C" __attribute__((noinline, used)) int l4d2vr_elf_test_marker(int value)
#endif
{
    return value * 3 + 1;
}

namespace
{
    std::vector<std::uint8_t> Bytes(const char* hex)
    {
        const auto pattern = l4d2vr_x86::Pattern::Parse(hex);
        std::vector<std::uint8_t> out;
        for (auto b : pattern.bytes) out.push_back(static_cast<std::uint8_t>(b < 0 ? 0 : b));
        out.resize(out.size() + 16u, 0x90u);
        return out;
    }

    std::size_t Stolen(const char* hex)
    {
        const auto code = Bytes(hex);
        const auto plan = l4d2vr_x86::PlanHook(code.data(), code.size(), 5u);
        return plan.ok ? plan.stolen : 0u;
    }

    void TestRealPrologues()
    {
        // Byte sequences recorded from the Marmot Cafe server_srv.so.
        Check(Stolen("55 89 E5 57 56 53 83 EC 2C 8B 75 0C") == 5u);            // ReadUsercmd
        Check(Stolen("55 89 E5 57 56 53 81 EC CC 00 00 00") == 5u);            // FireTerrorBullets
        Check(Stolen("55 89 E5 56 53 83 EC 10 8B 75 0C") == 5u);               // Weapon_ShootPosition
        Check(Stolen("55 89 E5 53 83 EC 14 8B 5D 08") == 7u);                  // CTerrorGun::FinishReload
        Check(Stolen("55 8B 0D BC 01 F2 00 89 E5") == 7u);                     // GetActiveWeapon
        Check(Stolen("55 B8 BF 00 00 00 89 E5 57") == 6u);                     // GetPrimaryAttackActivity
        Check(Stolen("55 31 C0 89 E5 8B 55 08") == 5u);                        // IsIncapacitated
        Check(Stolen("55 89 E5 57 56 8D BD E8 E9 FF FF 53") == 5u);            // ProcessUsercmds
        Check(Stolen("55 89 E5 5D C3 90 90") == 5u);                           // CBaseShotgun::FinishReload (ret last)
        Check(Stolen("65 A1 14 00 00 00 89 45 E4") == 6u);                     // gs: stack protector load
        Check(Stolen("F3 0F 10 45 0C 89 E5") == 5u);                           // movss xmm0,[ebp+0xc]
        Check(Stolen("8B 84 24 10 01 00 00 C3") == 7u);                        // mov eax,[esp+disp32]
        Check(Stolen("8B 04 85 00 10 00 00 C3") == 7u);                        // SIB with disp32, no base
    }

    void TestRefusals()
    {
        Check(Stolen("55 74 05 89 E5 57") == 0u);                              // jz rel8 in window
        Check(Stolen("55 EB 05 89 E5 57") == 0u);                              // jmp rel8
        Check(Stolen("55 0F 84 10 00 00 00") == 0u);                           // jz rel32
        Check(Stolen("C3 90 90 90 90 90") == 0u);                              // ret before 5 bytes
        Check(Stolen("55 FF E0 90 90 90") == 0u);                              // jmp eax before 5 bytes
        Check(Stolen("55 E2 FE 90 90 90") == 0u);                              // loop
        const std::uint8_t truncated[] = { 0x55, 0x81, 0xEC, 0x10 };
        Check(!l4d2vr_x86::PlanHook(truncated, sizeof(truncated), 5u).ok);
        Check(!l4d2vr_x86::PlanHook(nullptr, 0u, 5u).ok);
    }

    void TestTrampolineRelocation()
    {
        // call rel32 at entry: target = 0x1000 + 5 + 0x100 = 0x1105.
        const std::uint8_t code[] = { 0xE8, 0x00, 0x01, 0x00, 0x00, 0x55, 0x89, 0xE5 };
        const auto plan = l4d2vr_x86::PlanHook(code, sizeof(code), 5u);
        Check(plan.ok && plan.stolen == 5u && plan.relFields.size() == 1u && plan.relFields[0] == 1u);
        std::uint8_t tramp[16]{};
        Check(l4d2vr_x86::BuildTrampoline(code, 0x1000u, plan, tramp, 0x5000u));
        const std::uint32_t target = 0x5000u + 5u + static_cast<std::uint32_t>(l4d2vr_x86::ReadI32(tramp + 1));
        Check(tramp[0] == 0xE8 && target == 0x1105u);
        // jmp back to the first non-stolen byte.
        const std::uint32_t back = 0x5000u + 10u + static_cast<std::uint32_t>(l4d2vr_x86::ReadI32(tramp + 6));
        Check(tramp[5] == 0xE9 && back == 0x1005u);

        // Plain prologue: bytes copied verbatim.
        const std::uint8_t plain[] = { 0x55, 0x89, 0xE5, 0x57, 0x56, 0x53, 0x83, 0xEC, 0x2C };
        const auto p2 = l4d2vr_x86::PlanHook(plain, sizeof(plain), 5u);
        std::uint8_t t2[16]{};
        Check(l4d2vr_x86::BuildTrampoline(plain, 0x56B490u, p2, t2, 0x08000000u));
        Check(std::memcmp(t2, plain, 5) == 0 && t2[5] == 0xE9);
        Check(0x08000000u + 10u + static_cast<std::uint32_t>(l4d2vr_x86::ReadI32(t2 + 6)) == 0x56B495u);
    }

    void TestPatterns()
    {
        const auto p = l4d2vr_x86::Pattern::Parse("55 8B 0D ?? ?? ?? ?? 89 E5");
        Check(p.size() == 9u && p.bytes[3] == -1);
        const std::uint8_t code[] = { 0x90, 0x55, 0x8B, 0x0D, 1, 2, 3, 4, 0x89, 0xE5 };
        Check(!p.Matches(code, sizeof(code)) && p.Matches(code + 1, sizeof(code) - 1));
        Check(p.Find(code, sizeof(code)) == 1);
        Check(l4d2vr_x86::Pattern::Parse("5G").size() == 0u);
        Check(l4d2vr_x86::HexBytes(code, 3) == "90 55 8B");
    }

    void TestElf()
    {
        l4d2vr_elf::Image image;
        std::string error;
#if !defined(__linux__)
        std::vector<std::uint8_t> notElf(64, 0);
        Check(!image.LoadBytes(notElf, error));
        return;
#endif
        const bool loaded = image.LoadFile("/proc/self/exe", error);
        if constexpr (sizeof(void*) != 4u)
        {
            Check(!loaded && error.find("32-bit") != std::string::npos);
            return;
        }
        Check(loaded);
        l4d2vr_elf::Symbol marker{};
        Check(image.Resolve("l4d2vr_elf_test_marker", marker) && marker.type == l4d2vr_elf::kSttFunc);
        Check(image.IsExecutable(marker.value));
        const auto runtime = reinterpret_cast<std::uintptr_t>(&l4d2vr_elf_test_marker);
        std::uint8_t fileBytes[8]{};
        Check(image.ReadVirtual(marker.value, fileBytes, sizeof(fileBytes)));
        Check(std::memcmp(fileBytes, reinterpret_cast<const void*>(runtime), sizeof(fileBytes)) == 0);
        Check(!image.Resolve("no_such_symbol_l4d2vr", marker));
        std::vector<std::uint8_t> junk(64, 0);
        l4d2vr_elf::Image bad;
        Check(!bad.LoadBytes(junk, error));
        Check(l4d2vr_elf_test_marker(2) == 7);
    }
}

int main()
{
    TestRealPrologues();
    TestRefusals();
    TestTrampolineRelocation();
    TestPatterns();
    TestElf();
    std::printf("vr_server_hook_tests: %d checks passed\n", g_Checks);
    return 0;
}

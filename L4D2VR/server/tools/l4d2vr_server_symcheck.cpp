// Offline symbol-resolution self-test for the Linux dedicated plugin.
//
//   l4d2vr_server_symcheck <path/to/server_srv.so>
//
// Loads the on-disk symbol table (.symtab locals included), evaluates every
// hook/call target exactly like the plugin does at load (build guard, length
// decoder, vtable slot search) against link-time addresses, prints address,
// prologue bytes and the hook strategy verdict, then the feature bits that
// would resolve if the runtime-only network properties are found.
// Exit status: 0 when every target is accepted, 1 otherwise.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../server_core/vr_elf_image.h"
#include "../l4d2vr_linux_targets.h"

namespace
{
    class FileView final : public l4d2vr_linux::CodeView
    {
    public:
        explicit FileView(const l4d2vr_elf::Image& image) : m_Image(image) {}

        bool Symbol(const char* name, std::uint32_t& address, std::uint32_t& size) const override
        {
            l4d2vr_elf::Symbol s{};
            if (!m_Image.Resolve(name, s)) return false;
            address = s.value;
            size = s.size;
            return true;
        }

        bool Read(std::uint32_t address, void* out, std::size_t size) const override
        {
            return m_Image.ReadVirtual(address, out, size);
        }

        void ForEachVtable(void (*fn)(void*, const char*, std::uint32_t, std::uint32_t), void* ctx) const override
        {
            m_Image.ForEachSymbol([&](const std::string& name, const l4d2vr_elf::Symbol& s)
            {
                if (name.compare(0, 4, "_ZTV") == 0 && s.type == l4d2vr_elf::kSttObject && s.size >= 12u && s.duplicates == 0)
                    fn(ctx, name.c_str(), s.value, s.size);
            });
        }

    private:
        const l4d2vr_elf::Image& m_Image;
    };

    const char* StrategyName(l4d2vr_linux::Strategy s)
    {
        switch (s)
        {
        case l4d2vr_linux::Strategy::Inline: return "inline";
        case l4d2vr_linux::Strategy::Vtable: return "vtable";
        case l4d2vr_linux::Strategy::AllVtables: return "all-vtables";
        case l4d2vr_linux::Strategy::Call: return "call";
        case l4d2vr_linux::Strategy::Slot: return "slot";
        case l4d2vr_linux::Strategy::Data: return "data";
        }
        return "?";
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <server_srv.so>\n", argv[0]);
        return 2;
    }
    l4d2vr_elf::Image image;
    std::string error;
    if (!image.LoadFile(argv[1], error))
    {
        std::fprintf(stderr, "cannot load %s: %s\n", argv[1], error.c_str());
        return 2;
    }
    std::printf("server module: %s (%zu symbols)\n\n", argv[1], image.SymbolCount());
    FileView view(image);

    std::vector<l4d2vr_linux::TargetResult> results;
    int failures = 0;
    for (const auto& spec : l4d2vr_linux::Targets())
    {
        results.push_back(l4d2vr_linux::EvaluateTarget(view, spec));
        auto& r = results.back();
        std::printf("%-4s %-28s %-11s 0x%08X  %s\n", r.ok ? "OK" : "FAIL", spec.label,
            StrategyName(spec.strategy), r.address, r.detail.c_str());
        if (r.byteCount)
            std::printf("     bytes: %s\n", l4d2vr_x86::HexBytes(r.bytes, 16u).c_str());
        // Load-time relocations (TEXTREL) inside the guarded bytes must all be
        // wildcards, otherwise the runtime guard could never match.
        if (r.ok && spec.guard)
        {
            const auto guard = l4d2vr_x86::Pattern::Parse(spec.guard);
            for (std::uint32_t at : image.RelocationsIn(r.address, static_cast<std::uint32_t>(guard.size())))
            {
                bool wild = true;
                for (std::uint32_t b = at; b < at + 4u; ++b)
                    if (b >= r.address && b - r.address < guard.size() && guard.bytes[b - r.address] >= 0) wild = false;
                std::printf("     relocation at +0x%X: %s\n", at - r.address, wild ? "wildcarded" : "NOT wildcarded");
                if (!wild) { r.ok = false; ++failures; }
            }
        }
        if (spec.strategy == l4d2vr_linux::Strategy::AllVtables)
            for (const auto& slot : r.slots)
                std::printf("     slot 0x%08X index %d\n", slot.slotAddress, slot.index);
        if (!r.ok) ++failures;
    }

    const auto melee = l4d2vr_linux::ExtractMeleeOperands(view, results);
    std::printf("\n%-4s melee operands: %s\n", melee.ok ? "OK" : "FAIL", melee.detail.c_str());
    if (!melee.ok) ++failures;

    l4d2vr_linux::FeatureInputs inputs{};
    inputs.groups = l4d2vr_linux::ResolvedGroups(results);
    inputs.netpropStatus = inputs.netpropAmmo = inputs.netpropShell = inputs.netpropDeploy = inputs.netpropGrenade = true;
    inputs.meleeOperands = melee.ok;
    const auto resolved = l4d2vr_linux::ResolveFeatures(inputs);
    std::printf("\ngroups=0x%X usercmd_decoder=%d interaction=%d features=0x%X (netprops assumed present)\n",
        inputs.groups, resolved.usercmdDecoder ? 1 : 0, resolved.interaction ? 1 : 0, resolved.features);
    for (std::uint32_t bit = 1u; bit <= l4d2vr_server::kFeatureHandThrow; bit <<= 1u)
        std::printf("  %-14s 0x%03X %s\n", l4d2vr_server::FeatureName(bit), bit,
            (resolved.features & bit) ? "resolved" : "unavailable");
    std::printf("\n%s: %d target(s) rejected\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

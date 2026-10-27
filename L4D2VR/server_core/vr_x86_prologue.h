#pragma once

// Minimal IA-32 length decoder and trampoline builder for function-entry
// inline hooks. It understands the instruction forms that appear in GCC/MSVC
// i386 prologues (push/pop, mov/lea/arith with ModRM+SIB+disp, immediates,
// moffs, movss/movzx, segment/operand prefixes, call/jmp rel32) and refuses
// anything else - in particular short/conditional branches and returns inside
// the stolen window, which cannot be relocated safely. std-only.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace l4d2vr_x86
{
    enum class Kind : std::uint8_t { Plain, CallRel32, JmpRel32, Terminal, Unsupported };

    struct Instruction
    {
        std::size_t length = 0u;
        Kind kind = Kind::Unsupported;
        std::size_t relOffset = 0u;   // offset of the rel32 field within the instruction
    };

    namespace detail
    {
        // Returns the ModRM+SIB+displacement byte count starting at `p`, or 0.
        inline std::size_t ModRMLength(const std::uint8_t* p, std::size_t avail)
        {
            if (avail < 1u) return 0u;
            const std::uint8_t modrm = p[0];
            const unsigned mod = modrm >> 6, rm = modrm & 7u;
            std::size_t len = 1u;
            if (mod == 3u) return len;
            if (rm == 4u)
            {
                if (avail < 2u) return 0u;
                const std::uint8_t sib = p[1];
                ++len;
                if (mod == 0u && (sib & 7u) == 5u) len += 4u;
            }
            else if (mod == 0u && rm == 5u)
            {
                len += 4u;
            }
            if (mod == 1u) len += 1u;
            else if (mod == 2u) len += 4u;
            return len <= avail ? len : 0u;
        }
    }

    inline Instruction Decode(const std::uint8_t* code, std::size_t avail)
    {
        Instruction insn{};
        if (!code || avail == 0u) return insn;
        std::size_t i = 0u;
        bool operand16 = false;
        // Prefixes: operand size, REP/REPNE (SSE scalar), FS/GS segment.
        while (i < avail && i < 4u && (code[i] == 0x66u || code[i] == 0xF2u || code[i] == 0xF3u ||
            code[i] == 0x64u || code[i] == 0x65u))
        {
            if (code[i] == 0x66u) operand16 = true;
            ++i;
        }
        if (i >= avail) return insn;
        const std::uint8_t op = code[i++];
        auto finish = [&](std::size_t extra, Kind kind) -> Instruction
        {
            Instruction r{};
            if (i + extra > avail) return r;
            r.length = i + extra;
            r.kind = kind;
            return r;
        };
        auto modrm = [&](std::size_t immediate) -> Instruction
        {
            const std::size_t m = detail::ModRMLength(code + i, avail - i);
            if (m == 0u) return Instruction{};
            return finish(m + immediate, Kind::Plain);
        };
        const std::size_t imm32 = operand16 ? 2u : 4u;

        if (op >= 0x50u && op <= 0x5Fu) return finish(0u, Kind::Plain);           // push/pop r32
        if (op == 0x90u) return finish(0u, Kind::Plain);                          // nop
        if (op >= 0xB8u && op <= 0xBFu) return finish(imm32, Kind::Plain);        // mov r32, imm
        if (op >= 0xB0u && op <= 0xB7u) return finish(1u, Kind::Plain);           // mov r8, imm8
        if (op == 0xA1u || op == 0xA3u) return finish(4u, Kind::Plain);           // mov eax <-> moffs32
        if (op == 0x68u) return finish(imm32, Kind::Plain);                       // push imm32
        if (op == 0x6Au) return finish(1u, Kind::Plain);                          // push imm8
        if (op == 0xE8u)
        {
            Instruction r = finish(4u, Kind::CallRel32);
            r.relOffset = i;
            return r;
        }
        if (op == 0xE9u)
        {
            Instruction r = finish(4u, Kind::JmpRel32);
            r.relOffset = i;
            return r;
        }
        if (op == 0xC3u || op == 0xC2u || op == 0xCCu) return finish(op == 0xC2u ? 2u : 0u, Kind::Terminal);
        switch (op)
        {
        // ALU r/m forms (add/or/adc/sbb/and/sub/xor/cmp) and test/xchg/mov/lea.
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D:
            return modrm(0u);
        case 0x80: case 0x82: case 0x83: case 0xC0: case 0xC1: case 0xC6:
            return modrm(1u);
        case 0x81: case 0xC7: case 0x69:
            return modrm(imm32);
        case 0x6B:
            return modrm(1u);
        case 0xD1: case 0xD3: case 0xD9: case 0xDD:
            return modrm(0u);
        case 0xFF:
        {
            // inc/dec/call/jmp/push r/m. Indirect jmp (/4, /5) ends the flow.
            if (i >= avail) return Instruction{};
            const unsigned reg = (code[i] >> 3) & 7u;
            Instruction r = modrm(0u);
            if (r.length && (reg == 4u || reg == 5u)) r.kind = Kind::Terminal;
            return r;
        }
        case 0x0F:
        {
            if (i >= avail) return Instruction{};
            const std::uint8_t op2 = code[i++];
            switch (op2)
            {
            case 0x10: case 0x11: case 0x28: case 0x29: case 0x2E: case 0x2F: case 0x54: case 0x57:
            case 0x58: case 0x59: case 0x5C: case 0x5E: case 0x6E: case 0x7E: case 0xD6:
            case 0xB6: case 0xB7: case 0xBE: case 0xBF: case 0xAF:
                return modrm(0u);
            default:
                return Instruction{};   // includes jcc rel32 (0x80..0x8F)
            }
        }
        default:
            return Instruction{};       // jcc/jmp rel8, loop, string ops, etc.
        }
    }

    struct Plan
    {
        bool ok = false;
        std::size_t stolen = 0u;                  // bytes copied into the trampoline
        std::vector<std::size_t> relFields;       // offsets of rel32 fields to fix
        std::string error;
    };

    // Plans a hook that overwrites at least `minimum` bytes (5 for jmp rel32).
    inline Plan PlanHook(const std::uint8_t* code, std::size_t avail, std::size_t minimum = 5u)
    {
        Plan plan{};
        std::size_t offset = 0u;
        while (offset < minimum)
        {
            const Instruction insn = Decode(code + offset, avail > offset ? avail - offset : 0u);
            if (insn.kind == Kind::Unsupported || insn.length == 0u)
            {
                char buffer[96];
                std::snprintf(buffer, sizeof(buffer), "unsupported instruction at +%zu (0x%02X)",
                    offset, offset < avail ? code[offset] : 0u);
                plan.error = buffer;
                return plan;
            }
            if (insn.kind == Kind::Terminal || insn.kind == Kind::JmpRel32)
            {
                // Control leaves the function inside the stolen window; only safe
                // if it is the last stolen instruction and covers the minimum.
                if (offset + insn.length < minimum)
                {
                    plan.error = "control transfer inside the patch window";
                    return plan;
                }
            }
            if (insn.kind == Kind::CallRel32 || insn.kind == Kind::JmpRel32)
                plan.relFields.push_back(offset + insn.relOffset);
            offset += insn.length;
        }
        plan.ok = true;
        plan.stolen = offset;
        return plan;
    }

    inline std::int32_t ReadI32(const std::uint8_t* p)
    {
        std::int32_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }

    inline void WriteI32(std::uint8_t* p, std::int32_t v)
    {
        std::memcpy(p, &v, sizeof(v));
    }

    // Writes the trampoline: relocated stolen bytes + jmp back. `dst` needs
    // plan.stolen + 5 bytes. Addresses are the runtime addresses of src/dst.
    inline bool BuildTrampoline(const std::uint8_t* src, std::uint32_t srcAddress, const Plan& plan,
        std::uint8_t* dst, std::uint32_t dstAddress)
    {
        if (!plan.ok || plan.stolen < 5u) return false;
        std::memcpy(dst, src, plan.stolen);
        for (std::size_t field : plan.relFields)
        {
            const std::uint32_t nextOld = srcAddress + static_cast<std::uint32_t>(field + 4u);
            const std::uint32_t target = nextOld + static_cast<std::uint32_t>(ReadI32(src + field));
            const std::uint32_t nextNew = dstAddress + static_cast<std::uint32_t>(field + 4u);
            WriteI32(dst + field, static_cast<std::int32_t>(target - nextNew));
        }
        dst[plan.stolen] = 0xE9u;
        const std::uint32_t back = srcAddress + static_cast<std::uint32_t>(plan.stolen);
        WriteI32(dst + plan.stolen + 1u, static_cast<std::int32_t>(back - (dstAddress + static_cast<std::uint32_t>(plan.stolen) + 5u)));
        return true;
    }

    // Byte pattern with "??" wildcards, e.g. "55 89 E5 57 56 53 83 EC ??".
    struct Pattern
    {
        std::vector<std::int16_t> bytes;   // -1 = wildcard

        static Pattern Parse(const char* text)
        {
            Pattern p{};
            if (!text) return p;
            while (*text)
            {
                while (*text == ' ') ++text;
                if (!*text) break;
                if (text[0] == '?' )
                {
                    p.bytes.push_back(-1);
                    text += (text[1] == '?') ? 2 : 1;
                    continue;
                }
                auto hex = [](char c) -> int
                {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                const int hi = hex(text[0]), lo = text[1] ? hex(text[1]) : -1;
                if (hi < 0 || lo < 0) { p.bytes.clear(); return p; }
                p.bytes.push_back(static_cast<std::int16_t>(hi * 16 + lo));
                text += 2;
            }
            return p;
        }

        std::size_t size() const { return bytes.size(); }

        bool Matches(const std::uint8_t* code, std::size_t avail) const
        {
            if (bytes.empty() || avail < bytes.size()) return false;
            for (std::size_t i = 0; i < bytes.size(); ++i)
                if (bytes[i] >= 0 && code[i] != static_cast<std::uint8_t>(bytes[i])) return false;
            return true;
        }

        // Finds the pattern within [code, code+avail); returns offset or -1.
        long Find(const std::uint8_t* code, std::size_t avail) const
        {
            if (bytes.empty() || avail < bytes.size()) return -1;
            for (std::size_t off = 0; off + bytes.size() <= avail; ++off)
                if (Matches(code + off, avail - off)) return static_cast<long>(off);
            return -1;
        }
    };

    inline std::string HexBytes(const std::uint8_t* code, std::size_t count)
    {
        std::string out;
        char b[4];
        for (std::size_t i = 0; i < count; ++i)
        {
            std::snprintf(b, sizeof(b), i ? " %02X" : "%02X", code[i]);
            out += b;
        }
        return out;
    }
}

#pragma once

// Bounds-checked reader for 32-bit little-endian ELF shared objects: symbol
// lookup in .symtab (local symbols) and .dynsym, and virtual-address reads.
// The Linux plugin parses the on-disk server_srv.so that dl_iterate_phdr
// reports and adds the runtime load base; the self-test tool uses the same
// code on a copy of the binary. std-only (own struct definitions).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace l4d2vr_elf
{
    struct Symbol
    {
        std::uint32_t value = 0u;
        std::uint32_t size = 0u;
        std::uint8_t type = 0u;     // STT_*
        std::uint8_t bind = 0u;     // STB_*
        std::uint16_t section = 0u;
        int duplicates = 0;          // other definitions with the same name and a different value
    };

    constexpr std::uint8_t kSttObject = 1u;
    constexpr std::uint8_t kSttFunc = 2u;

    class Image
    {
    public:
        bool LoadFile(const std::string& path, std::string& error)
        {
            m_Data.clear();
            m_Symbols.clear();
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) { error = "cannot open " + path; return false; }
            if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); error = "seek failed"; return false; }
            const long length = std::ftell(f);
            if (length <= 0 || length > (512L << 20)) { std::fclose(f); error = "bad file size"; return false; }
            std::rewind(f);
            m_Data.resize(static_cast<std::size_t>(length));
            const std::size_t got = std::fread(m_Data.data(), 1, m_Data.size(), f);
            std::fclose(f);
            if (got != m_Data.size()) { error = "short read"; return false; }
            return Parse(error);
        }

        bool LoadBytes(std::vector<std::uint8_t> bytes, std::string& error)
        {
            m_Data = std::move(bytes);
            m_Symbols.clear();
            return Parse(error);
        }

        const Symbol* Find(const std::string& name) const
        {
            const auto it = m_Symbols.find(name);
            return it == m_Symbols.end() ? nullptr : &it->second;
        }

        // Unique function/object symbol lookup (ambiguous names are refused).
        bool Resolve(const std::string& name, Symbol& out) const
        {
            const Symbol* s = Find(name);
            if (!s || s->value == 0u || s->duplicates != 0) return false;
            out = *s;
            return true;
        }

        template <class Fn> void ForEachSymbol(Fn fn) const
        {
            for (const auto& kv : m_Symbols) fn(kv.first, kv.second);
        }

        // File bytes at a link-time virtual address (no relocations applied).
        bool ReadVirtual(std::uint32_t vaddr, void* out, std::size_t size) const
        {
            std::size_t offset = 0u;
            if (!VirtualToOffset(vaddr, size, offset)) return false;
            std::memcpy(out, m_Data.data() + offset, size);
            return true;
        }

        const std::uint8_t* PointerAt(std::uint32_t vaddr, std::size_t size) const
        {
            std::size_t offset = 0u;
            return VirtualToOffset(vaddr, size, offset) ? m_Data.data() + offset : nullptr;
        }

        bool IsExecutable(std::uint32_t vaddr) const
        {
            for (const Segment& s : m_Segments)
                if ((s.flags & 1u) != 0u && vaddr >= s.vaddr && vaddr - s.vaddr < s.memsz) return true;
            return false;
        }

        std::size_t SymbolCount() const { return m_Symbols.size(); }

        // Dynamic relocations (.rel.dyn/.rel.plt, i386 REL) touching
        // [vaddr, vaddr + size): each relocated 32-bit field is reported by
        // its starting address.
        std::vector<std::uint32_t> RelocationsIn(std::uint32_t vaddr, std::uint32_t size) const
        {
            std::vector<std::uint32_t> out;
            for (std::uint32_t r : m_Relocations)
                if (r + 4u > vaddr && r < vaddr + size) out.push_back(r);
            return out;
        }

    private:
        struct Segment { std::uint32_t vaddr, memsz, offset, filesz, flags; };

        template <class T> bool ReadAt(std::size_t offset, T& out) const
        {
            if (offset > m_Data.size() || sizeof(T) > m_Data.size() - offset) return false;
            std::memcpy(&out, m_Data.data() + offset, sizeof(T));
            return true;
        }

        bool VirtualToOffset(std::uint32_t vaddr, std::size_t size, std::size_t& offset) const
        {
            for (const Segment& s : m_Segments)
            {
                if (vaddr < s.vaddr) continue;
                const std::uint32_t delta = vaddr - s.vaddr;
                if (delta >= s.filesz || size > s.filesz - delta) continue;
                offset = static_cast<std::size_t>(s.offset) + delta;
                return offset <= m_Data.size() && size <= m_Data.size() - offset;
            }
            return false;
        }

        bool Parse(std::string& error)
        {
            m_Segments.clear();
            m_Relocations.clear();
            std::uint8_t ident[16]{};
            if (!ReadAt(0, ident) || std::memcmp(ident, "\x7f" "ELF", 4) != 0 || ident[4] != 1 || ident[5] != 1)
            { error = "not a 32-bit little-endian ELF"; return false; }
            std::uint16_t machine = 0, phentsize = 0, phnum = 0, shentsize = 0, shnum = 0;
            std::uint32_t phoff = 0, shoff = 0;
            if (!ReadAt(18, machine) || !ReadAt(28, phoff) || !ReadAt(32, shoff) || !ReadAt(42, phentsize) ||
                !ReadAt(44, phnum) || !ReadAt(46, shentsize) || !ReadAt(48, shnum))
            { error = "truncated header"; return false; }
            if (machine != 3u) { error = "not i386"; return false; }
            if (phentsize != 32u || shentsize != 40u) { error = "unexpected header sizes"; return false; }
            for (std::uint16_t i = 0; i < phnum; ++i)
            {
                const std::size_t base = static_cast<std::size_t>(phoff) + i * 32u;
                std::uint32_t type = 0, offset = 0, vaddr = 0, filesz = 0, memsz = 0, flags = 0;
                if (!ReadAt(base, type) || !ReadAt(base + 4, offset) || !ReadAt(base + 8, vaddr) ||
                    !ReadAt(base + 16, filesz) || !ReadAt(base + 20, memsz) || !ReadAt(base + 24, flags))
                { error = "truncated program header"; return false; }
                if (type == 1u) m_Segments.push_back({ vaddr, memsz, offset, filesz, flags });
            }
            for (std::uint16_t i = 0; i < shnum; ++i)
            {
                const std::size_t base = static_cast<std::size_t>(shoff) + i * 40u;
                std::uint32_t type = 0, offset = 0, size = 0, link = 0, entsize = 0;
                if (!ReadAt(base + 4, type) || !ReadAt(base + 16, offset) || !ReadAt(base + 20, size) ||
                    !ReadAt(base + 24, link) || !ReadAt(base + 36, entsize))
                { error = "truncated section header"; return false; }
                if (type == 9u && entsize == 8u && offset <= m_Data.size() && size <= m_Data.size() - offset)  // SHT_REL
                {
                    for (std::uint32_t e = 0; e + 8u <= size; e += 8u)
                    {
                        std::uint32_t where = 0, info = 0;
                        ReadAt(offset + e, where);
                        ReadAt(offset + e + 4, info);
                        if ((info & 0xFFu) != 0u) m_Relocations.push_back(where);   // skip R_386_NONE
                    }
                    continue;
                }
                if ((type != 2u && type != 11u) || entsize != 16u || link >= shnum) continue;   // SYMTAB/DYNSYM
                std::uint32_t strOffset = 0, strSize = 0;
                const std::size_t strBase = static_cast<std::size_t>(shoff) + link * 40u;
                if (!ReadAt(strBase + 16, strOffset) || !ReadAt(strBase + 20, strSize)) continue;
                if (strOffset > m_Data.size() || strSize > m_Data.size() - strOffset) continue;
                if (offset > m_Data.size() || size > m_Data.size() - offset) continue;
                const char* strings = reinterpret_cast<const char*>(m_Data.data() + strOffset);
                for (std::uint32_t s = 0; s + 16u <= size; s += 16u)
                {
                    std::uint32_t name = 0, value = 0, symSize = 0;
                    std::uint8_t info = 0;
                    std::uint16_t shndx = 0;
                    ReadAt(offset + s, name);
                    ReadAt(offset + s + 4, value);
                    ReadAt(offset + s + 8, symSize);
                    ReadAt(offset + s + 12, info);
                    ReadAt(offset + s + 14, shndx);
                    if (name == 0u || name >= strSize || shndx == 0u || value == 0u) continue;
                    const std::size_t maxLen = strSize - name;
                    std::size_t len = 0u;
                    while (len < maxLen && strings[name + len] != '\0') ++len;
                    if (len == maxLen) continue;
                    Symbol symbol{};
                    symbol.value = value;
                    symbol.size = symSize;
                    symbol.type = static_cast<std::uint8_t>(info & 0x0Fu);
                    symbol.bind = static_cast<std::uint8_t>(info >> 4);
                    symbol.section = shndx;
                    auto inserted = m_Symbols.emplace(std::string(strings + name, len), symbol);
                    if (!inserted.second && inserted.first->second.value != value)
                        ++inserted.first->second.duplicates;
                }
            }
            if (m_Symbols.empty()) { error = "no symbols"; return false; }
            return true;
        }

        std::vector<std::uint8_t> m_Data;
        std::vector<Segment> m_Segments;
        std::vector<std::uint32_t> m_Relocations;
        std::unordered_map<std::string, Symbol> m_Symbols;
    };
}

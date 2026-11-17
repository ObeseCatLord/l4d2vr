#pragma once

// "Save logs for sharing" (2.0.12): copies the files a bug report needs into
// <game>\VR\shared-logs\l4d2vr-logs-<YYYYMMDD-HHMMSS>\ and packs the same
// files into a .zip next to it, so a player can send one file.
//
// std-only (std::filesystem, no Windows.h): the file selection, the size caps,
// the redaction of secrets and the zip writer are unit tested on Linux
// (tests/vr_log_share_tests.cpp). game_log_share.inl gathers the Windows
// specific paths and system facts and runs Collect() on a worker thread.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace l4d2vr_logshare
{
    // Whole folder (and therefore the zip) never grows past this.
    inline constexpr uint64_t kTotalCapBytes = 40ull * 1024 * 1024;

    struct Item
    {
        std::filesystem::path source;
        std::string name;          // file name inside the shared folder
        uint64_t maxBytes = 0;     // larger files keep their last maxBytes (whole lines)
        bool redact = true;        // drop lines that carry passwords or tokens
    };

    struct Generated
    {
        std::string name;
        std::string text;
    };

    struct Result
    {
        bool ok = false;
        std::filesystem::path folder;
        std::filesystem::path zip;   // empty when the zip could not be written
        int files = 0;
        uint64_t bytes = 0;
        int redactedLines = 0;
        std::vector<std::string> notes; // "console.log: last 5.0 MB of 61.2 MB", "debug.log: not found"
    };

    inline std::string FolderName(const std::tm& t)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "l4d2vr-logs-%04d%02d%02d-%02d%02d%02d",
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        return buffer;
    }

    inline bool ContainsNoCase(std::string_view hay, std::string_view needle)
    {
        if (needle.empty() || hay.size() < needle.size())
            return false;
        for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        {
            size_t j = 0;
            while (j < needle.size() &&
                std::tolower(static_cast<unsigned char>(hay[i + j])) == std::tolower(static_cast<unsigned char>(needle[j])))
                ++j;
            if (j == needle.size())
                return true;
        }
        return false;
    }

    // A line that could carry a secret: server/rcon passwords (rcon_password,
    // sv_password, sm_... password cvars), Steam auth tickets, login tokens,
    // cookies. Such a line is replaced as a whole; nothing else is touched.
    inline bool IsSecretLine(std::string_view line)
    {
        // Specific words only: "token" alone would remove the mod's own
        // diagnostic lines ("poseToken=", "eyes token=").
        static constexpr std::string_view kNeedles[] = {
            "password", "passwd", "rcon_", "secret", "cookie", "authticket", "auth_ticket", "steamauth",
            "access_token", "auth_token", "authtoken", "refresh_token", "sessionid", "api_key", "apikey",
            "sv_search_key", "webapi",
        };
        for (std::string_view needle : kNeedles)
            if (ContainsNoCase(line, needle))
                return true;
        return false;
    }

    inline std::string RedactText(std::string_view text, int* redactedLines = nullptr)
    {
        std::string out;
        out.reserve(text.size());
        int redacted = 0;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find('\n', pos);
            const bool hasNewline = end != std::string_view::npos;
            if (!hasNewline)
                end = text.size();
            const std::string_view line = text.substr(pos, end - pos);
            if (IsSecretLine(line))
            {
                out += "[l4d2vr: line removed, it may contain a password or token]";
                if (!line.empty() && line.back() == '\r')
                    out += '\r';
                ++redacted;
            }
            else
                out.append(line.data(), line.size());
            if (hasNewline)
                out += '\n';
            pos = end + 1;
        }
        if (redactedLines)
            *redactedLines += redacted;
        return out;
    }

    // Start of the kept tail: the last maxBytes, moved forward to the next
    // line start so no half line opens the file.
    inline size_t TailStart(std::string_view text, uint64_t maxBytes)
    {
        if (maxBytes == 0 || text.size() <= maxBytes)
            return 0;
        size_t start = text.size() - static_cast<size_t>(maxBytes);
        const size_t newline = text.find('\n', start);
        if (newline != std::string_view::npos && newline + 1 < text.size())
            start = newline + 1;
        return start;
    }

    // Per-file byte budgets: each file at most its cap; when the files
    // together exceed totalCap, the largest budgets are cut first until the
    // total fits (small files such as the config are never squeezed out).
    inline std::vector<uint64_t> Budgets(const std::vector<uint64_t>& sizes, const std::vector<uint64_t>& caps, uint64_t totalCap)
    {
        std::vector<uint64_t> budget(sizes.size());
        uint64_t total = 0;
        for (size_t i = 0; i < sizes.size(); ++i)
        {
            const uint64_t cap = i < caps.size() && caps[i] > 0 ? caps[i] : sizes[i];
            budget[i] = (std::min)(sizes[i], cap);
            total += budget[i];
        }
        while (total > totalCap)
        {
            size_t largest = 0;
            for (size_t i = 1; i < budget.size(); ++i)
                if (budget[i] > budget[largest])
                    largest = i;
            uint64_t second = 0;
            for (size_t i = 0; i < budget.size(); ++i)
                if (i != largest)
                    second = (std::max)(second, budget[i]);
            const uint64_t excess = total - totalCap;
            const uint64_t cut = (std::min)(excess, (std::max)(budget[largest] - second, static_cast<uint64_t>(1)));
            const uint64_t applied = (std::min)(cut, budget[largest]);
            if (applied == 0)
                break;
            budget[largest] -= applied;
            total -= applied;
        }
        return budget;
    }

    // Lines of vrmod_log.txt that describe the system (mod build, Windows or
    // Proton, OpenXR runtime and headset, Vulkan device), for system-info.txt.
    inline std::string ExtractSystemLines(std::string_view log, size_t maxLines = 24)
    {
        static constexpr std::string_view kMarkers[] = {
            "] L4D2VR ", "[VR] VR runtime", "[VR][OpenXR] runtime registration", "[VR][OpenXR] instance ready",
            "[VR][OpenXR] session", "[VR][Vulkan]", "[DXVK]", "GPU", "Adapter", "desktop mode",
        };
        std::string out;
        size_t lines = 0;
        size_t pos = 0;
        while (pos < log.size() && lines < maxLines)
        {
            size_t end = log.find('\n', pos);
            if (end == std::string_view::npos)
                end = log.size();
            std::string_view line = log.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            for (std::string_view marker : kMarkers)
                if (line.find(marker) != std::string_view::npos && !IsSecretLine(line))
                {
                    out.append(line.data(), (std::min)(line.size(), static_cast<size_t>(600)));
                    out += '\n';
                    ++lines;
                    break;
                }
            pos = end + 1;
        }
        return out;
    }

    inline std::string LastLineContaining(std::string_view text, std::string_view needle)
    {
        const size_t at = text.rfind(needle);
        if (at == std::string_view::npos)
            return {};
        const size_t lineStart = text.rfind('\n', at);
        const size_t start = lineStart == std::string_view::npos ? 0 : lineStart + 1;
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos)
            end = text.size();
        std::string line(text.substr(start, end - start));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        return IsSecretLine(line) ? std::string() : line;
    }

    inline std::string ReadmeText(const std::string& folderName, const std::vector<std::string>& files,
        const std::vector<std::string>& notes)
    {
        std::string out;
        out += "L4D2VR logs for a bug report (" + folderName + ")\n\n";
        out += "Send the file " + folderName + ".zip (next to this folder) or this whole folder to whoever\n";
        out += "asked for your logs, or attach it to an issue at https://github.com/ObeseCatLord/l4d2vr/issues,\n";
        out += "and say what happened and roughly when (the logs have the time of every line).\n\n";
        out += "What is inside:\n";
        auto describe = [](const std::string& name) -> const char*
            {
                if (name == "vrmod_log.txt") return "the mod's log of this game session";
                if (name == "vrmod_log.prev.txt") return "the mod's log of the previous game session";
                if (name == "config.txt") return "your VR settings (VR\\config.txt)";
                if (name == "version.txt") return "the installed mod version";
                if (name == "config_migrations_applied.txt") return "which one-time setting updates already ran";
                if (name == "vr_saved_game_cvars.txt") return "game settings the mod changed while in VR, saved to restore them";
                if (name == "console.log") return "the end of the game's console log (left4dead2\\console.log)";
                if (name == "debug.log") return "the end of the game's crash/debug log";
                if (name == "left4dead2_d3d9.log") return "the renderer's (DXVK) log";
                if (name == "system-info.txt") return "mod version, Windows or Proton, VR runtime, GPU, connection";
                return "";
            };
        for (const std::string& file : files)
        {
            out += "  " + file;
            const char* what = describe(file);
            if (*what)
                out += std::string(" - ") + what;
            out += '\n';
        }
        out += "\nNothing secret is included: no Steam login data, and every line that mentions a password,\n";
        out += "an rcon setting, a cookie or an auth token was replaced by \"[l4d2vr: line removed ...]\". Large logs keep only their end.\n";
        if (!notes.empty())
        {
            out += "\nNotes:\n";
            for (const std::string& note : notes)
                out += "  " + note + '\n';
        }
        return out;
    }

    // ---- Minimal zip writer: stored (uncompressed) entries, UTF-8 names ----
    inline uint32_t Crc32(const void* data, size_t size, uint32_t crc = 0)
    {
        static const auto table = []() {
            std::vector<uint32_t> t(256);
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        const auto* p = static_cast<const unsigned char*>(data);
        crc = ~crc;
        for (size_t i = 0; i < size; ++i)
            crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
        return ~crc;
    }

    class StoredZipWriter
    {
    public:
        bool Open(const std::filesystem::path& path, const std::tm& when)
        {
            m_Out.open(path, std::ios::binary | std::ios::trunc);
            m_DosTime = static_cast<uint16_t>((when.tm_hour << 11) | (when.tm_min << 5) | (when.tm_sec / 2));
            const int year = (std::max)(1980, when.tm_year + 1900);
            m_DosDate = static_cast<uint16_t>(((year - 1980) << 9) | ((when.tm_mon + 1) << 5) | when.tm_mday);
            return m_Out.good();
        }

        bool Add(const std::string& name, const std::string& data)
        {
            if (!m_Out.good() || data.size() > 0xFFFFFFF0ull || m_Offset > 0xFFFFFFF0ull)
                return false;
            Entry e;
            e.name = name;
            e.crc = Crc32(data.data(), data.size());
            e.size = static_cast<uint32_t>(data.size());
            e.offset = static_cast<uint32_t>(m_Offset);
            U32(0x04034b50u); U16(20); U16(0x0800); U16(0); U16(m_DosTime); U16(m_DosDate);
            U32(e.crc); U32(e.size); U32(e.size); U16(static_cast<uint16_t>(name.size())); U16(0);
            Bytes(name.data(), name.size());
            Bytes(data.data(), data.size());
            m_Entries.push_back(e);
            return m_Out.good();
        }

        bool Close()
        {
            if (!m_Out.is_open())
                return false;
            const uint64_t directoryStart = m_Offset;
            for (const Entry& e : m_Entries)
            {
                U32(0x02014b50u); U16(20); U16(20); U16(0x0800); U16(0); U16(m_DosTime); U16(m_DosDate);
                U32(e.crc); U32(e.size); U32(e.size); U16(static_cast<uint16_t>(e.name.size()));
                U16(0); U16(0); U16(0); U16(0); U32(0); U32(e.offset);
                Bytes(e.name.data(), e.name.size());
            }
            const uint64_t directorySize = m_Offset - directoryStart;
            U32(0x06054b50u); U16(0); U16(0);
            U16(static_cast<uint16_t>(m_Entries.size())); U16(static_cast<uint16_t>(m_Entries.size()));
            U32(static_cast<uint32_t>(directorySize)); U32(static_cast<uint32_t>(directoryStart)); U16(0);
            const bool ok = m_Out.good();
            m_Out.close();
            return ok;
        }

    private:
        struct Entry { std::string name; uint32_t crc = 0, size = 0, offset = 0; };
        void Bytes(const void* p, size_t n) { m_Out.write(static_cast<const char*>(p), static_cast<std::streamsize>(n)); m_Offset += n; }
        void U16(uint16_t v) { const unsigned char b[2] = { (unsigned char)(v & 0xFF), (unsigned char)(v >> 8) }; Bytes(b, 2); }
        void U32(uint32_t v) { U16(static_cast<uint16_t>(v & 0xFFFF)); U16(static_cast<uint16_t>(v >> 16)); }
        std::ofstream m_Out;
        std::vector<Entry> m_Entries;
        uint64_t m_Offset = 0;
        uint16_t m_DosTime = 0, m_DosDate = 0;
    };

    inline std::string FormatMb(uint64_t bytes)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", bytes / (1024.0 * 1024.0));
        return buffer;
    }

    // Reads at most the last maxBytes of a file (whole lines). The game and
    // the mod keep their logs open: plain shared reads, never a lock.
    inline bool ReadTail(const std::filesystem::path& path, uint64_t maxBytes, std::string& out, uint64_t& fileSize)
    {
        out.clear();
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        in.seekg(0, std::ios::end);
        const std::streamoff end = in.tellg();
        if (end < 0)
            return false;
        fileSize = static_cast<uint64_t>(end);
        // Read a little more than the budget so the tail can start on a line.
        const uint64_t want = maxBytes > 0 ? (std::min)(fileSize, maxBytes + 4096) : fileSize;
        in.seekg(static_cast<std::streamoff>(fileSize - want), std::ios::beg);
        out.resize(static_cast<size_t>(want));
        if (want > 0)
            in.read(&out[0], static_cast<std::streamsize>(want));
        out.resize(static_cast<size_t>(in.gcount()));
        if (maxBytes > 0 && out.size() > maxBytes)
            out.erase(0, TailStart(out, maxBytes));
        return true;
    }

    // Copies the items (redacted, capped) and the generated files into
    // outRoot/folderName and, when makeZip, packs them into folderName.zip.
    // Missing files are noted, not errors.
    inline Result Collect(const std::vector<Item>& items, const std::vector<Generated>& generated,
        const std::filesystem::path& outRoot, const std::string& folderName, const std::tm& when,
        bool makeZip, uint64_t totalCap = kTotalCapBytes)
    {
        Result result;
        std::error_code ec;
        result.folder = outRoot / folderName;
        std::filesystem::create_directories(result.folder, ec);
        if (ec)
        {
            result.notes.push_back("cannot create the folder: " + ec.message());
            return result;
        }

        std::vector<uint64_t> sizes(items.size(), 0), caps(items.size(), 0);
        std::vector<bool> present(items.size(), false);
        uint64_t generatedBytes = 0;
        for (const Generated& g : generated)
            generatedBytes += g.text.size();
        for (size_t i = 0; i < items.size(); ++i)
        {
            const uint64_t size = std::filesystem::file_size(items[i].source, ec);
            present[i] = !ec;
            sizes[i] = present[i] ? size : 0;
            caps[i] = items[i].maxBytes;
        }
        const uint64_t room = totalCap > generatedBytes + 64 * 1024 ? totalCap - generatedBytes - 64 * 1024 : 0;
        const std::vector<uint64_t> budgets = Budgets(sizes, caps, room);

        StoredZipWriter zip;
        bool zipOk = makeZip && zip.Open(outRoot / (folderName + ".zip"), when);
        std::vector<std::string> names;
        auto emit = [&](const std::string& name, const std::string& data) -> bool
            {
                std::ofstream out(result.folder / name, std::ios::binary | std::ios::trunc);
                out.write(data.data(), static_cast<std::streamsize>(data.size()));
                if (!out.good())
                {
                    result.notes.push_back(name + ": could not be written");
                    return false;
                }
                ++result.files;
                result.bytes += data.size();
                names.push_back(name);
                if (zipOk)
                    zipOk = zip.Add(name, data);
                return true;
            };

        for (size_t i = 0; i < items.size(); ++i)
        {
            const Item& item = items[i];
            if (!present[i])
            {
                result.notes.push_back(item.name + ": not found");
                continue;
            }
            if (budgets[i] == 0 && sizes[i] > 0)
            {
                result.notes.push_back(item.name + ": left out (size cap)");
                continue;
            }
            std::string data;
            uint64_t fileSize = 0;
            if (!ReadTail(item.source, budgets[i], data, fileSize))
            {
                result.notes.push_back(item.name + ": could not be read");
                continue;
            }
            if (fileSize > data.size())
                result.notes.push_back(item.name + ": last " + FormatMb(data.size()) + " of " + FormatMb(fileSize));
            if (item.redact)
                data = RedactText(data, &result.redactedLines);
            emit(item.name, data);
        }
        for (const Generated& g : generated)
            if (g.name != "README.txt")
                emit(g.name, RedactText(g.text, &result.redactedLines));
        if (result.redactedLines > 0)
            result.notes.push_back(std::to_string(result.redactedLines) + " line(s) that mention a password, an rcon setting or a token were removed");
        std::vector<std::string> readmeFiles = names;
        readmeFiles.push_back("README.txt");
        std::string readme;
        for (const Generated& g : generated)
            if (g.name == "README.txt")
                readme = g.text;
        if (readme.empty())
            readme = ReadmeText(folderName, readmeFiles, result.notes);
        emit("README.txt", readme);

        if (makeZip)
        {
            zipOk = zip.Close() && zipOk;
            if (zipOk)
                result.zip = outRoot / (folderName + ".zip");
            else
            {
                std::filesystem::remove(outRoot / (folderName + ".zip"), ec);
                result.notes.push_back("the .zip could not be written; send the folder instead");
            }
        }
        result.ok = result.files > 0;
        return result;
    }
}

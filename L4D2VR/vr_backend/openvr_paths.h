#pragma once

// The OpenVR runtime's install folder from openvrpaths.vrpath, without
// openvr_api (OpenXR port M6: the release DLL no longer links it).
//
// The OpenXR backend reports it through IVrBackend::GetRuntimePath so the
// SteamVR glove meshes load where SteamVR is installed, as with OpenVR's
// VR_GetRuntimePath, which reads the same file: the first entry of the
// "runtime" array in %LOCALAPPDATA%\openvr\openvrpaths.vrpath (under Proton
// that is Proton's own C:\vrclient stub). Std-only for the unit tests
// (tests/vr_openvr_paths_tests.cpp).

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace l4d2vr_openvr_paths
{
    namespace detail
    {
        inline void SkipSpace(std::string_view text, size_t& i)
        {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n'))
                ++i;
        }

        inline void AppendUtf8(std::string& out, unsigned code)
        {
            if (code < 0x80)
                out.push_back(static_cast<char>(code));
            else if (code < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
        }

        // Parses a JSON string starting at text[i] == '"'. False on malformed input.
        inline bool ParseString(std::string_view text, size_t& i, std::string& out)
        {
            out.clear();
            if (i >= text.size() || text[i] != '"')
                return false;
            ++i;
            while (i < text.size())
            {
                const char c = text[i++];
                if (c == '"')
                    return true;
                if (c != '\\')
                {
                    out.push_back(c);
                    continue;
                }
                if (i >= text.size())
                    return false;
                const char e = text[i++];
                switch (e)
                {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                {
                    if (i + 4 > text.size())
                        return false;
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k)
                    {
                        const char h = text[i++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    AppendUtf8(out, code);
                    break;
                }
                default:
                    return false;
                }
            }
            return false;
        }
    }

    // The entries of the top-level "runtime" array, in order. Empty when the
    // key is missing or the file is malformed.
    inline std::vector<std::string> RuntimeEntries(std::string_view json)
    {
        std::vector<std::string> out;
        size_t i = 0;
        detail::SkipSpace(json, i);
        if (i >= json.size() || json[i] != '{')
            return out;
        ++i;
        int depth = 1;
        std::string key;
        while (i < json.size() && depth > 0)
        {
            detail::SkipSpace(json, i);
            if (i >= json.size())
                break;
            const char c = json[i];
            if (c == '"')
            {
                if (!detail::ParseString(json, i, key))
                    return {};
                detail::SkipSpace(json, i);
                if (depth == 1 && i < json.size() && json[i] == ':')
                {
                    ++i;
                    detail::SkipSpace(json, i);
                    if (key == "runtime" && i < json.size() && json[i] == '[')
                    {
                        ++i;
                        std::vector<std::string> entries;
                        for (;;)
                        {
                            detail::SkipSpace(json, i);
                            if (i >= json.size())
                                return {};
                            if (json[i] == ']')
                                return entries;
                            std::string value;
                            if (!detail::ParseString(json, i, value))
                                return {};
                            entries.push_back(value);
                            detail::SkipSpace(json, i);
                            if (i < json.size() && json[i] == ',')
                                ++i;
                        }
                    }
                }
                continue;
            }
            if (c == '{' || c == '[')
                ++depth;
            else if (c == '}' || c == ']')
                --depth;
            ++i;
        }
        return out;
    }

    // The first runtime folder without a trailing separator, or empty.
    inline std::string FirstRuntime(std::string_view json)
    {
        std::vector<std::string> entries = RuntimeEntries(json);
        for (std::string& entry : entries)
        {
            while (entry.size() > 1 && (entry.back() == '\\' || entry.back() == '/'))
                entry.pop_back();
            if (!entry.empty())
                return entry;
        }
        return std::string();
    }
}

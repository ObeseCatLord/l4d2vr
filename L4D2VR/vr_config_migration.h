#pragma once
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

// One-shot config migrations (dllmain.cpp) replace an OLD SHIPPED value only,
// never one the player tuned. Old values reached players' configs from the
// shipped sample, from the code default the config panel wrote when a key was
// missing, and from upstream builds, in their own spelling ("1" vs "1.0",
// "-1" vs "-1.0"), so values compare by meaning: booleans as the parser reads
// them (true/1/on/yes, case-insensitive),
// numbers and number lists within 1e-4, anything else as trimmed text.
namespace l4d2vr_config_migration
{
    inline std::string Trim(const std::string& s)
    {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
            ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
            --e;
        return s.substr(b, e - b);
    }

    inline std::string Lower(std::string s)
    {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return s;
    }

    // "0.4, 0.2,-0.27" -> {0.4, 0.2, -0.27}; false unless every part is a number.
    inline bool ParseNumbers(const std::string& text, std::vector<double>& out)
    {
        out.clear();
        const std::string t = Trim(text);
        if (t.empty())
            return false;
        size_t start = 0;
        while (start <= t.size())
        {
            const size_t comma = t.find(',', start);
            const std::string part = Trim(t.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (part.empty())
                return false;
            char* end = nullptr;
            const double v = std::strtod(part.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(v))
                return false;
            out.push_back(v);
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return !out.empty();
    }

    // The parser's boolean spellings (getBool): 1/true/on/yes and 0/false/off/no.
    // Returns -1 for anything else.
    inline int BoolValue(const std::string& lower)
    {
        if (lower == "1" || lower == "true" || lower == "on" || lower == "yes")
            return 1;
        if (lower == "0" || lower == "false" || lower == "off" || lower == "no")
            return 0;
        return -1;
    }

    inline bool IsBoolWord(const std::string& lower)
    {
        return lower == "true" || lower == "false" || lower == "on" || lower == "off" || lower == "yes" || lower == "no";
    }

    inline bool ValuesMatch(const std::string& a, const std::string& b)
    {
        const std::string ta = Trim(a), tb = Trim(b);
        const std::string la = Lower(ta), lb = Lower(tb);
        // A boolean on either side: compare as the parser reads booleans, so a
        // hand-edited "0" or "off" still matches an old shipped "false"
        // (2.0.11, review B70).
        if (IsBoolWord(la) || IsBoolWord(lb))
        {
            const int va = BoolValue(la), vb = BoolValue(lb);
            return va >= 0 && va == vb;
        }
        std::vector<double> na, nb;
        if (ParseNumbers(ta, na) && ParseNumbers(tb, nb))
        {
            if (na.size() != nb.size())
                return false;
            for (size_t i = 0; i < na.size(); ++i)
                if (std::fabs(na[i] - nb[i]) > 1.0e-4)
                    return false;
            return true;
        }
        return ta == tb;
    }

    // Replace the player's value? Only a missing key or the old shipped value.
    inline bool ShouldReplace(bool present, const std::string& current, const std::string& oldShipped)
    {
        return !present || ValuesMatch(current, oldShipped);
    }
}

#pragma once

// vrmod_log.txt volume (2.0.12, review s12 E20): a few always-on diagnostic
// lines produced most of a 3 MB/hour log (muzzle smoke captures, grip reads,
// melee bash candidates, viewmodel freezes, holster draw counters). Each call
// site of those lines (keyed by its format string) is limited to one line
// per interval unless VrHandsDebugLog is on; the next line that passes says
// how many were left out. State transitions and refusals are untouched.
// std-only so the rules are unit tested.
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace l4d2vr_logfilter
{
    struct Rule
    {
        std::string_view prefix;
        uint32_t intervalMs;
    };

    inline constexpr Rule kRules[] = {
        { "[VR][FX][muzzlesmoke] capture", 30000u },
        { "[VR][GripInput]", 10000u },
        { "[VR][Melee] bash candidate", 5000u },
        { "[VR][ViewmodelFreeze]", 10000u },
        { "[VR][HolsterDraw]", 15000u },
        { "[VR][VRIK] remote crouch", 10000u },
    };

    inline const Rule* FindRule(const char* fmt)
    {
        if (!fmt || std::strncmp(fmt, "[VR][", 5) != 0)
            return nullptr;
        const std::string_view text(fmt);
        for (const Rule& rule : kRules)
            if (text.substr(0, rule.prefix.size()) == rule.prefix)
                return &rule;
        return nullptr;
    }

    // Per call site (format string pointer) state; a small open table, a
    // site that does not fit is never limited (fail open).
    class RateLimiter
    {
    public:
        struct Decision
        {
            bool allow = true;
            uint32_t suppressedBefore = 0; // lines of this site left out since the last one written
        };

        Decision Check(const char* fmt, uint64_t nowMs, bool verbose)
        {
            Decision d;
            const Rule* rule = FindRule(fmt);
            if (!rule)
                return d;
            Site* site = Find(fmt);
            if (!site)
                return d;
            if (verbose || site->lastMs == 0 || nowMs - site->lastMs >= rule->intervalMs)
            {
                d.suppressedBefore = site->suppressed;
                site->suppressed = 0;
                site->lastMs = nowMs == 0 ? 1 : nowMs;
                return d;
            }
            ++site->suppressed;
            d.allow = false;
            return d;
        }

    private:
        struct Site
        {
            const char* fmt = nullptr;
            uint64_t lastMs = 0;
            uint32_t suppressed = 0;
        };

        Site* Find(const char* fmt)
        {
            for (Site& site : m_Sites)
            {
                if (site.fmt == fmt)
                    return &site;
                if (!site.fmt)
                {
                    site.fmt = fmt;
                    return &site;
                }
            }
            return nullptr;
        }

        std::array<Site, 64> m_Sites{};
    };
}

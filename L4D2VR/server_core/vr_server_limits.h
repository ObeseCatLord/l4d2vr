#pragma once

// Server configuration and anti-abuse limits for the dedicated plugin. std-only.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "vr_server_math.h"
#include "vr_server_protocol.h"

namespace l4d2vr_server
{
    struct ServerConfig
    {
        // Operator mask; a feature must be resolved AND enabled here.
        std::uint32_t featureMask = kFeatureAllMask;

        // Melee sweep (defaults from the previous dedicated plugin).
        bool meleeBlockWhileControlled = true;
        bool meleeBlockWhileIncapacitated = true;
        float meleeSwingCooldownSeconds = 0.35f;
        float meleeSwingBurstWindowSeconds = 2.0f;
        int meleeSwingBurstMax = 4;

        // Pose sanity.
        float unitsPerMeter = 43.2f;          // client VRScale default
        float maxHandReachMeters = 1.2f;      // hand may not be farther from the eye

        // Manual throw (client ManualThrow* defaults).
        float throwVelocityScale = 4.0f;
        float throwHorizontalScale = 1.0f;
        float throwVerticalScale = 1.0f;
        float throwArcLiftRatio = 0.0f;
        int throwVelocityWindowTicks = 5;
        float throwPeakVelocityBlend = 0.5f;
        float throwMaxSpeed = 1400.0f;        // hard anti-abuse clamp (units/s)
        float throwMinSpeed = 40.0f;          // below this keep the native throw
        float throwMaxAngularSpeed = 1440.0f; // deg/s
        float dropMaxSpeed = 450.0f;          // inventory/holster drop toss clamp
        // Hand throws (HandThrow, impulse 244): the client measures the hand
        // velocity at headset rate; projectile = hand * scale + body velocity,
        // clamped to throwMaxSpeed. 3.0 is Pavlov's grenade ProjectileSpeedMul.
        float throwHandVelocityScale = 3.0f;
        int handThrowMinIntervalMs = 250;

        // Eye override windows, in executed commands.
        int throwAimCommands = 48;
        int shoveAimCommands = 16;
        int pillAimCommands = 8;
        int useAimMode = 1;                   // 0 off, 1 grip pickup only, 2 any IN_USE

        // Intent rate limits.
        int holsterDropMinIntervalMs = 400;
        int shoveMinIntervalMs = 250;
        int pillGiveMinIntervalMs = 500;
        int intentBudgetPerSecond = 12;       // all 237..239 intents together

        bool verbose = false;

        float MaxHandReachUnits() const
        {
            return std::clamp(maxHandReachMeters, 0.1f, 5.0f) * std::clamp(unitsPerMeter, 1.0f, 200.0f);
        }
    };

    namespace config_detail
    {
        inline std::string Trim(const std::string& value)
        {
            std::size_t b = 0, e = value.size();
            while (b < e && std::isspace(static_cast<unsigned char>(value[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(value[e - 1]))) --e;
            return value.substr(b, e - b);
        }
        inline std::string Lower(std::string value)
        {
            for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return value;
        }
        inline bool ParseBool(const std::string& v, bool& out)
        {
            const std::string l = Lower(v);
            if (l == "1" || l == "true" || l == "yes" || l == "on") { out = true; return true; }
            if (l == "0" || l == "false" || l == "no" || l == "off") { out = false; return true; }
            return false;
        }
        inline bool ParseFloat(const std::string& v, float& out)
        {
            if (v.empty()) return false;
            char* end = nullptr;
            const float parsed = std::strtof(v.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(parsed)) return false;
            out = parsed;
            return true;
        }
        inline bool ParseInt(const std::string& v, int& out)
        {
            if (v.empty()) return false;
            char* end = nullptr;
            const long parsed = std::strtol(v.c_str(), &end, 10);
            if (!end || *end != '\0') return false;
            out = static_cast<int>(std::clamp<long>(parsed, -1000000L, 1000000L));
            return true;
        }
    }

    // Applies one "Key=Value" line; returns false for unknown keys/bad values.
    inline bool ApplyConfigLine(ServerConfig& c, const std::string& rawLine)
    {
        using namespace config_detail;
        std::string line = rawLine;
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        line = Trim(line);
        if (line.empty()) return true;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) return false;
        const std::string key = Lower(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));
        bool b = false; float f = 0.0f; int i = 0;
        if (key == "featuremask") { std::uint32_t m = 0; if (!ParseHexMask(value.c_str(), m)) return false; c.featureMask = m & kFeatureAllMask; return true; }
        if (key == "meleeswingblockwhilecontrolled" && ParseBool(value, b)) { c.meleeBlockWhileControlled = b; return true; }
        if (key == "meleeswingblockwhileincapacitated" && ParseBool(value, b)) { c.meleeBlockWhileIncapacitated = b; return true; }
        if (key == "meleeswingcooldownseconds" && ParseFloat(value, f)) { c.meleeSwingCooldownSeconds = std::clamp(f, 0.0f, 60.0f); return true; }
        if (key == "meleeswingburstwindowseconds" && ParseFloat(value, f)) { c.meleeSwingBurstWindowSeconds = std::clamp(f, 0.0f, 60.0f); return true; }
        if (key == "meleeswingburstmax" && ParseInt(value, i)) { c.meleeSwingBurstMax = std::clamp(i, 0, 16); return true; }
        if (key == "unitspermeter" && ParseFloat(value, f)) { c.unitsPerMeter = std::clamp(f, 1.0f, 200.0f); return true; }
        if (key == "maxhandreachmeters" && ParseFloat(value, f)) { c.maxHandReachMeters = std::clamp(f, 0.1f, 1.2f); return true; }
        if (key == "throwvelocityscale" && ParseFloat(value, f)) { c.throwVelocityScale = std::clamp(f, 0.0f, 10.0f); return true; }
        if (key == "throwhorizontalvelocityscale" && ParseFloat(value, f)) { c.throwHorizontalScale = std::clamp(f, 0.0f, 10.0f); return true; }
        if (key == "throwverticalvelocityscale" && ParseFloat(value, f)) { c.throwVerticalScale = std::clamp(f, 0.0f, 10.0f); return true; }
        if (key == "throwarcliftratio" && ParseFloat(value, f)) { c.throwArcLiftRatio = std::clamp(f, 0.0f, 2.0f); return true; }
        if (key == "throwvelocitywindowticks" && ParseInt(value, i)) { c.throwVelocityWindowTicks = std::clamp(i, 1, 15); return true; }
        if (key == "throwpeakvelocityblend" && ParseFloat(value, f)) { c.throwPeakVelocityBlend = std::clamp(f, 0.0f, 1.0f); return true; }
        if (key == "throwmaxspeed" && ParseFloat(value, f)) { c.throwMaxSpeed = std::clamp(f, 0.0f, 2500.0f); return true; }
        if (key == "throwminspeed" && ParseFloat(value, f)) { c.throwMinSpeed = std::clamp(f, 0.0f, 500.0f); return true; }
        if (key == "dropmaxspeed" && ParseFloat(value, f)) { c.dropMaxSpeed = std::clamp(f, 0.0f, 1000.0f); return true; }
        if (key == "throwhandvelocityscale" && ParseFloat(value, f)) { c.throwHandVelocityScale = std::clamp(f, 0.0f, 10.0f); return true; }
        if (key == "handthrowminintervalms" && ParseInt(value, i)) { c.handThrowMinIntervalMs = std::clamp(i, 50, 10000); return true; }
        if (key == "throwaimcommands" && ParseInt(value, i)) { c.throwAimCommands = std::clamp(i, 0, 96); return true; }
        if (key == "shoveaimcommands" && ParseInt(value, i)) { c.shoveAimCommands = std::clamp(i, 0, 64); return true; }
        if (key == "pillaimcommands" && ParseInt(value, i)) { c.pillAimCommands = std::clamp(i, 0, 64); return true; }
        if (key == "useaimmode" && ParseInt(value, i)) { c.useAimMode = std::clamp(i, 0, 2); return true; }
        if (key == "holsterdropminintervalms" && ParseInt(value, i)) { c.holsterDropMinIntervalMs = std::clamp(i, 100, 10000); return true; }
        if (key == "shoveminintervalms" && ParseInt(value, i)) { c.shoveMinIntervalMs = std::clamp(i, 50, 10000); return true; }
        if (key == "pillgiveminintervalms" && ParseInt(value, i)) { c.pillGiveMinIntervalMs = std::clamp(i, 100, 10000); return true; }
        if (key == "intentbudgetpersecond" && ParseInt(value, i)) { c.intentBudgetPerSecond = std::clamp(i, 1, 60); return true; }
        if (key == "verbose" && ParseBool(value, b)) { c.verbose = b; return true; }
        return false;
    }

    // ---- Pose sanity ----------------------------------------------------------

    // Keeps the hand within `maxUnits` of the eye. Returns true if clamped.
    inline bool ClampHandReach(const Vec3& eye, Vec3& hand, float maxUnits)
    {
        if (!eye.Finite() || !hand.Finite())
        {
            hand = eye;
            return true;
        }
        Vec3 delta = hand - eye;
        const float distance = delta.Length();
        if (!std::isfinite(distance) || distance <= maxUnits)
            return false;
        delta *= maxUnits / distance;
        hand = eye + delta;
        return true;
    }

    inline bool SaneControllerAngles(const Angles& a)
    {
        return a.Finite() && std::fabs(a.x) <= 360.0f && std::fabs(a.y) <= 3600.0f && std::fabs(a.z) <= 360.0f;
    }

    // ---- Rate limits ----------------------------------------------------------

    class MinIntervalLimiter
    {
    public:
        bool Allow(std::uint64_t nowMs, int minIntervalMs)
        {
            if (m_Have && nowMs >= m_Last && nowMs - m_Last < static_cast<std::uint64_t>(std::max(0, minIntervalMs)))
                return false;
            m_Have = true;
            m_Last = nowMs;
            return true;
        }
        void Reset() { m_Have = false; m_Last = 0u; }
    private:
        bool m_Have = false;
        std::uint64_t m_Last = 0u;
    };

    // Fixed one-second window budget shared by all intents of a player.
    class IntentBudget
    {
    public:
        bool Consume(std::uint64_t nowMs, int perSecond)
        {
            if (nowMs < m_WindowStart || nowMs - m_WindowStart >= 1000u)
            {
                m_WindowStart = nowMs;
                m_Used = 0;
            }
            if (m_Used >= std::max(1, perSecond))
                return false;
            ++m_Used;
            return true;
        }
        void Reset() { m_WindowStart = 0u; m_Used = 0; }
    private:
        std::uint64_t m_WindowStart = 0u;
        int m_Used = 0;
    };

    // Cooldown and burst gate for starting a physical melee swing; a port of the
    // previous dedicated plugin's CanStartMeleeSwing.
    class MeleeSwingGate
    {
    public:
        enum class Result { Allowed, Cooldown, Burst };

        Result TryStart(std::uint64_t nowMs, const ServerConfig& c)
        {
            const auto cooldownMs = static_cast<std::uint64_t>(std::max(0.0f, c.meleeSwingCooldownSeconds) * 1000.0f);
            if (m_HaveLast && cooldownMs > 0u && nowMs >= m_Last && nowMs - m_Last < cooldownMs)
                return Result::Cooldown;
            const auto windowMs = static_cast<std::uint64_t>(std::max(0.0f, c.meleeSwingBurstWindowSeconds) * 1000.0f);
            if (windowMs > 0u && c.meleeSwingBurstMax > 0)
            {
                int recent = 0;
                for (std::uint64_t t : m_History)
                    if (t != 0u && nowMs >= t && nowMs - t <= windowMs) ++recent;
                if (recent >= c.meleeSwingBurstMax)
                    return Result::Burst;
            }
            m_HaveLast = true;
            m_Last = nowMs;
            std::size_t slot = 0;
            for (std::size_t i = 0; i < m_History.size(); ++i)
            {
                if (m_History[i] == 0u) { slot = i; break; }
                if (m_History[i] < m_History[slot]) slot = i;
            }
            m_History[slot] = nowMs == 0u ? 1u : nowMs;
            return Result::Allowed;
        }
        void Reset() { m_HaveLast = false; m_Last = 0u; m_History = {}; }
    private:
        bool m_HaveLast = false;
        std::uint64_t m_Last = 0u;
        std::array<std::uint64_t, 16> m_History{};
    };
}

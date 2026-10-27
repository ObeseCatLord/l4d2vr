#pragma once

// Physical melee sweep planner: a std-only port of the listen host's
// dProcessUsercmds angular sweep (hooks_combat_network.inl) plus the previous
// dedicated plugin's cooldown/burst gate. The shell feeds one executed VR
// command at a time and runs the returned traces through the game's own
// CTerrorMeleeWeapon::TestMeleeSwingCollision.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "../vr_physical_controls.h"
#include "vr_server_limits.h"
#include "vr_server_math.h"

namespace l4d2vr_server
{
    struct MeleeTrace
    {
        Vec3 origin{};
        Vec3 direction{};
        Angles angles{};
    };

    struct MeleeSweepPlan
    {
        static constexpr int kMaxTraces = 12;
        int count = 0;
        std::array<MeleeTrace, kMaxTraces> traces{};
    };

    // Pure geometry: traces between two controller poses.
    inline MeleeSweepPlan PlanMeleeSweep(bool hasPrevious, const Vec3& prevPos, const Angles& prevAngles,
        const Vec3& curPos, const Angles& curAngles, float unitsPerMeter)
    {
        MeleeSweepPlan plan{};
        Vec3 initialForward, initialRight, initialUp;
        AngleVectors(prevAngles, &initialForward, &initialRight, &initialUp);
        Vec3 initialDir = RotateAroundAxis(initialForward, initialRight, 50.0f);
        Normalize(initialDir);

        Vec3 finalForward, finalRight, finalUp;
        AngleVectors(curAngles, &finalForward, &finalRight, &finalUp);
        Vec3 finalDir = RotateAroundAxis(finalForward, finalRight, 50.0f);
        Normalize(finalDir);

        const float swingDot = std::clamp(Dot(initialDir, finalDir), -1.0f, 1.0f);
        Vec3 pivot = Cross(initialDir, finalDir);
        bool canTraceSwing = true;
        if (Normalize(pivot) <= 0.0001f)
        {
            if (swingDot > -0.999f)
                canTraceSwing = false;
            else
            {
                pivot = initialUp;
                canTraceSwing = Normalize(pivot) > 0.0001f;
            }
        }
        const float swingAngle = std::acos(swingDot) * 180.0f / 3.14159265f;
        if (!std::isfinite(swingAngle) || swingAngle <= 0.01f)
            canTraceSwing = false;

        const Vec3 translation = hasPrevious ? curPos - prevPos : Vec3{};
        const float distance = translation.Length();
        if (!hasPrevious)
            canTraceSwing = false;
        const int samples = std::min(MeleeSweepPlan::kMaxTraces,
            l4d2vr_physical::MeleeSweepSamples(distance, canTraceSwing ? swingAngle : 0.0f, unitsPerMeter));
        if (samples <= 0)
            return plan;

        Vec3 direction = initialDir;
        const float step = canTraceSwing ? swingAngle / static_cast<float>(samples) : 0.0f;
        for (int i = 0; i < samples; ++i)
        {
            direction = canTraceSwing ? RotateAroundAxis(direction, pivot, step) : finalDir;
            MeleeTrace& t = plan.traces[static_cast<std::size_t>(plan.count++)];
            t.origin = hasPrevious ? prevPos + translation * (static_cast<float>(i + 1) / static_cast<float>(samples)) : curPos;
            t.direction = direction;
            t.angles = VectorAngles(direction);
        }
        return plan;
    }

    // Per-player sweep continuity, swing start gate and previous pose.
    class MeleeSweepState
    {
    public:
        struct Step
        {
            bool sweep = false;          // run plan.traces
            bool newSwing = false;       // reset the weapon's per-swing hit list first
            bool rejected = false;       // gate refused the new swing
            MeleeSwingGate::Result gate = MeleeSwingGate::Result::Allowed;
            MeleeSweepPlan plan{};
        };

        // One executed command. `eligible`: live survivor holding a melee weapon
        // and a finite VR pose; `blocked`: incapacitated/pinned per config.
        Step Process(std::uintptr_t owner, std::uintptr_t weapon, int command, bool eligible, bool swinging,
            bool blocked, const Vec3& pos, const Angles& angles, std::uint64_t nowMs, const ServerConfig& c)
        {
            Step step{};
            eligible = eligible && pos.Finite() && angles.Finite();
            const auto frame = m_History.Accept(owner, weapon, command, eligible);
            if (frame == l4d2vr_physical::MeleeSweepHistory::Ignore)
                return step;
            if (frame == l4d2vr_physical::MeleeSweepHistory::Rebase || !eligible)
            {
                m_HasPrevious = false;
                m_NewSwing = true;
            }
            if (eligible && frame == l4d2vr_physical::MeleeSweepHistory::Sweep && swinging)
            {
                if (blocked)
                {
                    m_SwingAllowed = false;
                }
                else
                {
                    if (m_NewSwing)
                    {
                        m_NewSwing = false;
                        step.gate = m_Gate.TryStart(nowMs, c);
                        m_SwingAllowed = step.gate == MeleeSwingGate::Result::Allowed;
                        step.newSwing = m_SwingAllowed;
                        step.rejected = !m_SwingAllowed;
                    }
                    if (m_SwingAllowed)
                    {
                        step.plan = PlanMeleeSweep(m_HasPrevious, m_PrevPos, m_PrevAngles, pos, angles, c.unitsPerMeter);
                        step.sweep = step.plan.count > 0;
                    }
                }
            }
            else
            {
                m_NewSwing = true;
                m_SwingAllowed = false;
            }
            m_PrevPos = pos;
            m_PrevAngles = angles;
            m_HasPrevious = eligible;
            return step;
        }

        void Reset() { *this = MeleeSweepState{}; }

    private:
        l4d2vr_physical::MeleeSweepHistory m_History{};
        MeleeSwingGate m_Gate{};
        bool m_HasPrevious = false;
        bool m_NewSwing = true;
        bool m_SwingAllowed = false;
        Vec3 m_PrevPos{};
        Angles m_PrevAngles{};
    };
}

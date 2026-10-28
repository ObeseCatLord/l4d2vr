#pragma once
#include <cmath>
#include <cstdint>

// Ambidextrous items (AmbidextrousItemHands): the hand that takes an item holds
// and uses it. The gameplay code calls the item hand "right" and the free hand
// "left"; LeftHanded maps those names to the physical controllers. "Swapped"
// means the configured off hand holds the active item, so the gameplay right
// hand is the other physical controller until the item changes hands. Poses,
// the trigger and grip roles, haptics, the viewmodel and the aim ray all follow
// that one mapping. std-only so the rules are unit-tested offline.
namespace l4d2vr_item_hand
{
    enum class Reason : unsigned
    {
        None = 0,
        HolsterDraw,      // the free hand drew from a holster
        HandOff,          // the free hand squeezed the item in the other hand
        ReleaseTransfer,  // the item hand let go while the free hand held the foregrip
        Reset,            // death, spectating, VR off, mouse mode, option off
    };

    inline const char* ReasonName(Reason reason)
    {
        switch (reason)
        {
        case Reason::HolsterDraw: return "holster draw";
        case Reason::HandOff: return "hand-off";
        case Reason::ReleaseTransfer: return "release transfer";
        case Reason::Reset: return "reset";
        default: return "none";
        }
    }

    // Physical hand that holds the active item.
    inline bool ItemHandIsPhysicalLeft(bool leftHanded, bool swapped) { return leftHanded != swapped; }
    // The swap that puts the item into the given physical hand.
    inline bool SwappedForPhysicalHand(bool leftHanded, bool physicalLeft) { return physicalLeft != leftHanded; }

    // A hand change requested during input processing is applied at the start
    // of the next tracking update, so every frame's poses and inputs agree.
    class Switch
    {
    public:
        void Request(bool swapped, Reason reason)
        {
            m_Pending = true;
            m_Target = swapped;
            m_Reason = reason;
        }
        void Cancel() { m_Pending = false; m_Reason = Reason::None; }
        bool Pending() const { return m_Pending; }
        bool PendingTarget() const { return m_Target; }
        // Returns true when the mapping changes.
        bool Commit(bool& swapped, Reason& reason)
        {
            reason = Reason::None;
            if (!m_Pending)
                return false;
            m_Pending = false;
            reason = m_Reason;
            m_Reason = Reason::None;
            if (swapped == m_Target)
                return false;
            swapped = m_Target;
            return true;
        }
    private:
        bool m_Pending = false;
        bool m_Target = false;
        Reason m_Reason = Reason::None;
    };

    // The free hand squeezes its grip on the item in the other hand: the item
    // moves to it. Anything that claims the squeeze first (a holster, the
    // magazine pouch, a magazine, bolt or slide, the foregrip) wins.
    struct HandOffInput
    {
        bool enabled = false;
        bool ready = false;          // in game, alive, not pinned, first-person control
        bool pressEdge = false;      // the free hand's grip was just squeezed
        bool claimed = false;        // the squeeze was taken by another interaction
        bool itemInHand = false;     // there is an active item to hand over
        float handDistanceMeters = -1.0f; // between the two controllers
        float radiusMeters = 0.14f;
    };
    inline bool HandOff(const HandOffInput& in)
    {
        return in.enabled && in.ready && in.pressEdge && !in.claimed && in.itemInHand &&
            std::isfinite(in.handDistanceMeters) && in.handDistanceMeters >= 0.0f &&
            in.handDistanceMeters <= in.radiusMeters;
    }

    // The item hand lets go while the free hand holds the item's foregrip: the
    // item stays in the free hand instead of dropping (letting go over a
    // holster still puts it away or swaps).
    struct ReleaseInput
    {
        bool enabled = false;
        bool ready = false;
        bool released = false;       // the item hand's (virtual) grip went from held to free
        bool twoHandGrip = false;    // the free hand holds the foregrip
        bool overHolster = false;    // the item hand is in its own or another holster
        bool supportHolds = false;   // the free hand keeps hold (toggle foregrip or still squeezing)
    };
    inline bool ReleaseTransfer(const ReleaseInput& in)
    {
        return in.enabled && in.ready && in.released && in.twoHandGrip && !in.overHolster && in.supportHolds;
    }

    // Hand-role actions trade places while swapped: the item hand's trigger
    // fires and its grip is the weapon grip. Everything else stays where the
    // bindings put it (sticks, face buttons).
    template <class Handle>
    inline Handle RemapRole(Handle action, bool swapped, Handle primary, Handle secondary,
        Handle weaponGrip, Handle offHandGrip, Handle invalid)
    {
        if (!swapped || action == invalid)
            return action;
        if (action == primary && secondary != invalid) return secondary;
        if (action == secondary && primary != invalid) return primary;
        if (action == weaponGrip && offHandGrip != invalid) return offHandGrip;
        if (action == offHandGrip && weaponGrip != invalid) return weaponGrip;
        return action;
    }
}

#pragma once
#include <cstdint>

// Toggle grips (Pavlov style): one firm squeeze takes an item and the hand
// keeps it after letting go; the next squeeze lets it go (holsters it inside
// its own holster, drops it elsewhere). The toggle turns the physical grip
// into a virtual "held" grip that the existing hold-to-grip logic consumes.
namespace l4d2vr_grip_toggle
{
    struct Input
    {
        bool enabled = true;          // toggle mode on; off passes the physical grip through
        bool active = false;          // grip action is bound and readable
        bool rawDown = false;         // physical grip state
        bool ready = false;           // gameplay can use the hand (in game, not pinned or incapped)
        std::uint64_t nowMs = 0u;
        std::uint32_t takeSerial = 0u;    // bumps whenever the hand takes an item (holster draw, adopted pickup)
        std::uint32_t pickupSerial = 0u;  // bumps when a contact pickup was requested from the server
        std::uint32_t heldItemTag = 0u;   // identity of the item in the hand (0 = none)
        bool inOwnHolster = false;        // the hand is inside the current item's own holster
        bool inOtherHolster = false;      // the hand is over another occupied holster
    };

    struct Params
    {
        std::uint32_t emptyGrabMs = 300u;   // a squeeze that takes nothing becomes a tap
        std::uint32_t pickupWaitMs = 1200u; // how long a requested pickup may take to arrive
        std::uint32_t settleMs = 1500u;     // item switch time before the held item is pinned
    };

    class Toggle
    {
    public:
        // Returns the virtual grip state.
        bool Update(const Input& in, const Params& params = {})
        {
            const bool pressed = in.rawDown && !m_PrevRaw;
            m_PrevRaw = in.rawDown;
            if (!in.enabled)
            {
                Clear();
                return in.active && in.rawDown;
            }
            if (!in.active || !in.ready)
            {
                Clear();
                return false;
            }

            if (m_SwapPress)
            {
                // Second half of a swap: grab from the other holster.
                m_SwapPress = false;
                StartGrab(in, params);
                return true;
            }
            if (pressed)
            {
                if (m_Held)
                {
                    // Squeezing over another holster swaps items: let go for one
                    // frame (the item is not dropped over a holster), then grab.
                    const bool swap = in.inOtherHolster;
                    Clear();
                    m_SwapPress = swap;
                    return false;
                }
                StartGrab(in, params);
            }
            if (!m_Held)
                return false;
            return Continue(in, params);
        }

        bool Held() const { return m_Held; }
        // The item changed hands (ambidextrous hand-off): the new hand's grip is
        // already in rawDown, so it is not a fresh press, and the item stays
        // held (or not) as it was.
        void Adopt(bool rawDown, bool held)
        {
            m_PrevRaw = rawDown;
            m_SwapPress = false;
            m_Pending = false;
            m_PinnedTag = 0u;
            m_Held = held;
        }
        void Reset()
        {
            Clear();
            m_PrevRaw = false;
        }

    private:
        void StartGrab(const Input& in, const Params& params)
        {
                m_Held = true;
                m_Pending = true;
                m_PressMs = in.nowMs;
                // Squeezing inside the current item's own holster puts it away
                // at once: a one-frame tap.
                m_Deadline = in.inOwnHolster ? 0u : params.emptyGrabMs;
                m_TakeAtPress = in.takeSerial;
                m_PickupAtPress = in.pickupSerial;
                m_PinnedTag = 0u;
        }

        bool Continue(const Input& in, const Params& params)
        {
            if (m_Pending)
            {
                if (in.takeSerial != m_TakeAtPress)
                {
                    m_Pending = false;
                    m_TakeAtPress = in.takeSerial;
                    m_TakenMs = in.nowMs;
                }
                else
                {
                    if (in.pickupSerial != m_PickupAtPress)
                    {
                        m_PickupAtPress = in.pickupSerial;
                        m_Deadline = params.pickupWaitMs;
                        m_PressMs = in.nowMs;
                    }
                    if (in.nowMs - m_PressMs > m_Deadline)
                    {
                        // Nothing was taken: release, so the squeeze acts as a tap
                        // (which also holsters the current item inside its holster).
                        Clear();
                        return false;
                    }
                }
                return true;
            }

            // Once the drawn item is in the hand, pin it; anything else that
            // replaces it (death, a throw, a scripted switch) ends the hold.
            if (m_PinnedTag == 0u)
            {
                if (in.nowMs - m_TakenMs >= params.settleMs && in.heldItemTag != 0u)
                    m_PinnedTag = in.heldItemTag;
            }
            else if (in.heldItemTag != m_PinnedTag)
            {
                Clear();
                return false;
            }
            // A new take while held (e.g. drawing straight from another holster)
            // re-arms the settle window for the new item.
            if (in.takeSerial != m_TakeAtPress)
            {
                m_TakeAtPress = in.takeSerial;
                m_TakenMs = in.nowMs;
                m_PinnedTag = 0u;
            }
            return true;
        }

        void Clear()
        {
            m_Held = false;
            m_Pending = false;
            m_SwapPress = false;
            m_PinnedTag = 0u;
        }

        bool m_PrevRaw = false;
        bool m_Held = false;
        bool m_Pending = false;
        bool m_SwapPress = false;
        std::uint64_t m_PressMs = 0u;
        std::uint64_t m_TakenMs = 0u;
        std::uint32_t m_Deadline = 0u;
        std::uint32_t m_TakeAtPress = 0u;
        std::uint32_t m_PickupAtPress = 0u;
        std::uint32_t m_PinnedTag = 0u;
    };
}

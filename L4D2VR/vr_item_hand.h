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
//
// 2.0.6 (headset session 7): a hand gets an item ONLY by taking it from the
// body (a holster, the chest sling, its stowed place in the empty-hands state)
// or the world. A held item never changes hands: the other hand's squeeze on
// it is the foregrip / support grip (or nothing), never ownership. The 2.0.3
// hand-off (squeeze the item with the free hand) and the release transfer
// (let go while the free hand holds the foregrip) are gone. To use a gun in
// the other hand, put it away (squeeze at its holster or the sling) and take
// it with that hand.
namespace l4d2vr_item_hand
{
    enum class Reason : unsigned
    {
        None = 0,
        HolsterDraw,      // the free hand drew from a holster
        SlingTake,        // the free hand took the stowed item (chest sling, its holster) back
        SelfTest,         // -l4d2vrselftest itemhand= / heldhand= (puts the item where a photo needs it)
        Reset,            // death, spectating, VR off, mouse mode, option off
    };

    inline const char* ReasonName(Reason reason)
    {
        switch (reason)
        {
        case Reason::HolsterDraw: return "holster draw";
        case Reason::SlingTake: return "sling take";
        case Reason::SelfTest: return "self-test";
        case Reason::Reset: return "reset";
        default: return "none";
        }
    }

    // Only these move the item to another hand (RequestItemHand).
    inline bool ChangesHands(Reason reason)
    {
        return reason == Reason::HolsterDraw || reason == Reason::SlingTake ||
            reason == Reason::SelfTest || reason == Reason::Reset;
    }

    // The new item hand already squeezes what it took (the slung or stowed
    // gun): its grip adopts the item instead of reading the squeeze as a
    // fresh press. A holster draw starts with a fresh press next frame.
    inline bool AdoptsGrip(Reason reason) { return reason == Reason::SlingTake || reason == Reason::SelfTest; }

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

    // The whole take / hold / put away / drop rule for the two hands, as one
    // state machine (Pavlov VR's grip modes, decoded from its controller code:
    // EGripMode Click = toggle, Hold = hold; numbers and mechanics only).
    //
    // Toggle grip (GripToggle, the default; Pavlov "Click"): everything happens
    // on the PRESS of the grip, never on its release.
    //   - An empty hand's squeeze at a holster, the sling or a stowed item takes
    //     it: that hand owns it (the trigger hand) and keeps it after letting go.
    //   - The owner's next squeeze lets go: at that item's own holster (or the
    //     sling for a long gun) it is put away (the hand is empty), anywhere else
    //     it is dropped at the hand with the hand's motion.
    //   - The other hand's squeeze on the held item is the support grip: on the
    //     foregrip it starts the two-handed grip, a second squeeze ends it
    //     (VrHandsTwoHandedGripHeldMode=false, Pavlov bTwoHandToggle); anywhere
    //     else on the item (its handle) nothing. It never takes the item.
    //     (Pavlov also lets go into the foregrip hand when the owner squeezes
    //     while two-handed; L4D2VR does not: the item is put away or dropped.)
    // Hold grip (GripToggle=false; Pavlov "Hold"): the owner keeps the item only
    // while squeezing; letting go puts it away near its holster and drops it
    // elsewhere. The support grip holds while squeezed (or toggles).
    enum class Action { None, Take, PutAway, Drop, SupportGrip, SupportRelease };

    inline const char* ActionName(Action action)
    {
        switch (action)
        {
        case Action::Take: return "take";
        case Action::PutAway: return "put away";
        case Action::Drop: return "drop";
        case Action::SupportGrip: return "support grip";
        case Action::SupportRelease: return "support release";
        default: return "none";
        }
    }

    // What is under the squeezing hand.
    struct Under
    {
        bool takeable = false;      // a holster, the sling or the stowed place with an item to take
        bool ownHolster = false;    // the held item's own holster (the sling for a long gun) within reach
        bool foregrip = false;      // the foregrip of the item in the other hand
        bool heldItem = false;      // anywhere else on the item in the other hand (its handle)
    };

    class Ownership
    {
    public:
        explicit Ownership(bool toggleGrip = true, bool foregripToggle = true)
            : m_Toggle(toggleGrip), m_ForegripToggle(foregripToggle) {}

        // hand: 0 left, 1 right (physical). Returns what the edge did.
        Action Press(int hand, const Under& under)
        {
            if (hand < 0 || hand > 1)
                return Action::None;
            if (m_Owner == hand)
            {
                // Hold mode: the grip is already down while held; a press can
                // only come after a release, which let the item go.
                if (!m_Toggle)
                    return Action::None;
                return LetGo(under);
            }
            if (m_Owner >= 0)
            {
                // The other hand holds an item: the squeeze is support only.
                if (m_Support && m_ForegripToggle)
                {
                    m_Support = false;
                    return Action::SupportRelease;
                }
                if (under.foregrip)
                {
                    m_Support = true;
                    return Action::SupportGrip;
                }
                if (under.takeable)
                    return TakeBy(hand);   // a different item from the body: the other one is put away
                return Action::None;       // the held item's handle: never ownership
            }
            if (under.takeable)
                return TakeBy(hand);
            return Action::None;
        }

        Action Release(int hand, const Under& under)
        {
            if (hand < 0 || hand > 1)
                return Action::None;
            if (m_Owner == hand)
                return m_Toggle ? Action::None : LetGo(under);
            if (m_Owner >= 0 && m_Support && !m_ForegripToggle)
            {
                m_Support = false;
                return Action::SupportRelease;
            }
            return Action::None;
        }

        int Owner() const { return m_Owner; }
        bool Support() const { return m_Support; }
        bool Toggle() const { return m_Toggle; }

    private:
        Action TakeBy(int hand)
        {
            m_Owner = hand;
            m_Support = false;
            return Action::Take;
        }
        Action LetGo(const Under& under)
        {
            m_Owner = -1;
            m_Support = false;   // the support hand lets go with the item
            return under.ownHolster ? Action::PutAway : Action::Drop;
        }

        bool m_Toggle = true;
        bool m_ForegripToggle = true;
        int m_Owner = -1;
        bool m_Support = false;
    };

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

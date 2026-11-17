#pragma once

// 2.0.12 (review s12, issues 12/13, E5, G2): no item is ever dropped by a grip
// squeeze. The owning hand's second squeeze (toggle grip) or letting go (hold
// grip) only puts the item away where a holster catches it (its own holster,
// the chest sling's 0.6 m catch for long guns); anywhere else the item simply
// stays in the hand. Grenade hand throws (pin pulled, then let go) and carried
// props (trigger release) are throws, not drops, and are unchanged.
//
// Sidearms (pistol, magnum, melee weapons, the chainsaw: slot 2) can never be
// dropped, not even by the deliberate drop that will come later: the server
// refuses them too (server_core weapon::IsNeverDroppable, the listen host).
//
// The hook for the later dedicated drop button/chord: Mode::Chord plus
// ReleaseInput::deliberateDrop (VR::ConsumeDeliberateDropRequest). Nothing
// binds it yet, and kMode stays Off until it exists.
//
// std-only so the rule is unit-tested offline (tests/vr_drop_policy_tests.cpp).
namespace l4d2vr_drop
{
    enum class Mode { Off = 0, Chord = 1 };

    // The shipped mode. A future config key / binding replaces this.
    constexpr Mode kMode = Mode::Off;

    // Slot 2: pistol 1, melee 19, chainsaw 20, magnum 32 (C_WeaponCSBase::WeaponID).
    inline bool IsSidearm(int weaponId)
    {
        return weaponId == 1 || weaponId == 19 || weaponId == 20 || weaponId == 32;
    }

    inline bool IsNeverDroppable(int weaponId) { return IsSidearm(weaponId); }

    enum class Outcome { None, PutAway, Throw, Keep, Drop };

    inline const char* OutcomeName(Outcome outcome)
    {
        switch (outcome)
        {
        case Outcome::PutAway: return "put away";
        case Outcome::Throw: return "thrown";
        case Outcome::Keep: return "kept in the hand";
        case Outcome::Drop: return "dropped";
        default: return "none";
        }
    }

    struct ReleaseInput
    {
        Mode mode = kMode;
        bool released = false;       // the owning grip let the item go (toggle: second squeeze)
        int weaponId = 0;
        bool caught = false;         // a holster / the sling took it (put away)
        bool thrown = false;         // a grenade hand throw used the release
        bool carriedProp = false;    // gascans etc.: released by the trigger, not here
        bool deliberateDrop = false; // the future drop button/chord was used for this release
    };

    inline Outcome Resolve(const ReleaseInput& in)
    {
        if (!in.released || in.carriedProp)
            return Outcome::None;
        if (in.caught)
            return Outcome::PutAway;
        if (in.thrown)
            return Outcome::Throw;
        if (in.mode == Mode::Chord && in.deliberateDrop && !IsNeverDroppable(in.weaponId))
            return Outcome::Drop;
        return Outcome::Keep;
    }
}

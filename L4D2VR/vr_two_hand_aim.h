#pragma once

// Two-handed aim (HL2VR / Pavlov style): on a long gun the support hand on the
// foregrip steers the aim (the gun points from the rear hand to the front
// hand). On a short gun the support hand only steadies it: the aim comes from
// the trigger hand alone (VrHandsTwoHandedAimShortGunsRearHandOnly). Recoil is
// separate: holding the foregrip of any gun, short ones included, still counts
// as two-handed for recoil (Pavlov's SMGs and pistols need two hands to
// control). std-only so the list and the rule are unit-tested offline.
namespace l4d2vr_two_hand
{
    // L4D2 CSWeaponID values.
    constexpr int kPistol = 1;
    constexpr int kUzi = 2;      // the Uzi SMG
    constexpr int kMac10 = 7;    // the silenced SMG
    constexpr int kMagnum = 32;
    constexpr int kMp5 = 33;

    // Pistols, the magnum and the SMGs. Dual pistols never take a foregrip.
    inline bool IsShortGun(int weaponId)
    {
        switch (weaponId)
        {
        case kPistol:
        case kUzi:
        case kMac10:
        case kMagnum:
        case kMp5:
            return true;
        default:
            return false;
        }
    }

    // Whether the support hand on the foregrip turns the aim.
    inline bool SupportHandSteersAim(int weaponId, bool shortGunsRearHandOnly)
    {
        return !(shortGunsRearHandOnly && IsShortGun(weaponId));
    }

    // Whether the support hand on the foregrip counts as two-handed for recoil.
    inline bool SupportHandSteadiesRecoil(bool foregripHeld) { return foregripHeld; }
}

#pragma once

// Weapons whose reserve the game never consumes (2.0.12, review s12 issue 17).
// The pistol and the magnum both use an infinite ammo type in L4D2; their
// reserve slot holds the game's sentinel 999, which must be shown as an
// infinity glyph, never as a number. Decide by weapon id, not by the value:
// a custom server can give a rifle 999 real rounds. std-only so the wrist
// panels, the pouch logic and the tests share one definition.
namespace l4d2vr_weapon
{
    // Native L4D2 WeaponID values (sdk.h C_WeaponCSBase::WeaponID).
    inline constexpr int kPistolId = 1;
    inline constexpr int kMagnumId = 32;

    inline constexpr bool HasInfiniteReserve(int weaponId)
    {
        return weaponId == kPistolId || weaponId == kMagnumId;
    }
}

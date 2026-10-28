#pragma once
#include <algorithm>

// Per-weapon physical reload tuning taken from Pavlov VR (read from its
// shipped data; numbers only). Pavlov describes a charging handle, slide or
// pump by its hand travel (LinearRange, cm), the fraction of it that cocks the
// gun (CokingThreashold, default 0.38) and the fraction below which the round
// is chambered on the way forward (ReChamberThreashold, default 0.11). A
// magazine seats once pushed SlideGoalThreshold cm into its well.
//
// Our bolt stage has the same shape: the bolt cocks once pulled `pull` metres,
// and the round chambers when it is let go after that or pushed back to within
// `ret` metres of rest. Hand travel and slide travel are separate, as in
// Pavlov: the slide model only moves `visual` metres however far the hand goes.
namespace l4d2vr_reload_tuning
{
    struct Tuning
    {
        float pullMeters = 0.0f;        // hand pull that cocks
        float returnMeters = 0.0f;      // chamber once back within this of rest
        float insertDepthMeters = 0.0f; // magazine depth that seats it
        float visualTravelMeters = 0.0f;// how far the slide model moves
    };

    struct PavlovHandle
    {
        float rangeCm;
        float coking;
        float rechamber;
        float slideGoalCm;
        float visualCm;
    };

    // Hand travel that is too short is lost in tracking noise; longer than a
    // forearm's comfortable pull is tiring on a viewmodel-sized gun.
    constexpr float kMinPullMeters = 0.03f;
    constexpr float kMaxPullMeters = 0.10f;
    // Our magazine depth check needs the magazine box to overlap the well this
    // much; Pavlov's deeper goals (Uzi 13 cm, 1911 9 cm) are capped here.
    constexpr float kMinInsertMeters = 0.01f;
    constexpr float kMaxInsertMeters = 0.04f;

    inline Tuning FromPavlov(const PavlovHandle& h)
    {
        Tuning t{};
        const float rawPull = h.rangeCm * h.coking * 0.01f;
        const float pull = std::clamp(rawPull, kMinPullMeters, kMaxPullMeters);
        // Keep the chamber point at the same fraction of the cock point when
        // the pull is clamped.
        const float ratio = h.coking > 0.001f ? h.rechamber / h.coking : 0.0f;
        t.pullMeters = pull;
        t.returnMeters = std::clamp(pull * ratio, 0.0f, pull - 0.005f);
        t.insertDepthMeters = std::clamp(h.slideGoalCm * 0.01f, kMinInsertMeters, kMaxInsertMeters);
        t.visualTravelMeters = std::min(pull, h.visualCm * 0.01f);
        return t;
    }

    // weaponId: L4D2 C_WeaponCSBase::WeaponID. Returns false for weapons that
    // keep the global settings (launcher, M60, non-guns).
    inline bool ForWeapon(int weaponId, Tuning& out)
    {
        // { range cm, coking, rechamber, slide goal cm, visual slide travel cm }
        // Pavlov source gun in the comment.
        PavlovHandle h{};
        switch (weaponId)
        {
        case 1:  h = { 20.0f, 0.38f, 0.20f,  3.0f, 3.0f }; break; // pistol        <- M9
        case 32: h = { 25.0f, 0.38f, 0.175f, 2.0f, 3.5f }; break; // magnum        <- Desert Eagle
        case 2:  h = { 20.0f, 0.38f, 0.11f,  2.0f, 4.0f }; break; // uzi           <- MP5 (closed-bolt SMG)
        case 7:  h = {  5.0f, 1.00f, 0.65f,  2.0f, 4.0f }; break; // mac10         <- Kriss Vector
        case 33: h = { 20.0f, 0.38f, 0.11f,  2.0f, 5.0f }; break; // mp5           <- MP5
        case 5:  h = {  7.0f, 1.00f, 0.70f,  2.0f, 6.0f }; break; // m16           <- M16
        case 26: h = { 25.0f, 0.38f, 0.11f,  1.0f, 8.0f }; break; // ak47          <- AK47
        case 9:  h = { 10.0f, 0.80f, 0.55f,  2.0f, 6.0f }; break; // scar          <- SCAR
        case 34: h = {  8.0f, 0.80f, 0.30f,  2.0f, 6.0f }; break; // sg552         <- Galil
        case 6:  h = { 10.0f, 1.00f, 0.50f,  4.0f, 7.0f }; break; // hunting rifle <- SKS
        case 10: h = { 30.0f, 0.38f, 0.11f,  2.0f, 7.0f }; break; // military      <- AutoSniper
        case 35: h = { 12.0f, 0.90f, 0.80f,  2.0f, 9.0f }; break; // awp           <- AWP
        case 36: h = { 12.0f, 0.90f, 0.85f,  4.0f, 9.0f }; break; // scout         <- Kar98
        case 3:                                                    // pump shotgun  <- Shotgun
        case 8:  h = { 17.0f, 1.00f, 0.20f,  2.0f, 9.0f }; break; // chrome        <- Shotgun
        case 4:                                                    // autoshotgun   <- AutoShotgun
        case 11: h = { 30.0f, 0.38f, 0.11f,  2.0f, 7.0f }; break; // spas          <- AutoShotgun
        default: return false;
        }
        out = FromPavlov(h);
        return true;
    }
}

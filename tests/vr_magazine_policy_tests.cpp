#include "../L4D2VR/vr_magazine_policy.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Check(bool result, const char* message)
{
    if (!result)
    {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

int main()
{
    using l4d2vr_magazine::UseNativeReloadFallback;
    using l4d2vr_magazine::WantsNativeReload;
    using l4d2vr_magazine::ResolveBoltPullDistanceMeters;
    const float scale = 43.2f;
    for (const float invalid : {0.0f, -0.02f, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 0.000001f})
    {
        Check(ResolveBoltPullDistanceMeters(invalid,false,scale)==0.055f,
            "An unusable slide calibration must recover a positive physical stroke");
        Check(ResolveBoltPullDistanceMeters(invalid,true,scale)==0.09f,
            "An unusable pump calibration must recover the longer physical pump stroke");
    }
    for (const float calibrated : {0.001f, 0.018f, 0.045f, 0.055f, 0.077f, 0.09f, 0.25f})
    {
        Check(ResolveBoltPullDistanceMeters(calibrated,false,scale)==calibrated &&
            ResolveBoltPullDistanceMeters(calibrated,true,scale)==calibrated,
            "A selected valid weapon or profile override must remain exact for slides and pumps");
    }
    Check(ResolveBoltPullDistanceMeters(0.4f,false,scale)==0.25f &&
        ResolveBoltPullDistanceMeters(0.4f,true,scale)==0.25f,
        "A finite oversized calibration must retain the existing upper limit");
    Check(ResolveBoltPullDistanceMeters(0.0001f,false,5.0f)==0.055f &&
        ResolveBoltPullDistanceMeters(0.0001f,false,scale)==0.0001f,
        "Only calibration that cannot clear the scaled mechanical threshold needs recovery");
    Check(!WantsNativeReload(true,true,false), "A reload gesture must not start stock animation while physical magazines own it");
    Check(WantsNativeReload(false,true,true), "An explicit backend completion must still reach native reload when needed");
    Check(WantsNativeReload(true,false,false), "Unsupported physical hosts must retain normal reload input");
    Check(!WantsNativeReload(false,false,false), "No input or backend pulse must not invent a native reload");
    l4d2vr_magazine::NativeFallbackPulse launcher;
    const bool launcherFallback = UseNativeReloadFallback(true,21,false,false);
    Check(launcher.Update(launcherFallback,true,1u,1000u), "A launcher release action must start native reload");
    Check(launcher.Update(launcherFallback,false,1u,1200u), "The launcher reload pulse must survive button release");
    Check(!launcher.Update(launcherFallback,false,1u,1350u), "The launcher native reload pulse must stop at its deadline");
    Check(!launcher.Update(UseNativeReloadFallback(true,37,false,false),true,2u,1500u), "M60 must not acquire a synthetic reload path");
    Check(!launcher.Update(UseNativeReloadFallback(true,14,false,false),true,3u,1600u), "A pipe bomb must not acquire a reload path");
    Check(!launcher.Update(UseNativeReloadFallback(false,21,false,false),true,4u,1700u), "Disabled manual handling must not own a launcher pulse");
    Check(!launcher.Update(UseNativeReloadFallback(true,5,true,true),true,5u,1800u), "Negotiated rifle magazines must retain their physical transaction path");
    using l4d2vr_magazine::ChamberRoundsAfterEject;
    using l4d2vr_magazine::ShouldEject;
    Check(!ShouldEject(true, false, true, 0, true), "Button mode must not automatically eject an empty magazine");
    Check(ShouldEject(true, true, true, 15, true), "Explicit release must eject a loaded magazine");
    Check(ShouldEject(true, true, true, 0, true), "Explicit release must eject an empty magazine");
    Check(!ShouldEject(true, true, true, 8, false), "A tube-fed shotgun must not eject a detachable magazine");
    Check(ShouldEject(false, false, true, 0, true), "Legacy empty-magazine handling remains available");
    Check(!ShouldEject(true, true, true, 15, true, true), "Release must do nothing while the magazine is already out");
    Check(!ShouldEject(false, false, true, 0, true, true), "An ejected empty magazine must not auto-eject again");
    {
        // One ejection per magazine: the seat latch survives session resets
        // and weapon switches until a magazine is seated again.
        using l4d2vr_magazine::MagazineSeatHistory;
        constexpr std::uintptr_t owner = 501u, rifle = 601u, smg = 602u;
        constexpr std::uint32_t rifleHandle = 0x3005u, smgHandle = 0x3006u;
        MagazineSeatHistory seats;
        seats.ObserveOwner(owner);
        seats.Observe(rifle, rifleHandle, 5, 43);
        Check(!seats.Ejected(owner, rifle, rifleHandle, 5), "A drawn rifle starts with its magazine seated");
        Check(ShouldEject(true, true, true, 43, true, seats.Ejected(owner, rifle, rifleHandle, 5)), "The first press ejects");
        seats.Eject(rifle, rifleHandle, 5, 1);
        seats.Observe(rifle, rifleHandle, 5, 1);
        Check(seats.Ejected(owner, rifle, rifleHandle, 5), "The ejected magazine stays out");
        Check(!ShouldEject(true, true, true, 1, true, seats.Ejected(owner, rifle, rifleHandle, 5)),
            "A second press while the magazine is out ejects nothing");
        seats.Observe(rifle, rifleHandle, 5, 0);
        Check(seats.Ejected(owner, rifle, rifleHandle, 5), "Firing the chambered round is not a refill");
        seats.Observe(smg, smgHandle, 2, 50);
        seats.ObserveOwner(0u);
        Check(seats.Ejected(owner, rifle, rifleHandle, 5), "Switching weapons or losing interaction keeps the rifle's magazine out");
        Check(!seats.Ejected(owner, smg, smgHandle, 2), "Other guns keep their own magazines");
        seats.Observe(rifle, rifleHandle, 5, 0);
        Check(seats.Ejected(owner, rifle, rifleHandle, 5), "Redrawing the rifle still has no magazine");
        Check(!seats.Ejected(owner, rifle, rifleHandle + 4096u, 5) && !seats.Ejected(owner + 1u, rifle, rifleHandle, 5),
            "A recycled gun or another owner cannot inherit the ejected magazine");
        seats.Seat(rifle, rifleHandle, 5);
        Check(!seats.Ejected(owner, rifle, rifleHandle, 5), "Inserting a magazine re-arms the release");
        seats.Eject(rifle, rifleHandle, 5, 43); // stock reload: the clip is untouched until it runs
        seats.Observe(rifle, rifleHandle, 5, 43);
        Check(seats.Ejected(owner, rifle, rifleHandle, 5), "An unfinished stock reload keeps the magazine out");
        seats.Observe(rifle, rifleHandle, 5, 50);
        Check(!seats.Ejected(owner, rifle, rifleHandle, 5), "A native refill means a magazine is seated");
        seats.Eject(rifle, rifleHandle, 5, 1);
        seats.ObserveOwner(owner + 1u);
        Check(!seats.Ejected(owner + 1u, rifle, rifleHandle, 5), "A new player starts with seated magazines");
        MagazineSeatHistory unserialed;
        unserialed.ObserveOwner(owner);
        unserialed.Observe(rifle, 0u, 5, 10);
        unserialed.Eject(rifle, 0u, 5, 1);
        Check(unserialed.Ejected(owner, rifle, 0u, 5), "Without a serial the current draw still keeps its magazine out");
        unserialed.Observe(smg, smgHandle, 2, 50);
        Check(!unserialed.Ejected(owner, rifle, 0u, 5), "Without a serial a redraw cannot be told from a new gun");
    }
    Check(ChamberRoundsAfterEject(-1) == 0, "Unknown clip data must not create ammunition");
    Check(ChamberRoundsAfterEject(0) == 0, "An empty pistol must not gain an infinite-reserve chambered round");
    Check(ChamberRoundsAfterEject(1) == 1, "Removing a magazine must preserve the last existing chambered round");
    Check(ChamberRoundsAfterEject(15) == 1, "A tactical reload must preserve exactly one existing round");
    Check(ChamberRoundsAfterEject(30) == 1, "Clip capacity must not increase the retained chamber count");
    Check(!l4d2vr_magazine::ChamberReadyAfterEject(true, 1), "An ejection ACK cannot cycle a previously empty physical chamber");
    Check(l4d2vr_magazine::ChamberReadyAfterEject(false, 1), "An existing loaded chamber survives a tactical ejection ACK");
    Check(!l4d2vr_magazine::ChamberReadyAfterEject(false, 0), "The native empty result cannot preserve a chambered round");
    Check(!l4d2vr_magazine::ChamberReadyAfterEject(false, -1), "An unknown native count cannot ready the chamber");
    Check(l4d2vr_magazine::ShouldResumeMagazineCycle(true,false,true,1), "A retained empty chamber resumes its existing cycle even with a partially loaded clip");
    Check(!l4d2vr_magazine::ShouldResumeMagazineCycle(true,false,false,30), "A chambered tactical reload must not acquire another cycle");
    Check(!l4d2vr_magazine::ShouldResumeMagazineCycle(true,true,true,30), "An active reload keeps its own transaction and hand stage");
    Check(!l4d2vr_magazine::ShouldResumeMagazineCycle(true,false,true,0), "An empty magazine needs ammunition before cycle recovery");
    Check(l4d2vr_magazine::MayFinishShotgunLoading(true,true,1,0,true), "Returning to a loaded shotgun permits cycling without another shell");
    Check(l4d2vr_magazine::MayFinishShotgunLoading(true,true,0,1,true), "Existing empty-stroke interaction remains available after loading activity");
    Check(!l4d2vr_magazine::MayFinishShotgunLoading(true,true,0,0,true), "A fresh empty tube must accept ammunition first");
    Check(!l4d2vr_magazine::MayFinishShotgunLoading(true,false,8,2,true), "A loaded shotgun chamber does not need another cycle");
    Check(!l4d2vr_magazine::MayFinishShotgunLoading(true,true,1,0,false), "Region recovery cannot grab the action without grip input");
    l4d2vr_magazine::ChamberHistory optionalRetention;
    optionalRetention.ObserveOwner(true,100u);
    optionalRetention.Observe(200u,0x3005u,5,0);
    Check(optionalRetention.HasEmptyChamber(100u,200u,0x3005u,5), "Enabled retention keeps the unfinished physical chamber");
    optionalRetention.ObserveOwner(false,100u); // existing auto-reload suppression retention option disabled
    Check(!optionalRetention.HasEmptyChamber(100u,200u,0x3005u,5), "Disabled retention cannot introduce a new switch-time fire block");
    optionalRetention.ObserveOwner(true,100u);
    Check(!optionalRetention.Observe(200u,0x3005u,5,50), "Re-enabled retention starts from the current loaded draw");
    Check(l4d2vr_magazine::MayCatchEjectedMagazine(true, true, 0.04f, 0.05f), "Grip held under the well can catch an ejected magazine");
    Check(!l4d2vr_magazine::MayCatchEjectedMagazine(false, true, 0.0f, 0.05f), "Ejection alone cannot attach a magazine to the support hand");
    Check(!l4d2vr_magazine::MayCatchEjectedMagazine(true, false, 0.0f, 0.05f), "Lost tracking cannot catch an ejected magazine");
    Check(!l4d2vr_magazine::MayCatchEjectedMagazine(true, true, 0.06f, 0.05f), "A distant support hand cannot catch ammunition");
    Check(!l4d2vr_magazine::MayCatchEjectedMagazine(true, true, std::numeric_limits<float>::quiet_NaN(), 0.05f), "Invalid pose distances cannot attach ammunition");
    // The pouch is reached from the hand itself, and a hand on the bolt racks.
    Check(l4d2vr_magazine::MayTakeFreshAmmo(0.05f, 0.20f, 1e9f, 0.10f), "A hand at the pouch takes a spare");
    Check(!l4d2vr_magazine::MayTakeFreshAmmo(0.25f, 0.20f, 1e9f, 0.10f), "A hand beyond the pouch reach takes nothing");
    Check(!l4d2vr_magazine::MayTakeFreshAmmo(0.15f, 0.20f, 0.0f, 0.10f), "Racking the slide next to the pouch is a rack, not a spare");
    Check(l4d2vr_magazine::MayTakeFreshAmmo(0.02f, 0.20f, 0.08f, 0.10f), "A hand in the pouch near the gun still takes a spare");
    Check(!l4d2vr_magazine::MayTakeFreshAmmo(std::numeric_limits<float>::quiet_NaN(), 0.20f, 1e9f, 0.10f), "No pouch pose, no spare");
    {
        l4d2vr_magazine::FreshInsertGate fresh;
        Check(!fresh.Update(true), "A spare taken with the hand at the well does not seat in the same motion");
        Check(!fresh.Update(true), "Holding it there still does not seat it");
        Check(!fresh.Update(false), "Taking it out of the well arms it");
        Check(fresh.Update(true), "Pushing it back in seats it");
        fresh.Reset();
        Check(!fresh.Update(true), "The next spare needs its own trip out of the well");
    }
    l4d2vr_magazine::RetainedInsertGate retained;
    Check(!retained.Update(true, 0.0f, 0.03f), "Catching at the well must not automatically reinsert the magazine");
    Check(!retained.Update(false, 0.01f, 0.03f), "A small overlap change is not a withdrawal gesture");
    Check(!retained.Update(true, 0.04f, 0.03f), "Travel alone must not arm reinsertion while the magazine remains in the well");
    Check(!retained.Update(false, 0.04f, 0.03f), "Withdrawing arms a later return without loading ammunition yet");
    Check(retained.Update(true, 0.01f, 0.03f), "A withdrawn magazine can return to its socket");
    retained.Reset();
    Check(!retained.Update(true, 0.04f, 0.03f), "Another caught magazine needs its own withdrawal gesture");
    using namespace l4d2vr_magazine;
    ChamberHistory chambers;
    chambers.ObserveOwner(true, 100u);
    Check(chambers.Observe(10u, 0x1001u, 1, 0), "Firing the last round must leave an empty chamber");
    Check(chambers.Observe(10u, 0x1001u, 1, 15), "Inserting or predicting a full magazine must not cycle its slide");
    Check(!chambers.Observe(20u, 0x1002u, 2, 50), "Drawing another loaded gun must not inherit that empty chamber");
    Check(chambers.Observe(10u, 0x1001u, 1, 15), "Drawing the pistol again must retain the unfinished cycle");
    chambers.Complete(20u);
    Check(chambers.Observe(10u, 0x1001u, 1, 15), "Cycling another gun cannot ready the pistol");
    chambers.Complete(10u);
    Check(!chambers.Observe(10u, 0x1001u, 1, 15), "A completed cycle readies only the current weapon");
    Check(chambers.Observe(10u, 0x1001u, 1, 0), "Spending the chamber again requires a new cycle");
    Check(!chambers.Observe(10u, 0x2001u, 1, 15), "A new native entity serial must not inherit deleted-object state");
    Check(chambers.Observe(30u, 0u, 1, 0), "A missing serial must still support the current physical reload");
    Check(chambers.Observe(30u, 0u, 1, 15), "Missing serial data must not automatically chamber an inserted magazine");
    chambers.Observe(40u, 0u, 19, -1);
    Check(!chambers.Observe(30u, 0u, 1, 15), "Unknown entity identities must not survive an intervening draw");
    Check(chambers.Observe(20u, 0x1002u, 2, 0), "The other weapon keeps its own chamber history");
    chambers.Forget(20u);
    Check(!chambers.Observe(20u, 0x1002u, 2, 50), "Native reload fallback must not retain a physical-only latch");
    chambers.Observe(10u, 0x2001u, 1, 0);
    chambers.ObserveOwner(true, 200u);
    Check(!chambers.Observe(10u, 0x2001u, 1, 15), "Replacing the player must release the previous inventory history");
    chambers.Observe(10u, 0x2001u, 1, 0);
    chambers.ObserveOwner(false, 200u);
    chambers.ObserveOwner(true, 200u);
    Check(!chambers.Observe(10u, 0x2001u, 1, 15), "Death, incap or disabled physical reload must clear ownership");
    {
        // The client predicts the last shot; the host, a tick behind when the
        // trigger went up, never fires it and sends the round back.
        ChamberHistory host;
        host.ObserveOwner(true, 100u);
        Check(host.Observe(10u, 0x1005u, 5, 0, 360, false, 1000u), "A predicted last shot empties the chamber");
        Check(!host.Observe(10u, 0x1005u, 5, 1, 360, false, 1080u), "A last shot the host never fired leaves that round chambered");
        Check(!host.HasEmptyChamber(100u, 10u, 0x1005u, 5), "No rack is demanded for a round the host kept");
        Check(host.Observe(10u, 0x1005u, 5, 0, 360, false, 1500u), "Firing that round empties the chamber again");
        Check(host.Observe(10u, 0x1005u, 5, 0, 360, false, 2600u), "An empty gun stays empty");
        Check(host.Observe(10u, 0x1005u, 5, 50, 360, false, 2700u), "Rounds after the overturn window are a refill: the chamber still needs a rack");

        ChamberHistory insert;
        insert.ObserveOwner(true, 100u);
        insert.Observe(10u, 0x1005u, 5, 0, 360, false, 1000u);
        Check(insert.Observe(10u, 0x1005u, 5, 0, 360, true, 1100u), "Reloading an empty gun keeps the empty chamber");
        Check(insert.Observe(10u, 0x1005u, 5, 50, 311, false, 1200u), "A fresh magazine does not chamber a round");
        Check(insert.Observe(10u, 0x1005u, 5, 50, 311, false, 1300u), "Nor does a later frame of it");

        ChamberHistory reserve;
        reserve.ObserveOwner(true, 100u);
        reserve.Observe(10u, 0x1005u, 5, 0, 360, false, 1000u);
        Check(reserve.Observe(10u, 0x1005u, 5, 50, 310, false, 1100u), "A refill taken from the reserve (a stock reload) does not chamber a round");

        ChamberHistory reloading;
        reloading.ObserveOwner(true, 100u);
        Check(reloading.Observe(10u, 0x1005u, 5, 0, 360, true, 1000u), "A gun emptied while reloading is empty");
        Check(reloading.Observe(10u, 0x1005u, 5, 1, 360, false, 1100u), "Rounds that arrive with a reload are never an overturned shot");

        ChamberHistory unknown;
        unknown.ObserveOwner(true, 100u);
        unknown.Observe(10u, 0x1005u, 5, 0);
        Check(unknown.Observe(10u, 0x1005u, 5, 1), "Without a reserve reading the empty chamber is kept");

        ChamberHistory completed;
        completed.ObserveOwner(true, 100u);
        completed.Observe(10u, 0x1005u, 5, 0, 360, false, 1000u);
        completed.Complete(10u);
        Check(completed.Observe(10u, 0x1005u, 5, 0, 360, false, 1100u), "A rack on an empty gun leaves it empty");
    }

    // A tactical top-up can end after one shell. The cumulative number of
    // inserted shells does not keep firing blocked for the rest of the session.
    Check(ShotgunBlocksFire(false, true, false, false, false), "Holding a shell at the loading stage blocks firing");
    Check(ShotgunBlocksFire(false, false, false, false, ShellSettlementPending(100u, 1100u)),
        "A queued shell must settle before a shot can spend the predicted clip");
    Check(!ShotgunBlocksFire(false, false, false, false, ShellSettlementPending(200u, 0u)),
        "One confirmed shell in a partially filled tube must allow firing from the existing chamber");
    Check(ShotgunBlocksFire(true, false, true, false, false), "The resulting shot must wait for the pump cycle");
    Check(!ShotgunBlocksFire(false, false, false, false, false), "Completing the cycle must not require filling the tube");
    Check(ShotgunBlocksFire(true, false, false, false, false), "Loading a shell into an empty gun does not chamber it");
    Check(ShotgunBlocksFire(false, false, false, true, false), "An early slide cycle must still wait for backend ammo");
    Check(!ShellSettlementPending(1100u, 1100u), "A lost server settlement cannot retain a permanent firing lock");
    Check(!ShellSettlementPending(1200u, 1100u), "Expired settlements must remain unlocked");
    l4d2vr_magazine::NativeReloadLedger local, teammate;
    local.Observe(100, 10u, true);
    Check(local.Blocks(10u) && !teammate.Blocks(10u), "Physical reload suppression belongs only to its sender");
    local.Observe(101, 10u, false);
    local.Observe(100, 10u, true);
    Check(!local.Blocks(10u), "An older backup command must not cancel an authorized reload");
    local.Observe(102, 20u, true);
    Check(local.Blocks(20u) && !local.Blocks(10u), "Changing weapons must release the old weapon's reload gate");
    local.Observe(103, 0u, true);
    Check(!local.Blocks(20u) && !local.Blocks(0u), "Empty inventory must not retain a reload gate");
    local.Observe(104, 20u, true, false, 1u);
    local.Observe(105, 20u, false, true, 1u);
    Check(local.Blocks(20u), "A remote pull or grip action must not silently permit automatic reload");
    local.Observe(106, 30u, false, true, 1u);
    Check(!local.Blocks(20u) && !local.Blocks(30u), "A transient action on a new weapon must not inherit another weapon's gate");
    local.Observe(107, 30u, true, false, 1u);
    local.Observe(1, 40u, true, false, 2u);
    Check(local.Blocks(40u) && !local.Blocks(30u), "A replaced player must get a new command sequence and ownership");
    local.Observe(2, 40u, false, false, 2u);
    local.Observe(1, 40u, true, false, 2u);
    Check(!local.Blocks(40u), "Backup commands from the current player must still be rejected");
    local.Observe(200, 40u, true, false, 2u, 1u);
    local.Observe(1, 40u, false, false, 2u, 2u);
    Check(!local.Blocks(40u), "Reusing a player address with a new edict serial must clear the old reload gate");
    local.Observe(2, 50u, true, false, 2u, 2u);
    Check(local.Blocks(50u), "The replacement edict must accept its own command sequence");
}

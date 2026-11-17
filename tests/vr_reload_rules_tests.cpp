// 2.0.12 (s12 review): reload state-machine rules for every weapon class.
#include "../L4D2VR/vr_reload_rules.h"
#include "../L4D2VR/vr_magazine_policy.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static int g_Failures = 0;
static void Check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_Failures; }
}

using namespace l4d2vr_reload_rules;

static void FinishLoadingIsEdgeTriggered()
{
    // Issue 2a: the input still held from carrying the shell never arms the bolt.
    Check(!MayFinishShotgunLoading(true, true, 1, 1, false, false, false, 1000u),
        "a held (level) input after a shell confirm does not finish loading");
    Check(!MayFinishShotgunLoading(true, true, 1, 1, true, false, true, 1000u),
        "a press before the input was let go once after the confirm does not count");
    Check(!MayFinishShotgunLoading(true, true, 1, 1, true, false, false, 100u),
        "a press within 250 ms of a shell confirm does not count");
    Check(MayFinishShotgunLoading(true, true, 1, 1, true, false, false, 300u),
        "a fresh press after release and 250 ms arms the bolt");
    Check(MayFinishShotgunLoading(true, true, 2, 0, false, true, false, 0u),
        "the pump's own fore-end grip may cycle at once");
    Check(!MayFinishShotgunLoading(true, true, 0, 0, true, false, false, 9999u),
        "an empty tube takes shells first");
    Check(!MayFinishShotgunLoading(true, false, 3, 1, true, false, false, 9999u),
        "a loaded chamber needs no cycle");
    Check(!MayFinishShotgunLoading(false, true, 3, 1, true, false, false, 9999u), "magazine guns are not shell guns");
}

static void PouchPerWeapon()
{
    // Every shotgun: one shell per squeeze, rate 0.15 s, reserve, tube.
    for (int id : {3, 4, 8, 11})
    {
        PouchInput in{};
        in.pressed = true; in.shellMode = true; in.clip = 2; in.maxClip = 8; in.reserve = 20; in.weaponId = id;
        Check(EvaluatePouchTake(in) == PouchResult::Take, "a shotgun takes a shell at the pouch");
        in.secondsSinceLastTake = 0.10f;
        Check(EvaluatePouchTake(in) == PouchResult::RateLimited, "shotguns: at most one shell per 0.15 s");
        in.secondsSinceLastTake = 0.16f;
        Check(EvaluatePouchTake(in) == PouchResult::Take, "shotguns: 0.16 s later the next shell comes");
        in.reserve = 0;
        Check(EvaluatePouchTake(in) == PouchResult::NoReserve, "reserve 0: no shell (E14)");
        in.reserve = 5; in.clip = 8;
        Check(EvaluatePouchTake(in) == PouchResult::TubeFull, "full tube: no shell");
        in.clip = 2; in.pressed = false;
        Check(EvaluatePouchTake(in) == PouchResult::NotPressed, "no press, no shell (one per squeeze)");
        in.pressed = true; in.gunInOtherHand = false;
        Check(EvaluatePouchTake(in) == PouchResult::NoGunInOtherHand, "no gun in the other hand: nothing");
        Check(!ShowPouchSpare(true, 2, 8, 0, false), "no phantom shell at the pouch with reserve 0");
        Check(!ShowPouchSpare(true, 8, 8, 10, false), "no shell shown with a full tube");
        Check(ShowPouchSpare(true, 2, 8, 10, false), "a shell shows while it can be taken");
    }
    // Magazine guns (G8): 0.25 s, reserve; sidearms are infinite.
    for (int id : {2, 5, 6, 7, 9, 10, 26, 33, 34, 35, 36})
    {
        PouchInput in{};
        in.pressed = true; in.clip = 0; in.maxClip = 30; in.reserve = 0; in.weaponId = id;
        Check(EvaluatePouchTake(in) == PouchResult::NoReserve, "magazine guns: no spare with reserve 0 (G8)");
        in.reserve = 60; in.secondsSinceLastTake = 0.2f;
        Check(EvaluatePouchTake(in) == PouchResult::RateLimited, "magazine guns: 0.25 s between spares");
        in.secondsSinceLastTake = 0.3f;
        Check(EvaluatePouchTake(in) == PouchResult::Take, "magazine guns take a spare");
    }
    for (int id : {1, 32})
    {
        PouchInput in{};
        in.pressed = true; in.reserve = 0; in.infiniteReserve = true; in.weaponId = id;
        Check(EvaluatePouchTake(in) == PouchResult::Take, "pistol/magnum: the pouch is infinite");
    }
    Check(PouchGrabIntervalSeconds(11) == 0.15f && PouchGrabIntervalSeconds(32) == 0.25f, "Pavlov MagGrabRateMax");
}

static void PouchBoltArbitration()
{
    using l4d2vr_magazine::MayTakeFreshAmmo;
    // Issue 2c: a gun held at the chest puts its receiver at the pouch.
    Check(MayTakeFreshAmmo(0.06f, 0.15f, 0.05f, 0.10f), "near both: the shell wins unless the hand is on the bolt");
    Check(!MayTakeFreshAmmo(0.06f, 0.15f, 0.00f, 0.10f), "hand on the bolt: a rack, not a shell");
    Check(MayTakeFreshAmmo(0.02f, 0.15f, 0.01f, 0.10f), "bolt within 3 cm but not 2 cm nearer: shell");
    // Same in source units (43.2 units per metre).
    const float u = 43.2f;
    Check(MayTakeFreshAmmo(0.06f * u, 0.15f * u, 0.05f * u, 0.10f * u, u), "scaled: shell wins");
    Check(!MayTakeFreshAmmo(0.06f * u, 0.15f * u, 0.0f, 0.10f * u, u), "scaled: bolt wins when on it");
    Check(!MayTakeFreshAmmo(0.20f, 0.15f, 1.0f, 0.10f), "out of pouch reach: nothing");
}

static void HeldSpareIsSticky()
{
    // Issue 2d: a tap no longer loses the shell 0.2 s later.
    Check(EvaluateHeldSpare(false, false, false, 0.23f, 0.05f, false) == HeldSpare::Keep, "released 0.23 s after the take: kept");
    Check(EvaluateHeldSpare(false, false, false, 2.0f, 1.0f, false) == HeldSpare::Keep, "released 1 s: still kept");
    Check(EvaluateHeldSpare(false, false, false, 5.0f, 3.1f, false) == HeldSpare::DropReleasedTooLong, "released 3 s: dropped");
    Check(EvaluateHeldSpare(false, true, true, 1.0f, 0.0f, false) == HeldSpare::DropSecondPress, "second press away from the port drops it");
    Check(EvaluateHeldSpare(false, true, true, 1.0f, 0.0f, true) == HeldSpare::Keep, "a press at the port inserts, it does not drop");
    Check(EvaluateHeldSpare(false, false, false, 9.0f, 9.0f, true) == HeldSpare::Keep, "let go at the port: it goes in");
    Check(EvaluateHeldSpare(false, true, true, 0.05f, 0.0f, false) == HeldSpare::Keep, "the take's own press does not drop it");
    Check(EvaluateHeldSpare(true, false, false, 0.05f, 0.0f, false) == HeldSpare::Drop, "a caught old magazine falls when let go");
    Check(EvaluateHeldSpare(true, true, false, 5.0f, 0.0f, false) == HeldSpare::Keep, "a held old magazine stays");
}

static void ReloadCueMask()
{
    HintInput base{};
    base.enabled = true; base.interactionAvailable = true;
    // Issue 4: SPAS/auto dry with shells loaded and an empty chamber.
    for (int id : {4, 11})
    {
        (void)id;
        HintInput in = base; in.shellMode = true; in.chamberEmpty = true; in.clip = 3; in.waitingForFresh = true;
        Check((BuildReloadHintMask(in) & kHintBolt) != 0u, "SPAS/auto: bolt cue while it cannot fire");
        Check((BuildReloadHintMask(in) & kHintPouch) != 0u, "shotgun: the pouch cue stays while the tube is open");
        in.holdingSpare = true; in.waitingForFresh = false;
        Check((BuildReloadHintMask(in) & kHintBolt) == 0u, "no bolt cue while a shell is in the hand");
        in.holdingSpare = false; in.chamberEmpty = false;
        Check((BuildReloadHintMask(in) & kHintBolt) == 0u, "chambered: no bolt cue");
        in.chamberEmpty = true; in.clip = 0;
        Check((BuildReloadHintMask(in) & kHintBolt) == 0u, "empty tube: shells first, no bolt cue");
    }
    {
        HintInput in = base; in.shellMode = true; in.manualPumpBlocksFire = true; in.clip = 4;
        Check((BuildReloadHintMask(in) & kHintBolt) != 0u, "pump/chrome after a shot: pump cue");
    }
    {
        // Magazine guns: magazine out -> spare, not bolt; resumed empty chamber -> bolt.
        HintInput in = base; in.waitingForFresh = true; in.clip = 0;
        Check(BuildReloadHintMask(in) == kHintPouch, "magazine out: only the pouch glows");
        in.pouchHasSpare = false;
        Check(BuildReloadHintMask(in) == 0u, "reserve 0: no pouch glow");
        HintInput idle = base; idle.chamberEmpty = true; idle.clip = 30;
        Check(BuildReloadHintMask(idle) == kHintBolt, "drawn again with an empty chamber: rack cue");
    }
    {
        // Issue 8 / G7: never a stale cue.
        HintInput in = base; in.waitingForBoltGrab = true;
        in.interactionAvailable = false;
        Check(BuildReloadHintMask(in) == 0u, "no cue when the update did not run for a held gun");
        HintInput dual = base; dual.dualPistols = true; dual.waitingForBoltGrab = true; dual.manualPumpBlocksFire = true;
        Check(BuildReloadHintMask(dual) == 0u, "dual pistols: never the previous gun's bolt cue");
        dual.dualNeedsRackOther = true;
        Check(BuildReloadHintMask(dual) == kHintOtherSlide, "dual pistols: the other gun's slide when it needs a rack");
        HintInput off = base; off.enabled = false; off.waitingForBoltGrab = true;
        Check(BuildReloadHintMask(off) == 0u, "glow off: nothing");
    }
}

static void IncapRules()
{
    Check(UsableWhileIncapacitated(1) && UsableWhileIncapacitated(32), "pistol and magnum work while down");
    for (int id : {2, 3, 4, 5, 6, 8, 9, 10, 11, 21, 26, 33, 34, 35, 36, 37})
        Check(!UsableWhileIncapacitated(id), "long guns are not held while down");
    Check(PhysicalReloadStateAllowed(true, false, false, false, true, 1), "downed with a pistol: physical reload");
    Check(!PhysicalReloadStateAllowed(true, false, false, true, true, 1), "hanging from a ledge: no reload");
    Check(!PhysicalReloadStateAllowed(true, false, true, false, false, 5), "pinned: no reload");
    Check(!PhysicalReloadStateAllowed(false, false, false, false, false, 5), "dead: no reload");
    Check(PhysicalReloadStateAllowed(true, false, false, false, false, 11), "standing: every gun");
}

static void GateImpulses()
{
    Check(ResolveGateImpulse(0u, false) == GateImpulse::Block, "old server: per-command block");
    Check(ResolveGateImpulse(kGapNotFirearm, false) == GateImpulse::None, "old server: nothing otherwise");
    Check(ResolveGateImpulse(0u, true) == GateImpulse::StickyBlock, "sticky server: sticky block");
    Check(ResolveGateImpulse(kGapNotFirearm, true) == GateImpulse::None,
        "sticky: a command built for a grenade carries nothing (the block stands)");
    Check(ResolveGateImpulse(kGapImpulseBusy, true) == GateImpulse::None, "sticky: busy impulse keeps the gate");
    Check(ResolveGateImpulse(kGapNoBackend, true) == GateImpulse::Allow, "sticky: GL/M60 (no backend) allow");
    Check(ResolveGateImpulse(kGapDualNativeFallback, true) == GateImpulse::Allow, "sticky: dual native fallback allows");
    Check(ResolveGateImpulse(kGapIncapacitated, true) == GateImpulse::Allow, "sticky: stock reload while down without IncapReload");
    Check(ResolveGateImpulse(kGapPhysicalOff, true) == GateImpulse::Allow, "sticky: physical reload off allows");
}

static void StickyLedger()
{
    using l4d2vr_magazine::NativeReloadLedger;
    const std::uintptr_t grenade = 0x100, spas = 0x200, gl = 0x300, pistol = 0x400;
    {
        // 2.0.11 behaviour (no sticky impulse): the gap of issue 14.
        NativeReloadLedger l;
        l.Observe(1, spas, true, false, 7u, 0u);
        l.Observe(2, grenade, false, false, 7u, 0u);
        l.Observe(3, spas, false, false, 7u, 0u);
        Check(!l.Blocks(spas), "legacy: a stale non-block command opens the gate (old clients unchanged)");
    }
    {
        // Issue 14: server switches grenade -> empty SPAS; stale commands carry nothing.
        NativeReloadLedger l;
        l.Observe(1, grenade, false, false, 7u, 0u, true, false, 1000u);
        Check(l.Sticky(), "a sticky block enters sticky mode");
        for (int c = 2; c <= 7; ++c) l.Observe(c, spas, false, false, 7u, 0u, false, false, 1000u + c * 10u);
        Check(l.Blocks(spas), "six stale commands after a server-side switch: the SPAS stays blocked");
        // A stale allow built for the grenade arrives within the settle window.
        l.Observe(8, spas, false, false, 7u, 0u, false, true, 1100u);
        Check(l.Blocks(spas), "an allow within 300 ms of a server-side switch is ignored");
        l.Observe(9, spas, true, false, 7u, 0u, true, false, 1200u);
        Check(l.Blocks(spas), "the client's own block for the SPAS");
    }
    {
        // Explicit allow (GL drawn by the client) after the settle window.
        NativeReloadLedger l;
        l.Observe(1, spas, true, false, 7u, 0u, true, false, 0u);
        l.Observe(2, gl, false, false, 7u, 0u, false, true, 50u);
        Check(l.Blocks(gl), "allow right after the draw: still blocked");
        l.Observe(3, gl, false, false, 7u, 0u, false, true, 400u);
        Check(!l.Blocks(gl), "allow after 300 ms: the GL reloads natively");
        Check(l.Blocks(spas), "only the allowed weapon is open");
        l.Observe(4, pistol, false, false, 7u, 0u, false, false, 500u);
        Check(l.Blocks(pistol), "a server switch to the pistol with no impulse: blocked");
        l.Observe(5, gl, true, false, 7u, 0u, true, false, 600u);
        Check(l.Blocks(gl), "a block closes it again");
    }
    {
        NativeReloadLedger l;
        l.Observe(1, spas, true, false, 7u, 0u, true, false, 0u);
        l.Observe(2, spas, false, false, 8u, 0u, false, false, 10u);
        Check(!l.Sticky() && !l.Blocks(spas), "a new owner (respawn, reconnect) starts unsticky");
        l.Observe(3, spas, false, false, 8u, 0u, false, true, 10u);
        Check(!l.Blocks(spas), "non-sticky: allow is a native command");
    }
}

static void HandPullStateMachine()
{
    MagazineHandPull pull;
    Check(!pull.Begin(false, true, 0.01f, 0.05f, true), "a hand holding something cannot pull the magazine");
    Check(!pull.Begin(true, false, 0.01f, 0.05f, true), "no press, no grip");
    Check(!pull.Begin(true, true, 0.08f, 0.05f, true), "out of reach of the seated magazine");
    Check(!pull.Begin(true, true, 0.01f, 0.05f, false), "no seated magazine to pull");
    Check(pull.Begin(true, true, 0.01f, 0.05f, true), "an empty hand grips the seated magazine");
    Check(pull.Update(true, 0.02f, 0.01f, 0.06f, 0.10f) == MagazineHandPull::State::Gripping, "pulling along the well");
    Check(pull.Update(true, 0.07f, 0.02f, 0.06f, 0.10f) == MagazineHandPull::State::Pulled, "past the threshold: out");
    pull.Reset();
    Check(pull.Begin(true, true, 0.0f, 0.05f, true), "grip again");
    Check(pull.Update(true, 0.01f, 0.12f, 0.06f, 0.10f) == MagazineHandPull::State::Aborted,
        "leaving the slide bounds sideways aborts (Pavlov MagSlideBounds)");
    pull.Reset();
    pull.Begin(true, true, 0.0f, 0.05f, true);
    Check(pull.Update(false, 0.03f, 0.0f, 0.06f, 0.10f) == MagazineHandPull::State::Aborted,
        "letting go before the threshold leaves it seated");
    Check(pull.Update(true, 0.09f, 0.0f, 0.06f, 0.10f) == MagazineHandPull::State::Aborted, "an aborted pull stays aborted");
}

int main()
{
    FinishLoadingIsEdgeTriggered();
    PouchPerWeapon();
    PouchBoltArbitration();
    HeldSpareIsSticky();
    ReloadCueMask();
    IncapRules();
    GateImpulses();
    StickyLedger();
    HandPullStateMachine();
    // Weapon groups used everywhere above.
    for (int id = 0; id < 40; ++id)
        if (UsesPhysicalReload(id))
            Check(IsShellFed(id) != HasDetachableMagazine(id), "a physical gun is shell-fed xor magazine-fed");
    Check(!UsesPhysicalReload(21) && !UsesPhysicalReload(37) && !UsesPhysicalReload(20), "GL, M60, chainsaw: stock reload");
    if (g_Failures) { std::fprintf(stderr, "%d failure(s)\n", g_Failures); return 1; }
    std::puts("vr_reload_rules_tests: all passed");
    return 0;
}

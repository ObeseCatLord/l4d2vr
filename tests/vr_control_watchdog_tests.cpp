// 2.0.12 (s12 E4/issue 3, coordinator request): no hands / no control watchdog.
#include "../L4D2VR/vr_control_watchdog.h"
#include <cstdio>

static int g_Failures = 0;
static void Check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_Failures; }
}

using namespace l4d2vr_control_watchdog;

static Sample Live()
{
    Sample s{};
    s.inGame = true; s.alive = true; s.firstPerson = true; s.hasActiveWeapon = true;
    return s;
}

int main()
{
    {
        // E4: the last item dropped, the game refuses the empty SPAS: no weapon.
        Watchdog w;
        Sample s = Live(); s.hasActiveWeapon = false;
        Check(w.Update(s, 1000u).recover == 0u, "no weapon for a moment: nothing yet");
        Check(w.Update(s, 3400u).recover == 0u, "2.4 s: still waiting");
        Result r = w.Update(s, 3600u);
        Check((r.recover & kNoActiveWeapon) != 0u && (r.stuck & kNoActiveWeapon) != 0u, "2.6 s without a weapon: recover");
        Check(w.Update(s, 5000u).recover == 0u, "no recovery storm");
        Check((w.Update(s, 13700u).recover & kNoActiveWeapon) != 0u, "still stuck 10 s later: recover again");
        s.hasActiveWeapon = true;
        r = w.Update(s, 14000u);
        Check((r.cleared & kNoActiveWeapon) != 0u && r.stuck == 0u, "a weapon arrives: cleared");
        Check(w.Recoveries(kNoActiveWeapon) == 2u, "two recoveries counted");
    }
    {
        // States where the base game takes the weapons away are not stuck.
        for (int i = 0; i < 4; ++i)
        {
            Watchdog w;
            Sample s = Live(); s.hasActiveWeapon = false;
            if (i == 0) s.ledge = true;
            if (i == 1) s.controlledBySI = true;
            if (i == 2) s.mounted = true;
            if (i == 3) s.useAction = true;
            w.Update(s, 0u);
            Check(w.Update(s, 20000u).stuck == 0u, "ledge / pinned / mounted gun / use action: not stuck");
        }
        Watchdog w;
        Sample s = Live(); s.hasActiveWeapon = false; s.handsStowed = true;
        w.Update(s, 0u);
        Check(w.Update(s, 20000u).stuck == 0u, "hands empty on purpose (item put away): not stuck");
        Sample dead = Live(); dead.alive = false; dead.hasActiveWeapon = false;
        w.Update(dead, 0u);
        Check(w.Update(dead, 20000u).stuck == 0u, "dead or spectating: not watched");
    }
    {
        // A held weapon whose viewmodel (the hands) is never drawn.
        Watchdog w;
        Sample s = Live(); s.msSinceViewmodelDrawn = 5000u;
        w.Update(s, 0u);
        Check((w.Update(s, 4100u).recover & kNoViewmodel) != 0u, "4 s without hands: recover");
        Sample tp = Live(); tp.firstPerson = false; tp.msSinceViewmodelDrawn = 99999u;
        Watchdog w2; w2.Update(tp, 0u);
        Check((w2.Update(tp, 9000u).stuck & kNoViewmodel) == 0u, "third person (heal, revive): no viewmodel expected");
        Sample incap = Live(); incap.incapacitated = true; incap.msSinceViewmodelDrawn = 99999u;
        Watchdog w3; w3.Update(incap, 0u);
        Check((w3.Update(incap, 9000u).stuck & kNoViewmodel) == 0u, "downed: the game's own view decides");
    }
    {
        // Gate bits: suppressed input / control not ready are stuck; a menu or pause is not.
        Watchdog w;
        Sample s = Live(); s.gateBits = 1024u;
        w.Update(s, 0u);
        Check((w.Update(s, 6100u).recover & kInputGate) != 0u, "input suppressed for 6 s: recover");
        Watchdog w2;
        Sample menu = Live(); menu.gateBits = 128u | 64u;
        w2.Update(menu, 0u);
        Check(w2.Update(menu, 60000u).stuck == 0u, "a menu cursor or pause is a real state");
        Watchdog w3;
        Sample ready = Live(); ready.controlReady = false;
        w3.Update(ready, 0u);
        Check((w3.Update(ready, 8100u).recover & kControlNotReady) != 0u, "control never ready: recover");
    }
    {
        // Reload states.
        Watchdog w;
        Sample s = Live(); s.reloadState = 7; s.reloadWaitsForServer = true;
        w.Update(s, 0u);
        Check(w.Update(s, 5000u).stuck == 0u, "a transaction in flight 5 s: not yet");
        Check((w.Update(s, 6100u).recover & kReloadStuck) != 0u, "6 s waiting for a host answer: reset");
        Sample loading = Live(); loading.reloadState = 3;
        Watchdog w2; w2.Update(loading, 0u);
        Check(w2.Update(loading, 60000u).stuck == 0u, "waiting for a spare for a minute: fine");
        Check((w2.Update(loading, 121000u).recover & kReloadStuck) != 0u, "two minutes unchanged: reset");
        loading.reloadState = 4;
        Check(w2.Update(loading, 121500u).stuck == 0u, "a state change restarts the clock");
    }
    {
        // Holster draw refused (G4): the game did not deploy the item within 300 ms.
        Watchdog w;
        Sample s = Live(); s.drawPending = true; s.msSinceDrawRequest = 200u;
        Check(w.Update(s, 0u).stuck == 0u, "200 ms: still coming");
        s.msSinceDrawRequest = 320u;
        Check((w.Update(s, 120u).recover & kDrawRefused) != 0u, "320 ms and not active: refused");
        s.drawArrived = true;
        Check(w.Update(s, 200u).stuck == 0u, "arrived: fine");
    }
    {
        Check(PickRecoverySlot(0x1Fu, 0u) == 1, "secondary first");
        Check(PickRecoverySlot(0x1Du, 0u) == 0, "no secondary: primary");
        Check(PickRecoverySlot(0x03u, 0x02u) == 0, "secondary tried: primary next");
        Check(PickRecoverySlot(0x03u, 0x03u) == 1, "all tried: start over");
        Check(PickRecoverySlot(0u, 0u) == -1, "nothing at all");
        for (std::uint32_t bit = 1u; bit < kAll; bit <<= 1u) Check(Name(bit)[0] != '?', "every condition has a name");
    }
    if (g_Failures) { std::fprintf(stderr, "%d failure(s)\n", g_Failures); return 1; }
    std::puts("vr_control_watchdog_tests: all passed");
    return 0;
}

// OpenXR frame ticket ring (L4D2VR/vr_backend/openxr_frame_ring.h): the
// ordering rules and the call sequences the OpenXR backend drives through it,
// checked against a fake runtime that enforces the OpenXR frame rules.
#include "../L4D2VR/vr_backend/openxr_frame_ring.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int checks = 0;
static void Check(bool passed, const char* contract)
{
    ++checks;
    if (!passed)
    {
        std::fprintf(stderr, "FAIL: %s\n", contract);
        std::exit(1);
    }
}

using l4d2vr_openxr::FrameRing;
using l4d2vr_openxr::FrameTicket;
using l4d2vr_openxr::RingViolation;

// A runtime that tracks xrWaitFrame / xrBeginFrame / xrEndFrame the way the
// OpenXR spec defines them and records every rule break.
struct FakeRuntime
{
    int waited = 0;          // waited frames not yet begun (spec: at most 1)
    bool begun = false;      // a begun frame not yet ended
    int64_t nextDisplay = 1000;
    int ends = 0, discards = 0, errors = 0;
    std::vector<int64_t> endedTimes;

    bool Wait(int64_t& displayTime)
    {
        if (waited != 0) { ++errors; return false; }  // would block forever in a real runtime
        waited = 1;
        displayTime = nextDisplay;
        nextDisplay += 16;
        return true;
    }
    void Begin()
    {
        if (waited == 0) { ++errors; return; }  // XR_ERROR_CALL_ORDER_INVALID
        if (begun) ++discards;                 // XR_FRAME_DISCARDED
        waited = 0;
        begun = true;
    }
    void End(int64_t displayTime)
    {
        if (!begun) { ++errors; return; }
        begun = false;
        ++ends;
        endedTimes.push_back(displayTime);
    }
};

// The backend's policy (vr_backend_openxr.cpp) on top of the ring.
struct Driver
{
    FrameRing ring;
    FakeRuntime rt;

    // WaitGetPoses: close an unbegun frame first, then wait.
    bool WaitGetPoses()
    {
        switch (ring.PlanWait())
        {
        case FrameRing::WaitPlan::Busy:
            return false;
        case FrameRing::WaitPlan::EndStaleFirst:
        {
            FrameTicket t;
            if (!ring.BeginStale(&t)) return false;
            rt.Begin();
            ring.End(&t);
            rt.End(t.displayTime);
            break;
        }
        default:
            break;
        }
        if (!ring.BeginWait())
            return false;
        int64_t display = 0;
        const bool ok = rt.Wait(display);
        return ring.CompleteWait(ok, display, 16, true) != 0;
    }
    // SubmitExplicitTimingData / first Submit.
    void BeginForGame()
    {
        if (ring.Begin() == FrameRing::BeginResult::Begun)
            rt.Begin();
    }
    // PostPresentHandoff.
    void EndForGame()
    {
        if (!ring.HasBegun())
        {
            if (!ring.HasWaited())
                return;
            BeginForGame();
        }
        FrameTicket t;
        if (ring.End(&t))
            rt.End(t.displayTime);
    }
};

int main()
{
    // ---- Basic ordering ------------------------------------------------------
    {
        FrameRing ring;
        Check(ring.PlanWait() == FrameRing::WaitPlan::Wait, "an idle ring plans a plain wait");
        Check(ring.Begin() == FrameRing::BeginResult::NoFrame, "nothing to begin before a wait");
        Check(!ring.End(), "end without begin is refused");
        Check(ring.Stats().violations == 1 && ring.Stats().lastViolation == RingViolation::EndWithoutBegin,
            "end without begin is a counted violation");
        Check(ring.BeginWait(), "first wait is allowed");
        Check(ring.PlanWait() == FrameRing::WaitPlan::Busy, "a wait in flight makes others busy");
        Check(!ring.BeginWait() && ring.Stats().lastViolation == RingViolation::DoubleWait, "a second wait is refused");
        Check(ring.CompleteWait(true, 100, 16, true) == 1, "first ticket id is 1");
        Check(ring.HasWaited() && ring.Waited().displayTime == 100, "the waited ticket carries its display time");
        Check(ring.PlanWait() == FrameRing::WaitPlan::EndStaleFirst, "an unbegun frame must be closed before the next wait");
        Check(!ring.BeginWait() && ring.Stats().lastViolation == RingViolation::WaitOverWaited, "waiting over an unbegun frame is refused");
        FrameTicket t;
        Check(ring.Begin(&t) == FrameRing::BeginResult::Begun && t.id == 1, "begin takes the waited ticket");
        Check(ring.Begin(&t) == FrameRing::BeginResult::AlreadyBegun && t.id == 1, "a second begin of the same frame is a no-op");
        Check(ring.PlanWait() == FrameRing::WaitPlan::Wait, "a begun frame allows the next wait (pipelining)");
        Check(ring.End(&t) && t.id == 1, "end closes the begun frame");
        Check(!ring.HasBegun() && !ring.HasWaited(), "ring is idle after end");
    }

    // ---- Failed waits create no ticket ---------------------------------------
    {
        FrameRing ring;
        ring.BeginWait();
        Check(ring.CompleteWait(false, 0, 0, false) == 0, "failed wait returns no ticket");
        Check(!ring.HasWaited() && !ring.WaitInFlight() && ring.Stats().waitFailures == 1, "failed wait leaves the ring idle");
        Check(ring.CompleteWait(true, 1, 1, true) == 0 && ring.Stats().lastViolation == RingViolation::CompleteWithoutWait,
            "a result without a wait is a violation");
    }

    // ---- Discard: begin while an older frame is still begun ------------------
    {
        FrameRing ring;
        ring.BeginWait(); ring.CompleteWait(true, 100, 16, true);
        ring.Begin();
        ring.BeginWait(); ring.CompleteWait(true, 116, 16, true);
        FrameTicket t;
        Check(ring.Begin(&t) == FrameRing::BeginResult::Begun && t.id == 2, "the newer frame is begun");
        Check(ring.Stats().discarded == 1, "the older begun frame counts as discarded");
        Check(ring.End(&t) && t.id == 2 && ring.Stats().ends == 1, "end applies to the newer frame");
    }

    // ---- Reset forgets open frames, keeps ids increasing ---------------------
    {
        FrameRing ring;
        ring.BeginWait(); ring.CompleteWait(true, 100, 16, true);
        ring.Reset();
        Check(!ring.HasWaited() && ring.PlanWait() == FrameRing::WaitPlan::Wait, "reset drops the waited frame");
        ring.BeginWait();
        Check(ring.CompleteWait(true, 200, 16, true) == 2, "ticket ids keep increasing across a reset");
    }

    // ---- Main-thread (mat_queue_mode 0) loop: wait, begin, end ---------------
    {
        Driver d;
        for (int frame = 0; frame < 100; ++frame)
        {
            Check(d.WaitGetPoses(), "main thread wait succeeds");
            d.BeginForGame();   // SubmitExplicitTimingData
            d.BeginForGame();   // Submit(left) begins too; must be a no-op
            d.EndForGame();     // PostPresentHandoff
        }
        Check(d.rt.ends == 100 && d.rt.errors == 0 && d.rt.discards == 0, "every main-thread frame is ended once, no rule breaks");
        Check(d.ring.Stats().staleEnds == 0 && d.ring.Stats().violations == 0, "no stale ends in the steady main-thread loop");
        for (size_t i = 1; i < d.rt.endedTimes.size(); ++i)
            Check(d.rt.endedTimes[i] > d.rt.endedTimes[i - 1], "frames end in display-time order");
    }

    // ---- Menus / skipped submits: the next wait closes the frame -------------
    {
        Driver d;
        for (int frame = 0; frame < 50; ++frame)
        {
            Check(d.WaitGetPoses(), "menu wait succeeds");
            if (frame % 3 == 0)
            {
                d.BeginForGame();
                d.EndForGame();
            }
        }
        Check(d.rt.errors == 0 && d.rt.discards == 0, "skipped submits never break the frame rules");
        Check(d.rt.ends == 49, "every waited frame but the open last one is ended (game or stale)");
        Check(d.ring.Stats().staleEnds == 49 - 17, "frames the game skipped are closed by the wait thread");
    }

    // ---- Loading stall: the pump keeps whole frames flowing ------------------
    {
        Driver d;
        Check(d.WaitGetPoses(), "game waited a frame before the map load");
        // The game thread stalls; the loading pump calls WaitGetPoses at HMD rate.
        for (int pumped = 0; pumped < 200; ++pumped)
            Check(d.WaitGetPoses(), "pumped wait succeeds");
        Check(d.rt.ends == 200 && d.rt.errors == 0, "each pumped wait ends the previous frame (200 whole frames)");
        // The game resumes: its own frame on the last pumped ticket.
        d.BeginForGame();
        d.EndForGame();
        Check(d.rt.ends == 201 && d.rt.errors == 0, "the resumed game frame ends cleanly");
        Check(d.ring.Stats().violations == 0, "no ring violations across the stall");
    }

    // ---- Pose waiter (queued mode): game submits while a wait is in flight ---
    {
        Driver d;
        Check(d.WaitGetPoses(), "pose waiter first wait");
        for (int frame = 0; frame < 60; ++frame)
        {
            // The waiter loops: it closes the unbegun frame and starts the next wait.
            Check(d.ring.PlanWait() == FrameRing::WaitPlan::EndStaleFirst, "the waiter finds its frame unbegun");
            FrameTicket t;
            d.ring.BeginStale(&t); d.rt.Begin(); d.ring.End(&t); d.rt.End(t.displayTime);
            d.ring.BeginWait();
            // While xrWaitFrame blocks, the game submits: nothing to begin yet.
            Check(d.ring.Begin() == FrameRing::BeginResult::NoFrame, "no frame to begin during the wait");
            d.EndForGame();  // PostPresentHandoff finds nothing: the eyes ride the next stale end
            Check(d.ring.PlanWait() == FrameRing::WaitPlan::Busy, "a second waiter would join, not wait");
            int64_t display = 0;
            d.rt.Wait(display);
            d.ring.CompleteWait(true, display, 16, true);
            if (frame % 2 == 0)
            {
                // Sometimes the game's handoff lands right after the wait returns.
                d.BeginForGame();
                d.EndForGame();
                Check(d.WaitGetPoses(), "waiter waits again after the game ended the frame");
            }
        }
        Check(d.rt.errors == 0 && d.ring.Stats().violations == 0, "queued interleavings never break the frame rules");
    }

    // ---- A game frame aborted after begin (left eye failed) ------------------
    {
        Driver d;
        d.WaitGetPoses();
        d.BeginForGame();           // begun, then the submit path bails out
        Check(d.WaitGetPoses(), "the next wait is allowed with a begun frame (pipelining)");
        d.BeginForGame();           // begins the new frame, discarding the abandoned one
        d.EndForGame();
        Check(d.rt.discards == 1 && d.rt.errors == 0 && d.ring.Stats().discarded == 1,
            "an abandoned begun frame is discarded by the next begin, never ended twice");
    }

    // ---- The assert hook sees every violation ---------------------------------
    {
        struct Seen { int count = 0; RingViolation last = RingViolation::None; } seen;
        FrameRing ring;
        ring.SetAssertHook([](RingViolation v, void* ctx) {
            auto* s = static_cast<Seen*>(ctx);
            ++s->count;
            s->last = v;
        }, &seen);
        ring.End();
        ring.BeginStale();
        Check(seen.count == 2 && seen.last == RingViolation::BeginWithoutWait, "assert hook reports violations");
        Check(std::string(l4d2vr_openxr::Name(RingViolation::DoubleWait)) == "double xrWaitFrame", "violation names");
    }

    std::printf("vr_openxr_frame_ring: %d checks passed\n", checks);
    return 0;
}

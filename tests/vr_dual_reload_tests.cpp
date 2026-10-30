#include "../L4D2VR/vr_dual_reload.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Failed at line %d\n", __LINE__); std::abort(); } } while (false)

using namespace l4d2vr_dual_reload;

namespace
{
    // Two hands holding pistols forward, 40 cm apart.
    Input Base(std::uint64_t now)
    {
        Input in{};
        in.nowMs = now;
        in.eligible = true;
        in.canReload = true;
        for (int h = 0; h < 2; ++h)
        {
            auto& hand = in.hands[static_cast<std::size_t>(h)];
            hand.valid = true;
            hand.position = {h == 0 ? 0.2f : -0.2f, 0.0f, 1.2f};
            hand.forward = {0.0f, 1.0f, 0.0f};
            hand.up = {0.0f, 0.0f, 1.0f};
        }
        in.pouchValid = true;
        in.pouch = {-0.25f, 0.0f, 0.9f};
        return in;
    }

    struct Rig
    {
        Config config{};
        Machine machine{};
        std::uint64_t now = 1000u;
        Input in = Base(1000u);
        unsigned Step(unsigned ms = 11u)
        {
            now += ms; in.nowMs = now;
            const unsigned events = machine.Update(config, in);
            in.release = false;
            return events;
        }
        // Moves hand `h` to `to` over `steps` frames; returns the OR of the events.
        unsigned Move(int h, V3 to, int steps = 20)
        {
            unsigned events = 0u;
            const V3 from = in.hands[static_cast<std::size_t>(h)].position;
            for (int i = 1; i <= steps; ++i)
            {
                const float u = static_cast<float>(i) / static_cast<float>(steps);
                in.hands[static_cast<std::size_t>(h)].position = Add(from, Scale(Sub(to, from), u));
                events |= Step();
            }
            return events;
        }
        unsigned Hold(int frames)
        {
            unsigned events = 0u;
            for (int i = 0; i < frames; ++i) events |= Step();
            return events;
        }
    };

    // Full sequence helper up to the stacked stage; `moving` moves into the other hand.
    void EjectAndStack(Rig& rig, int moving)
    {
        rig.in.release = true;
        CHECK((rig.Step() & kEventEjected) != 0u);
        CHECK(rig.machine.GetStage() == Stage::Ejected);
        CHECK(rig.machine.BlocksFire());
        CHECK(rig.machine.HideMagazines());
        const int receiving = 1 - moving;
        V3 target = rig.in.hands[static_cast<std::size_t>(receiving)].position;
        target.x += receiving == 0 ? -0.07f : 0.07f;
        const unsigned events = rig.Move(moving, target) | rig.Hold(10);
        CHECK((events & kEventStacked) != 0u);
        CHECK(rig.machine.GetStage() == Stage::Stacked);
        CHECK(rig.machine.Receiving() == receiving);
        CHECK(rig.machine.FreeHand() == moving);
        CHECK(rig.machine.Role(receiving) == HandRole::StackedPistols);
        CHECK(rig.machine.Role(moving) == HandRole::Free);
    }

    void GrabAndSeat(Rig& rig)
    {
        const int f = rig.machine.FreeHand();
        auto& free = rig.in.hands[static_cast<std::size_t>(f)];
        rig.Move(f, rig.in.pouch);
        free.grab = true;
        CHECK((rig.Step() & kEventGrabbedMags) != 0u);
        CHECK(rig.machine.HoldingMagazines());
        CHECK(rig.machine.Role(f) == HandRole::StackedMagazines);
        // The wells: 10 cm below the receiving hand; the held magazines follow the free hand.
        const V3 receivingPos = rig.in.hands[static_cast<std::size_t>(rig.machine.Receiving())].position;
        rig.in.seatValid = true;
        rig.in.seat = Add(receivingPos, V3{0.0f, 0.0f, -0.04f});
        rig.in.holdAngleDeg = 10.0f;
        unsigned events = 0u;
        const V3 start = free.position, end = Add(receivingPos, V3{0.0f, 0.0f, -0.13f});
        for (int i = 1; i <= 30 && !(events & kEventSeated); ++i)
        {
            free.position = Add(start, Scale(Sub(end, start), static_cast<float>(i) / 30.0f));
            rig.in.held = Add(free.position, V3{0.0f, 0.0f, 0.09f});
            events |= rig.Step();
        }
        CHECK((events & kEventSeated) != 0u);
        CHECK(rig.machine.GetStage() == Stage::Committing);
        CHECK(!rig.machine.HideMagazines());
    }

    void Rack(Rig& rig, int gun)
    {
        const int f = rig.machine.FreeHand();
        auto& free = rig.in.hands[static_cast<std::size_t>(f)];
        rig.in.slideValid = {true, true};
        const V3 base = rig.in.hands[static_cast<std::size_t>(rig.machine.Receiving())].position;
        rig.in.slide = {Add(base, V3{0.02f, -0.02f, 0.03f}), Add(base, V3{-0.02f, -0.02f, 0.03f})};
        rig.in.slideBack = {V3{0.0f, -1.0f, 0.0f}, V3{0.0f, -1.0f, 0.0f}};
        free.grab = false; rig.Step();
        rig.Move(f, rig.in.slide[static_cast<std::size_t>(gun)], 10);
        free.grab = true;
        CHECK((rig.Step() & kEventSlideGrabbed) != 0u);
        CHECK(rig.machine.SlideHeld() == gun);
        CHECK(rig.machine.Role(f) == HandRole::Slide);
        const unsigned events = rig.Move(f, Add(free.position, V3{0.0f, -0.03f, 0.0f}), 8);
        CHECK((events & kEventSlideRacked) != 0u);
        CHECK(!rig.machine.NeedsRack(gun));
        CHECK(rig.machine.SlidePullM() > 0.024f && rig.machine.SlidePullM() <= rig.config.slideTravelM + 1e-6f);
        free.grab = false;
        CHECK((rig.Step() & kEventSlideReleased) != 0u);
        CHECK(rig.machine.SlidePullM() == 0.0f);
    }

    void TakeBack(Rig& rig)
    {
        const int f = rig.machine.FreeHand();
        auto& free = rig.in.hands[static_cast<std::size_t>(f)];
        const int r = rig.machine.Receiving();
        V3 target = rig.in.hands[static_cast<std::size_t>(r)].position;
        target.x += r == 0 ? -0.07f : 0.07f;
        rig.Move(f, target, 10);
        free.grip = true;
        CHECK((rig.Step() & kEventDone) != 0u);
        CHECK(rig.machine.GetStage() == Stage::Idle);
        CHECK(!rig.machine.BlocksFire());
        free.grip = false; rig.Step();
    }
}

int main()
{
    // ---- Ammunition: L4D2's native dual reload ---------------------------------------------------
    CHECK(DualCapacity(15) == 30);
    CHECK(DualCapacity(30) == 30);
    CHECK(DualCapacity(0) == 0);
    AmmoResult ammo{};
    CHECK(NativeReload(0, 0, true, 30, ammo) && ammo.clip == 30 && ammo.reserve == 0 && ammo.added == 30);
    CHECK(NativeReload(7, 120, true, 30, ammo) && ammo.clip == 30 && ammo.reserve == 120 && ammo.added == 23);
    CHECK(NativeReload(0, 20, false, 30, ammo) && ammo.clip == 20 && ammo.reserve == 0); // less than two magazines
    CHECK(NativeReload(10, 12, false, 30, ammo) && ammo.clip == 22 && ammo.reserve == 0);
    CHECK(NativeReload(4, 200, false, 30, ammo) && ammo.clip == 30 && ammo.reserve == 174);
    CHECK(NativeReload(30, 200, false, 30, ammo) && ammo.added == 0);
    CHECK(!NativeReload(31, 0, false, 30, ammo));
    CHECK(!NativeReload(-1, 0, false, 30, ammo));
    CHECK(!NativeReload(0, -1, false, 30, ammo));
    int right = -1, left = -1;
    Split(30, right, left); CHECK(right == 15 && left == 15);
    Split(1, right, left); CHECK(right == 0 && left == 1);
    Split(21, right, left); CHECK(right == 10 && left == 11);
    Split(-4, right, left); CHECK(right == 0 && left == 0);

    // ---- Empty guns, left hand into the right hand, both slides -----------------------------------
    {
        Rig rig;
        rig.in.roundsRight = 0; rig.in.roundsLeft = 0;
        CHECK(!rig.machine.BlocksFire());
        CHECK(rig.machine.Role(0) == HandRole::None);
        EjectAndStack(rig, 1);
        GrabAndSeat(rig);
        // Rounds in: both guns were empty, both need their slides.
        CHECK(rig.machine.Commit(15, 15) == kEventCommitted);
        CHECK(rig.machine.GetStage() == Stage::Racking);
        CHECK(rig.machine.NeedsRack(0) && rig.machine.NeedsRack(1));
        CHECK(rig.machine.Commit(15, 15) == 0u); // only once
        // The grab still held from seating does not take a slide: a new press does.
        rig.in.hands[1].grab = true; rig.Step();
        CHECK(rig.machine.SlideHeld() < 0);
        Rack(rig, 0);
        CHECK(rig.machine.GetStage() == Stage::Racking);
        CHECK(rig.machine.NeedsRack(1));
        Rack(rig, 1);
        CHECK(rig.machine.GetStage() == Stage::Ready);
        CHECK(rig.machine.BlocksFire());
        TakeBack(rig);
    }

    // ---- Right hand into the left hand -----------------------------------------------------------
    {
        Rig rig;
        EjectAndStack(rig, 0);
        GrabAndSeat(rig);
        rig.machine.Commit(15, 15);
        Rack(rig, 1);
        Rack(rig, 0);
        TakeBack(rig);
    }

    // ---- Tactical reload: both chambered, no slides -----------------------------------------------
    {
        Rig rig;
        rig.in.roundsRight = 3; rig.in.roundsLeft = 4;
        EjectAndStack(rig, 1);
        CHECK(rig.machine.ChamberedBefore(0) && rig.machine.ChamberedBefore(1));
        GrabAndSeat(rig);
        rig.machine.Commit(15, 15);
        CHECK(rig.machine.GetStage() == Stage::Ready);
        CHECK(!rig.machine.NeedsRack(0) && !rig.machine.NeedsRack(1));
        TakeBack(rig);
    }

    // ---- One gun dry (shared clip of 1: right 0, left 1): only the right slide ----------------------
    {
        Rig rig;
        Split(1, rig.in.roundsRight, rig.in.roundsLeft);
        EjectAndStack(rig, 1);
        GrabAndSeat(rig);
        rig.machine.Commit(15, 15);
        CHECK(rig.machine.NeedsRack(0) && !rig.machine.NeedsRack(1));
        // A pull at the chambered gun's slide does nothing.
        rig.in.slideValid = {true, true};
        const V3 base = rig.in.hands[0].position;
        rig.in.slide = {Add(base, V3{0.3f, 0.0f, 0.0f}), Add(base, V3{-0.02f, 0.0f, 0.03f})};
        rig.in.slideBack = {V3{0.0f, -1.0f, 0.0f}, V3{0.0f, -1.0f, 0.0f}};
        rig.in.hands[1].grab = false; rig.Step();
        rig.Move(1, rig.in.slide[1], 8);
        rig.in.hands[1].grab = true;
        CHECK((rig.Step() & kEventSlideGrabbed) == 0u);
        rig.in.hands[1].grab = false; rig.Step();
        Rack(rig, 0);
        CHECK(rig.machine.GetStage() == Stage::Ready);
        TakeBack(rig);
    }

    // ---- Partial fill from a small reserve: an empty gun that gets no rounds needs no slide ----------
    {
        Rig rig;
        EjectAndStack(rig, 1);
        GrabAndSeat(rig);
        int r = 0, l = 0; Split(1, r, l);       // a reserve of one round: NativeReload(0, 1) = 1
        rig.machine.Commit(r, l);
        CHECK(!rig.machine.NeedsRack(0) && rig.machine.NeedsRack(1));
    }

    // ---- Nothing to reload with: release refused ---------------------------------------------------
    {
        Rig rig;
        rig.in.canReload = false;
        rig.in.release = true;
        CHECK((rig.Step() & kEventRefused) != 0u);
        CHECK(rig.machine.GetStage() == Stage::Idle);
    }

    // ---- Stacking needs the guns roughly parallel and a short dwell -------------------------------
    {
        Rig rig;
        rig.in.release = true; rig.Step();
        rig.in.hands[1].forward = {1.0f, 0.0f, 0.0f}; // 90 degrees off
        unsigned events = rig.Move(1, Add(rig.in.hands[0].position, V3{-0.07f, 0.0f, 0.0f})) | rig.Hold(10);
        CHECK((events & kEventStacked) == 0u);
        CHECK((events & kEventContact) != 0u);
        CHECK(rig.machine.GetStage() == Stage::Ejected);
        rig.in.hands[1].forward = {0.3f, 1.0f, 0.0f}; // about 17 degrees: fine
        events = rig.Hold(1);
        CHECK((events & kEventStacked) == 0u);    // dwell
        events = rig.Hold(10);
        CHECK((events & kEventStacked) != 0u);
    }

    // ---- Cancels -----------------------------------------------------------------------------------
    {
        // Weapon switch while stacked.
        Rig rig;
        EjectAndStack(rig, 1);
        rig.in.eligible = false;
        CHECK((rig.Step() & kEventCancelled) != 0u);
        CHECK(rig.machine.GetStage() == Stage::Idle);
        CHECK(!rig.machine.BlocksFire());
        rig.in.eligible = true;
        CHECK(rig.Step() == 0u);
    }
    {
        // Letting go of the double magazine away from the guns drops it; the pouch has more.
        Rig rig;
        EjectAndStack(rig, 1);
        rig.Move(1, rig.in.pouch);
        rig.in.hands[1].grab = true;
        CHECK((rig.Step() & kEventGrabbedMags) != 0u);
        rig.in.hands[1].grab = false;
        CHECK((rig.Step() & kEventDroppedMags) != 0u);
        CHECK(rig.machine.GetStage() == Stage::Stacked);
        rig.in.hands[1].grab = true;
        CHECK((rig.Step() & kEventGrabbedMags) != 0u);
    }
    {
        // Taking the gun back before the magazines went in: akimbo, magazines still out,
        // and the hands must part before they stack again.
        Rig rig;
        EjectAndStack(rig, 1);
        rig.in.hands[1].grip = true;
        CHECK((rig.Step() & kEventUnstacked) != 0u);
        CHECK(rig.machine.GetStage() == Stage::Ejected);
        CHECK(rig.machine.HideMagazines() && rig.machine.BlocksFire());
        rig.in.hands[1].grip = false;
        CHECK((rig.Hold(20) & kEventStacked) == 0u);
        rig.Move(1, V3{-0.3f, 0.0f, 1.2f});
        rig.Move(1, Add(rig.in.hands[0].position, V3{-0.07f, 0.0f, 0.0f}));
        CHECK((rig.Hold(10) | rig.Step()) != 0u || rig.machine.GetStage() == Stage::Stacked);
        CHECK(rig.machine.GetStage() == Stage::Stacked);
    }
    {
        // A grip press far from the stacked guns does not take a gun back, and the
        // pouch is only reached by the free hand.
        Rig rig;
        EjectAndStack(rig, 1);
        rig.Move(1, V3{-0.6f, 0.0f, 1.2f});
        rig.in.hands[1].grip = true;
        CHECK((rig.Step() & kEventUnstacked) == 0u);
        rig.in.hands[1].grip = false; rig.Step();
        rig.in.hands[0].grab = true; // the receiving hand's trigger near nothing
        CHECK((rig.Step() & kEventGrabbedMags) == 0u);
        CHECK(rig.machine.GetStage() == Stage::Stacked);
    }
    {
        // Magazines held at the wells but turned too far do not seat.
        Rig rig;
        EjectAndStack(rig, 1);
        rig.Move(1, rig.in.pouch);
        rig.in.hands[1].grab = true; rig.Step();
        rig.in.seatValid = true;
        rig.in.seat = V3{0.2f, 0.0f, 1.16f};
        rig.in.held = rig.in.seat;
        rig.in.holdAngleDeg = 80.0f;
        CHECK((rig.Hold(5) & kEventSeated) == 0u);
        rig.in.holdAngleDeg = 20.0f;
        CHECK((rig.Step() & kEventSeated) != 0u);
    }

    // ---- Host ledger --------------------------------------------------------------------------------
    {
        ServerLedger ledger; ledger.Reset(77u);
        Snapshot snap{true, 0x00103u, 500u, 0, 0, 30, true};
        int written = -1, writtenReserve = -1;
        auto writer = [&](int clip, int reserve) { written = clip; writtenReserve = reserve; return true; };
        Request request{77u, 1u, 0x00103u, 498u, 0, 0};
        Reply reply = ledger.Apply(request, snap, 1000u, writer);
        CHECK(reply.status == Status::Applied && reply.clip == 30 && reply.reserve == 0 && written == 30);
        // Exact repeat: the memo, no second write.
        written = -1;
        reply = ledger.Apply(request, snap, 1001u, writer);
        CHECK(reply.status == Status::Applied && written == -1);
        // Older sequence.
        CHECK(ledger.Apply(Request{77u, 1u, 0x00103u, 499u, 0, 0}, snap, 1002u, writer).status == Status::Stale);
        // Wrong token / stale counts / wrong gun / full / rate limit.
        CHECK(ledger.Apply(Request{78u, 2u, 0x00103u, 499u, 0, 0}, snap, 1300u, writer).status == Status::Session);
        CHECK(ledger.Apply(Request{77u, 3u, 0x00103u, 499u, 5, 0}, snap, 1300u, writer).status == Status::Stale);
        CHECK(ledger.Apply(Request{77u, 4u, 0x00104u, 499u, 0, 0}, snap, 1300u, writer).status == Status::Weapon);
        Snapshot full = snap; full.clip = 30;
        CHECK(ledger.Apply(Request{77u, 5u, 0x00103u, 499u, 30, 0}, full, 1300u, writer).status == Status::NoSpaceOrAmmo);
        CHECK(ledger.Apply(Request{77u, 6u, 0x00103u, 499u, 0, 0}, snap, 1050u, writer).status == Status::RateLimited);
        Snapshot single = snap; single.eligible = false;
        CHECK(ledger.Apply(Request{77u, 7u, 0x00103u, 499u, 0, 0}, single, 1400u, writer).status == Status::Weapon);
        // Too old a command, or one from the future.
        CHECK(ledger.Apply(Request{77u, 8u, 0x00103u, 400u, 0, 0}, snap, 1400u, writer).status == Status::Stale);
        CHECK(ledger.Apply(Request{77u, 9u, 0x00103u, 520u, 0, 0}, snap, 1400u, writer).status == Status::Stale);
        // A finite reserve smaller than two magazines: partial fill, native numbers.
        Snapshot finite{true, 0x00103u, 600u, 2, 12, 30, false};
        reply = ledger.Apply(Request{77u, 10u, 0x00103u, 600u, 2, 12}, finite, 2000u, writer);
        CHECK(reply.status == Status::Applied && reply.clip == 14 && reply.reserve == 0);
        // The native write failing.
        CHECK(ledger.Apply(Request{77u, 11u, 0x00103u, 499u, 0, 0}, snap, 3000u,
            [](int, int) { return false; }).status == Status::Backend);
        CHECK(ledger.Unsupported(Request{77u, 12u, 0x00103u, 600u, 0, 0}).status == Status::Unsupported);
    }

    // ---- Client request -----------------------------------------------------------------------------
    {
        ClientRequest client;
        Request out{};
        CHECK(!client.Begin(0x103u, 0, 0, 10u, 1u, 0u, out));
        client.Offer(55u);
        CHECK(client.Supported());
        CHECK(client.Begin(0x103u, 0, 0, 10u, 0x1234u, 100u, out));
        CHECK(out.token == 55u && out.sequence == 1u && client.Pending());
        CHECK(!client.Begin(0x103u, 0, 0, 10u, 0x1234u, 100u, out)); // one at a time
        Reply reply{};
        CHECK(client.Update(0x103u, 0x1234u, 150u, 0, 0, reply) == ClientRequest::Poll::Waiting);
        Request retry{};
        CHECK(!client.NextRetry(300u, retry));
        CHECK(client.NextRetry(650u, retry) && retry == out);
        // A bogus applied reply (fewer rounds) is ignored.
        client.Receive(Reply{out, Status::Applied, 0, 0});
        CHECK(client.Update(0x103u, 0x1234u, 700u, 0, 0, reply) == ClientRequest::Poll::Waiting);
        client.Receive(Reply{out, Status::Applied, 30, 0});
        // Waits for the replicated clip.
        CHECK(client.Update(0x103u, 0x1234u, 700u, 0, 0, reply) == ClientRequest::Poll::Waiting);
        CHECK(client.Update(0x103u, 0x1234u, 720u, 30, 0, reply) == ClientRequest::Poll::Result);
        CHECK(reply.status == Status::Applied && reply.clip == 30 && !client.Pending());
        // Timeout.
        CHECK(client.Begin(0x103u, 3, 10, 20u, 0x1234u, 1000u, out));
        CHECK(client.Update(0x103u, 0x1234u, 3600u, 3, 10, reply) == ClientRequest::Poll::Timeout);
        CHECK(!client.Pending() && client.Supported());
        // A switched gun cancels quietly.
        CHECK(client.Begin(0x103u, 3, 10, 21u, 0x1234u, 4000u, out));
        CHECK(client.Update(0x104u, 0x1234u, 4010u, 3, 10, reply) == ClientRequest::Poll::None);
        CHECK(!client.Pending());
        // Rejections come straight back.
        CHECK(client.Begin(0x103u, 3, 10, 22u, 0x1234u, 5000u, out));
        client.Receive(Reply{out, Status::Stale, 3, 10});
        CHECK(client.Update(0x103u, 0x1234u, 5010u, 3, 10, reply) == ClientRequest::Poll::Result);
        CHECK(reply.status == Status::Stale);
        client.Offer(56u);
        CHECK(client.Begin(0x103u, 3, 10, 23u, 0x1234u, 6000u, out) && out.token == 56u && out.sequence == 1u);
    }

    // ---- Wire format ------------------------------------------------------------------------------
    {
        char line[160];
        const Request request{1234567u, 9u, 0x00103u, 4567u, 3, 120};
        CHECK(FormatRequest(request, line, sizeof(line)));
        CHECK(std::strcmp(line, "l4d2vr_mag_dual_v1 1234567 9 259 4567 3 120\n") == 0);
        const char* argv[] = {"l4d2vr_mag_dual_v1", "1234567", "9", "259", "4567", "3", "120"};
        Request parsed{};
        CHECK(ParseRequest(7, argv, parsed) && parsed == request);
        const char* bad[] = {"l4d2vr_mag_dual_v1", "1234567", "9", "259", "4567", "65", "120"};
        CHECK(!ParseRequest(7, bad, parsed));
        CHECK(!ParseRequest(6, argv, parsed));
        const Reply reply{request, Status::Applied, 30, 93};
        CHECK(FormatReply(reply, line, sizeof(line)));
        CHECK(std::strcmp(line, "l4d2vr_mag_dual_result 1234567 9 259 4567 3 120 0 30 93\n") == 0);
        const char* rargv[] = {"l4d2vr_mag_dual_result", "1234567", "9", "259", "4567", "3", "120", "0", "30", "93"};
        Reply back{};
        CHECK(ParseReply(10, rargv, back) && back.request == request && back.status == Status::Applied &&
            back.clip == 30 && back.reserve == 93);
        const char* rbad[] = {"l4d2vr_mag_dual_result", "1234567", "9", "259", "4567", "3", "120", "8", "30", "93"};
        CHECK(!ParseReply(10, rbad, back));
    }

    std::puts("vr_dual_reload_tests: all passed");
    return 0;
}

#pragma once
// Physical dual-pistol reload (std-only, unit tested in tests/vr_dual_reload_tests.cpp).
//
// The player's sequence (gameplay hands: 0 = right, the primary/viewmodel gun;
// 1 = left, the second gun):
//   1. Magazine release: both empty magazines drop out (Ejected).
//   2. One pistol hand is brought to the other, guns roughly parallel. When the
//      hands meet the moving hand's pistol stacks beside the other one in the
//      receiving hand, and the moving hand is free (Stacked).
//   3. The free hand takes two magazines stuck together from the pouch
//      (HoldingMags) and pushes them into both magazine wells at once (seated:
//      the rounds are committed, Committing).
//   4. A gun that was empty before (nothing chambered) needs its slide pulled
//      back by the free hand, one gun at a time (Racking). A gun with a round
//      chambered (tactical reload) needs none.
//   5. The free hand's grip at the stacked guns takes its pistol back: normal
//      akimbo again (Ready -> Idle).
// Fire is blocked from the release until the guns are back in their hands.
//
// Ammunition matches L4D2's own dual reload: the two guns share one clip
// (m_iClip1, 2 x 15 rounds) and one reserve, and the reload adds
// min(capacity - clip, reserve) rounds (infinite reserve: a full 30). The
// rounds are committed once, when the magazines seat: by a host transaction
// (l4d2vr_mag_dual_v1, feature DualMagTxn, on the mod's listen host and the
// dedicated plugin) or, on any other server, by the stock reload command.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include "vr_remote_shells.h"

namespace l4d2vr_dual_reload
{
    using Status = l4d2vr_shell::Status;
    constexpr unsigned kVersion = 1u;
    constexpr int kGunCapacity = 15;
    constexpr int kMaxCapacity = 64;
    constexpr int kMaxReserve = 5000;
    // How far the request's command may be from the newest command the host
    // decoded, either way (a stalled host or a long frame queue).
    constexpr std::int64_t kCommandWindow = 64;

    // ---- Vectors ----------------------------------------------------------------------------------
    struct V3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
    inline V3 Add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    inline V3 Sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    inline V3 Scale(const V3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    inline float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline float Length(const V3& a) { return std::sqrt(Dot(a, a)); }
    inline float Distance(const V3& a, const V3& b) { return Length(Sub(a, b)); }
    inline bool Finite(const V3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
    inline V3 Normalize(const V3& a)
    {
        const float length = Length(a);
        return length > 1e-6f && std::isfinite(length) ? Scale(a, 1.0f / length) : V3{};
    }
    // Angle between two directions in degrees (180 when either is degenerate).
    inline float AngleDeg(const V3& a, const V3& b)
    {
        const V3 na = Normalize(a), nb = Normalize(b);
        if (Length(na) < 0.5f || Length(nb) < 0.5f) return 180.0f;
        const float c = std::fmax(-1.0f, std::fmin(1.0f, Dot(na, nb)));
        return std::acos(c) * 57.29577951308232f;
    }

    // ---- Ammunition (L4D2's native dual reload) ----------------------------------------------------
    // CPistol reports one gun's clip size; dual pistols hold two of them in m_iClip1.
    inline int DualCapacity(int maxClip1)
    {
        if (maxClip1 <= 0) return 0;
        return maxClip1 <= kGunCapacity ? maxClip1 * 2 : std::min(maxClip1, kMaxCapacity);
    }
    struct AmmoResult { int clip = 0, reserve = 0, added = 0; };
    // CTerrorGun's reload: the clip keeps its rounds and takes min(room, reserve)
    // from the reserve (an infinite reserve is not reduced).
    inline bool NativeReload(int clip, int reserve, bool infinite, int capacity, AmmoResult& out)
    {
        if (capacity <= 0 || capacity > kMaxCapacity || clip < 0 || clip > capacity ||
            reserve < 0 || reserve > kMaxReserve) return false;
        const int room = capacity - clip;
        const int added = infinite ? room : std::min(room, reserve);
        out = {clip + added, infinite ? reserve : reserve - added, added};
        return true;
    }
    // The shared clip split over the two guns as the pistol ledger does it
    // (l4d2vr_pistol::AmmoLedger::Observe): right = clip / 2, left the rest.
    inline void Split(int clip, int& right, int& left)
    {
        clip = std::max(0, clip);
        right = clip / 2; left = clip - right;
    }

    // ---- Hand roles: named pose targets for the hand-pose library ---------------------------------
    // What each gameplay hand does during the sequence. The finger curls below
    // are minimum curls (thumb..index..pinky, 0 open .. 1 fist) for the hand
    // pose code; refine them there (agent/hand-poses).
    enum class HandRole : int
    {
        None,               // not part of a dual reload
        AkimboPistol,       // still holding its own pistol (mags out, not stacked)
        StackedPistols,     // the receiving hand: both pistols side by side in one fist
        Free,               // the free hand, empty
        StackedMagazines,   // the free hand holding two magazines stuck together in one fist
        Slide,              // the free hand pulling one of the slides back
    };
    constexpr std::array<float, 5> kStackedPistolsMinCurl = {0.55f, 0.70f, 0.78f, 0.80f, 0.80f};
    constexpr std::array<float, 5> kStackedMagazinesMinCurl = {0.30f, 0.62f, 0.70f, 0.72f, 0.72f};
    constexpr std::array<float, 5> kSlideMinCurl = {0.45f, 0.55f, 0.70f, 0.75f, 0.78f};

    // ---- The sequence ------------------------------------------------------------------------------
    enum class Stage : unsigned
    {
        Idle = 0, Ejected = 1, Stacked = 2, HoldingMags = 3, Committing = 4, Racking = 5, Ready = 6
    };
    inline const char* StageName(Stage stage)
    {
        switch (stage)
        {
        case Stage::Idle: return "idle";
        case Stage::Ejected: return "ejected";
        case Stage::Stacked: return "stacked";
        case Stage::HoldingMags: return "holding-mags";
        case Stage::Committing: return "committing";
        case Stage::Racking: return "racking";
        case Stage::Ready: return "ready";
        }
        return "?";
    }
    enum Event : unsigned
    {
        kEventEjected = 1u << 0,      // both magazines out: drop them, clip-out sound, haptics
        kEventStacked = 1u << 1,      // the moving gun is in the receiving hand
        kEventUnstacked = 1u << 2,    // the free hand took its gun back before the magazines went in
        kEventGrabbedMags = 1u << 3,  // the free hand took the double magazine from the pouch
        kEventDroppedMags = 1u << 4,  // let go of the double magazine away from the guns
        kEventSeated = 1u << 5,       // both magazines went in: commit the rounds now
        kEventCommitted = 1u << 6,    // the rounds are in (Commit)
        kEventSlideGrabbed = 1u << 7,
        kEventSlideRacked = 1u << 8,  // one slide went back far enough: that gun is chambered
        kEventSlideReleased = 1u << 9,
        kEventDone = 1u << 10,        // the guns are back in their own hands
        kEventCancelled = 1u << 11,   // the sequence ended early (weapon switch, death, menu ...)
        kEventRefused = 1u << 12,     // release pressed with nothing to reload with
        kEventContact = 1u << 13,     // the hands came within reach of each other (light tick)
    };

    struct Config
    {
        float stackDistanceM = 0.12f;      // controller to controller
        float stackMaxAngleDeg = 45.0f;    // between the two barrels
        float stackMaxRollDeg = 70.0f;     // between the two guns' up axes
        std::uint32_t stackDwellMs = 60u;
        float pouchReachM = 0.16f;
        float insertRadiusM = 0.05f;       // held magazines to the wells
        float insertMaxAngleDeg = 40.0f;   // free hand to receiving hand
        float slideReachM = 0.07f;
        float slideTravelM = 0.035f;
        float slideRackM = 0.025f;
        float takeBackReachM = 0.16f;
    };

    struct HandSample
    {
        bool valid = false;
        V3 position{}, forward{}, up{};
        bool grab = false;   // the reload-interaction input (trigger with trigger interactions, else grip)
        bool grip = false;   // the grip (takes the gun back)
    };

    struct Input
    {
        std::uint64_t nowMs = 0u;
        bool eligible = false;        // dual pistols in both hands, alive, VR input live
        bool release = false;         // magazine release pressed this frame
        bool canReload = false;       // the reserve has rounds (or is infinite)
        int roundsRight = 0, roundsLeft = 0;  // per gun before the reload (chamber state)
        std::array<HandSample, 2> hands{};
        bool pouchValid = false;
        V3 pouch{};
        bool seatValid = false;
        V3 seat{};                    // where the double magazine seats (both wells)
        V3 held{};                    // the held double magazine, same reference point
        float holdAngleDeg = 180.0f;  // free hand's rotation to the receiving hand's
        std::array<bool, 2> slideValid{};
        std::array<V3, 2> slide{};    // where each slide is taken (world)
        std::array<V3, 2> slideBack{};// unit direction a slide moves when pulled
    };

    class Machine
    {
    public:
        void Reset()
        {
            const auto prevGrab = m_PrevGrab, prevGrip = m_PrevGrip;
            *this = Machine{};
            m_PrevGrab = prevGrab; m_PrevGrip = prevGrip;
        }
        Stage GetStage() const { return m_Stage; }
        bool Active() const { return m_Stage != Stage::Idle; }
        bool BlocksFire() const { return m_Stage != Stage::Idle; }
        bool Stacked() const { return m_Stage >= Stage::Stacked; }
        int Receiving() const { return Stacked() ? m_Receiving : -1; }
        int FreeHand() const { return Stacked() ? 1 - m_Receiving : -1; }
        bool HideMagazines() const { return m_Stage >= Stage::Ejected && m_Stage <= Stage::HoldingMags; }
        bool HoldingMagazines() const { return m_Stage == Stage::HoldingMags; }
        bool NeedsRack(int gun) const { return gun >= 0 && gun < 2 && m_NeedsRack[static_cast<std::size_t>(gun)]; }
        bool ChamberedBefore(int gun) const { return gun >= 0 && gun < 2 && m_ChamberedBefore[static_cast<std::size_t>(gun)]; }
        int SlideHeld() const { return m_SlideHeld; }
        float SlidePullM() const { return m_SlidePull; }
        std::uint32_t Serial() const { return m_Serial; }
        float Speed(int hand) const { return hand >= 0 && hand < 2 ? m_Speed[static_cast<std::size_t>(hand)] : 0.0f; }
        HandRole Role(int hand) const
        {
            if (hand < 0 || hand > 1 || m_Stage == Stage::Idle) return HandRole::None;
            if (!Stacked()) return HandRole::AkimboPistol;
            if (hand == m_Receiving) return HandRole::StackedPistols;
            if (m_Stage == Stage::HoldingMags) return HandRole::StackedMagazines;
            if (m_SlideHeld >= 0) return HandRole::Slide;
            return HandRole::Free;
        }

        unsigned Update(const Config& config, const Input& in)
        {
            unsigned events = 0u;
            std::array<bool, 2> grabPressed{}, gripPressed{};
            for (std::size_t h = 0; h < 2; ++h)
            {
                const bool grab = in.hands[h].valid && in.hands[h].grab;
                const bool grip = in.hands[h].valid && in.hands[h].grip;
                grabPressed[h] = grab && !m_PrevGrab[h];
                gripPressed[h] = grip && !m_PrevGrip[h];
                m_PrevGrab[h] = grab; m_PrevGrip[h] = grip;
                TrackSpeed(h, in.hands[h], in.nowMs);
            }
            if (!in.eligible)
            {
                if (m_Stage != Stage::Idle) events |= kEventCancelled;
                const auto speed = m_Speed; const auto last = m_LastPosition; const auto lastValid = m_LastValid;
                const auto lastMs = m_LastMs;
                Reset();
                m_Speed = speed; m_LastPosition = last; m_LastValid = lastValid; m_LastMs = lastMs;
                return events;
            }
            const bool bothValid = in.hands[0].valid && in.hands[1].valid;
            const float handGap = bothValid ? Distance(in.hands[0].position, in.hands[1].position) : 1e9f;
            switch (m_Stage)
            {
            case Stage::Idle:
                if (in.release)
                {
                    if (!in.canReload) { events |= kEventRefused; break; }
                    m_ChamberedBefore = {in.roundsRight > 0, in.roundsLeft > 0};
                    m_NeedsRack = {false, false};
                    m_Stage = Stage::Ejected; m_StackSince = 0u; m_StackArmed = true; m_Contact = false;
                    ++m_Serial;
                    events |= kEventEjected;
                }
                break;
            case Stage::Ejected:
            {
                if (!bothValid) { m_StackSince = 0u; break; }
                if (!m_StackArmed && handGap > config.stackDistanceM + 0.05f) m_StackArmed = true;
                const bool handsClose = handGap <= config.stackDistanceM;
                if (handsClose && !m_Contact) events |= kEventContact;
                m_Contact = handsClose;
                const bool aligned = AngleDeg(in.hands[0].forward, in.hands[1].forward) <= config.stackMaxAngleDeg &&
                    AngleDeg(in.hands[0].up, in.hands[1].up) <= config.stackMaxRollDeg;
                if (!m_StackArmed || !handsClose || !aligned) { m_StackSince = 0u; break; }
                if (!m_StackSince) m_StackSince = in.nowMs ? in.nowMs : 1u;
                if (in.nowMs - m_StackSince < config.stackDwellMs) break;
                // The hand that moved into the other one hands its gun over; when
                // neither clearly moved the left gun goes into the right hand.
                const int moving = m_Speed[1] >= m_Speed[0] ? 1 : 0;
                m_Receiving = 1 - moving;
                m_Stage = Stage::Stacked;
                events |= kEventStacked;
                break;
            }
            case Stage::Stacked:
            {
                const int f = 1 - m_Receiving;
                const auto& free = in.hands[static_cast<std::size_t>(f)];
                if (!free.valid) break;
                if (grabPressed[static_cast<std::size_t>(f)] && in.pouchValid &&
                    Distance(free.position, in.pouch) <= config.pouchReachM)
                {
                    m_Stage = Stage::HoldingMags;
                    events |= kEventGrabbedMags;
                }
                else if (gripPressed[static_cast<std::size_t>(f)] && bothValid && handGap <= config.takeBackReachM)
                {
                    // Before the magazines are in, the free hand can take its gun back.
                    m_Stage = Stage::Ejected; m_Receiving = -1; m_StackSince = 0u; m_StackArmed = false;
                    m_Contact = true;
                    events |= kEventUnstacked;
                }
                break;
            }
            case Stage::HoldingMags:
            {
                const int f = 1 - m_Receiving;
                const auto& free = in.hands[static_cast<std::size_t>(f)];
                if (!free.valid || !free.grab)
                {
                    m_Stage = Stage::Stacked;
                    events |= kEventDroppedMags;
                    break;
                }
                if (in.seatValid && Finite(in.seat) && Finite(in.held) &&
                    Distance(in.held, in.seat) <= config.insertRadiusM && in.holdAngleDeg <= config.insertMaxAngleDeg)
                {
                    m_Stage = Stage::Committing;
                    events |= kEventSeated;
                }
                break;
            }
            case Stage::Committing:
                break;
            case Stage::Racking:
            {
                const int f = 1 - m_Receiving;
                const auto& free = in.hands[static_cast<std::size_t>(f)];
                if (m_SlideHeld < 0)
                {
                    if (!free.valid || !grabPressed[static_cast<std::size_t>(f)]) break;
                    int best = -1; float bestDistance = config.slideReachM;
                    for (int gun = 0; gun < 2; ++gun)
                    {
                        const auto g = static_cast<std::size_t>(gun);
                        if (!m_NeedsRack[g] || !in.slideValid[g]) continue;
                        const float d = Distance(free.position, in.slide[g]);
                        if (d <= bestDistance) { bestDistance = d; best = gun; }
                    }
                    if (best >= 0)
                    {
                        m_SlideHeld = best; m_SlideStart = free.position; m_SlidePull = 0.0f;
                        events |= kEventSlideGrabbed;
                    }
                    break;
                }
                const auto g = static_cast<std::size_t>(m_SlideHeld);
                if (!free.valid || !free.grab)
                {
                    m_SlideHeld = -1; m_SlidePull = 0.0f;
                    events |= kEventSlideReleased;
                    if (!m_NeedsRack[0] && !m_NeedsRack[1]) m_Stage = Stage::Ready;
                    break;
                }
                const V3 back = Normalize(in.slideBack[g]);
                const float pull = Dot(Sub(free.position, m_SlideStart), back);
                m_SlidePull = std::fmax(0.0f, std::fmin(config.slideTravelM, std::isfinite(pull) ? pull : 0.0f));
                if (m_NeedsRack[g] && m_SlidePull >= config.slideRackM)
                {
                    m_NeedsRack[g] = false;
                    events |= kEventSlideRacked;
                }
                break;
            }
            case Stage::Ready:
            {
                const int f = 1 - m_Receiving;
                if (gripPressed[static_cast<std::size_t>(f)] && bothValid && handGap <= config.takeBackReachM)
                {
                    const auto speed = m_Speed; const auto last = m_LastPosition; const auto lastValid = m_LastValid;
                    const auto lastMs = m_LastMs; const auto serial = m_Serial;
                    Reset();
                    m_Speed = speed; m_LastPosition = last; m_LastValid = lastValid; m_LastMs = lastMs; m_Serial = serial;
                    events |= kEventDone;
                }
                break;
            }
            }
            return events;
        }

        // The rounds are in. roundsRight/Left: each gun's rounds after the reload.
        unsigned Commit(int roundsRight, int roundsLeft)
        {
            if (m_Stage != Stage::Committing) return 0u;
            m_NeedsRack = {!m_ChamberedBefore[0] && roundsRight > 0, !m_ChamberedBefore[1] && roundsLeft > 0};
            m_Stage = m_NeedsRack[0] || m_NeedsRack[1] ? Stage::Racking : Stage::Ready;
            return kEventCommitted;
        }

    private:
        void TrackSpeed(std::size_t h, const HandSample& hand, std::uint64_t nowMs)
        {
            if (!hand.valid || !Finite(hand.position)) { m_LastValid[h] = false; m_Speed[h] *= 0.5f; return; }
            if (m_LastValid[h] && nowMs > m_LastMs[h] && nowMs - m_LastMs[h] < 200u)
            {
                const float dt = static_cast<float>(nowMs - m_LastMs[h]) * 0.001f;
                const float instant = Distance(hand.position, m_LastPosition[h]) / dt;
                if (std::isfinite(instant)) m_Speed[h] = m_Speed[h] * 0.7f + instant * 0.3f;
            }
            m_LastPosition[h] = hand.position; m_LastMs[h] = nowMs; m_LastValid[h] = true;
        }

        Stage m_Stage = Stage::Idle;
        int m_Receiving = -1;
        std::array<bool, 2> m_ChamberedBefore{}, m_NeedsRack{};
        std::array<bool, 2> m_PrevGrab{}, m_PrevGrip{};
        std::array<float, 2> m_Speed{};
        std::array<V3, 2> m_LastPosition{};
        std::array<bool, 2> m_LastValid{};
        std::array<std::uint64_t, 2> m_LastMs{};
        std::uint64_t m_StackSince = 0u;
        bool m_StackArmed = true, m_Contact = false;
        int m_SlideHeld = -1;
        V3 m_SlideStart{};
        float m_SlidePull = 0.0f;
        std::uint32_t m_Serial = 0u;
    };

    // ---- Host transaction: l4d2vr_mag_dual_v1 ------------------------------------------------------
    // client: l4d2vr_mag_dual_v1 <token> <sequence> <handle> <command> <clip> <reserve>
    // server: l4d2vr_mag_dual_result <token> <sequence> <handle> <command> <clip> <reserve> <status> <clip'> <reserve'>
    // The token is the magazine-transaction token (l4d2vr_mag_capability); the
    // command is sent only to servers that advertise DualMagTxn (0x1000).
    constexpr char kRequestCommand[] = "l4d2vr_mag_dual_v1";
    constexpr char kResultCommand[] = "l4d2vr_mag_dual_result";
    struct Request
    {
        std::uint32_t token = 0u, sequence = 0u, handle = 0u, command = 0u;
        int clip = 0, reserve = 0;
        bool operator==(const Request& other) const
        {
            return token == other.token && sequence == other.sequence && handle == other.handle &&
                command == other.command && clip == other.clip && reserve == other.reserve;
        }
    };
    struct Reply { Request request{}; Status status = Status::Backend; int clip = 0, reserve = 0; };
    struct Snapshot
    {
        bool eligible = false;      // a VR player's active, owned dual pistols
        std::uint32_t handle = 0u, latestCommand = 0u;
        int clip = 0, reserve = 0, capacity = 0;  // capacity: the pair's (30)
        bool infinite = false;
    };

    inline bool ParseRequest(int argc, const char* const* argv, Request& request)
    {
        if (argc != 7 || !argv) return false;
        std::uint32_t values[6]{};
        constexpr std::uint32_t limits[6] = {0xffffffffu, 0xffffffffu, (1u << 22) - 1u, 0x7fffffffu,
            static_cast<std::uint32_t>(kMaxCapacity), static_cast<std::uint32_t>(kMaxReserve)};
        for (int arg = 1; arg <= 6; ++arg)
            if (!l4d2vr_shell::ParseNumber(argv[arg], limits[arg - 1], values[arg - 1])) return false;
        request = {values[0], values[1], values[2], values[3], static_cast<int>(values[4]), static_cast<int>(values[5])};
        return true;
    }
    inline bool FormatRequest(const Request& r, char* out, std::size_t size)
    {
        const int n = std::snprintf(out, size, "%s %u %u %u %u %d %d\n", kRequestCommand,
            r.token, r.sequence, r.handle, r.command, r.clip, r.reserve);
        return n > 0 && static_cast<std::size_t>(n) < size;
    }
    inline bool FormatReply(const Reply& reply, char* out, std::size_t size)
    {
        const Request& r = reply.request;
        const int n = std::snprintf(out, size, "%s %u %u %u %u %d %d %u %d %d\n", kResultCommand,
            r.token, r.sequence, r.handle, r.command, r.clip, r.reserve,
            static_cast<unsigned>(reply.status), std::max(0, reply.clip), std::max(0, reply.reserve));
        return n > 0 && static_cast<std::size_t>(n) < size;
    }
    inline bool ParseReply(int argc, const char* const* argv, Reply& reply)
    {
        if (argc != 10 || !argv) return false;
        std::uint32_t values[9]{};
        constexpr std::uint32_t limits[9] = {0xffffffffu, 0xffffffffu, (1u << 22) - 1u, 0x7fffffffu,
            static_cast<std::uint32_t>(kMaxCapacity), static_cast<std::uint32_t>(kMaxReserve), 7u,
            static_cast<std::uint32_t>(kMaxCapacity), static_cast<std::uint32_t>(kMaxReserve)};
        for (int arg = 1; arg <= 9; ++arg)
            if (!l4d2vr_shell::ParseNumber(argv[arg], limits[arg - 1], values[arg - 1])) return false;
        reply.request = {values[0], values[1], values[2], values[3], static_cast<int>(values[4]), static_cast<int>(values[5])};
        reply.status = static_cast<Status>(values[6]);
        reply.clip = static_cast<int>(values[7]); reply.reserve = static_cast<int>(values[8]);
        return true;
    }

    class ServerLedger
    {
    public:
        void Reset(std::uint32_t token) { *this = {}; m_Token = token; }
        std::uint32_t Token() const { return m_Token; }
        Reply Unsupported(const Request& request)
        {
            Reply reply{request, Status::Session, 0, 0};
            if (!m_Token || request.token != m_Token || !request.sequence) return reply;
            if (m_HaveMemo && request == m_Memo.request) return m_Memo;
            if (request.sequence <= m_HighSequence) { reply.status = Status::Stale; return reply; }
            m_HighSequence = request.sequence;
            reply.status = Status::Unsupported; m_Memo = reply; m_HaveMemo = true;
            return reply;
        }
        template<class Writer> Reply Apply(const Request& request, const Snapshot& state, std::uint64_t now, Writer writer)
        {
            Reply reply{request, Status::Session, state.clip, state.reserve};
            if (!m_Token || request.token != m_Token || !request.sequence) return reply;
            if (m_HaveMemo && request == m_Memo.request) return m_Memo;
            if (request.sequence <= m_HighSequence) { reply.status = Status::Stale; return reply; }
            m_HighSequence = request.sequence;
            AmmoResult result{};
            if (!state.eligible || !l4d2vr_shell::ValidHandle(request.handle) || request.handle != state.handle)
                reply.status = Status::Weapon;
            else if (!request.command || !state.latestCommand ||
                static_cast<std::int64_t>(state.latestCommand) - request.command > kCommandWindow ||
                static_cast<std::int64_t>(request.command) - state.latestCommand > kCommandWindow ||
                state.clip != request.clip || state.reserve != request.reserve)
                reply.status = Status::Stale;
            else if (!NativeReload(state.clip, state.reserve, state.infinite, state.capacity, result) || result.added <= 0)
                reply.status = Status::NoSpaceOrAmmo;
            else if (m_HaveApplied && (now < m_LastApplied || now - m_LastApplied < 100u))
                reply.status = Status::RateLimited;
            else if (!writer(result.clip, result.reserve))
                reply.status = Status::Backend;
            else
            {
                reply.status = Status::Applied; reply.clip = result.clip; reply.reserve = result.reserve;
                m_LastApplied = now; m_HaveApplied = true;
            }
            m_Memo = reply; m_HaveMemo = true;
            return reply;
        }
    private:
        std::uint32_t m_Token = 0u, m_HighSequence = 0u;
        bool m_HaveMemo = false, m_HaveApplied = false;
        std::uint64_t m_LastApplied = 0u;
        Reply m_Memo{};
    };

    class ClientRequest
    {
    public:
        enum class Poll { None, Waiting, Result, Timeout };
        // The magazine-transaction token; a new token starts a new session.
        void Offer(std::uint32_t token)
        {
            if (!token || token == m_Token) return;
            *this = {}; m_Token = token;
        }
        void Disconnect() { *this = {}; }
        bool Supported() const { return m_Token != 0u; }
        bool Pending() const { return m_Pending; }
        void Cancel() { m_Pending = m_Ready = false; m_Retries = 0u; }
        bool Begin(std::uint32_t handle, int clip, int reserve, std::uint32_t command, std::uintptr_t owner,
            std::uint64_t now, Request& out)
        {
            if (!m_Token || m_Pending || !owner || !command || command > 0x7fffffffu ||
                !l4d2vr_shell::ValidHandle(handle) || clip < 0 || clip > kMaxCapacity ||
                reserve < 0 || reserve > kMaxReserve || m_Next == (std::numeric_limits<std::uint32_t>::max)())
                return false;
            m_Request = {m_Token, ++m_Next, handle, command, clip, reserve};
            m_Owner = owner; m_Started = m_LastRetry = now;
            m_Pending = true; m_Ready = false; m_Retries = 0u;
            out = m_Request; return true;
        }
        bool NextRetry(std::uint64_t now, Request& request)
        {
            if (!m_Pending || m_Ready || m_Retries >= 3u || now < m_Started ||
                now - m_Started >= l4d2vr_shell::kReplyTimeoutMs || now < m_LastRetry || now - m_LastRetry < 500u)
                return false;
            ++m_Retries; m_LastRetry = now; request = m_Request;
            return true;
        }
        void Receive(const Reply& reply)
        {
            if (!m_Pending || !(reply.request == m_Request) || static_cast<unsigned>(reply.status) > 7u) return;
            if (reply.status == Status::Applied)
            {
                const int added = reply.clip - m_Request.clip;
                const int used = m_Request.reserve - reply.reserve;
                if (reply.clip < 0 || reply.clip > kMaxCapacity || reply.reserve < 0 || reply.reserve > kMaxReserve ||
                    added <= 0 || (used != 0 && used != added)) return;
            }
            m_Reply = reply; m_Ready = true;
        }
        // Result once the reply arrived and (when applied) the replicated clip shows it.
        Poll Update(std::uint32_t handle, std::uintptr_t owner, std::uint64_t now,
            int replicatedClip, int replicatedReserve, Reply& result)
        {
            if (!m_Pending) return Poll::None;
            if (handle != m_Request.handle || owner != m_Owner) { Cancel(); return Poll::None; }
            if (now < m_Started || now - m_Started >= l4d2vr_shell::kReplyTimeoutMs)
            { Cancel(); return Poll::Timeout; }
            if (!m_Ready || (m_Reply.status == Status::Applied &&
                (replicatedClip != m_Reply.clip || replicatedReserve != m_Reply.reserve))) return Poll::Waiting;
            result = m_Reply; Cancel();
            return Poll::Result;
        }
    private:
        std::uint32_t m_Token = 0u, m_Next = 0u;
        std::uintptr_t m_Owner = 0u;
        std::uint64_t m_Started = 0u, m_LastRetry = 0u;
        unsigned m_Retries = 0u;
        bool m_Pending = false, m_Ready = false;
        Request m_Request{};
        Reply m_Reply{};
    };
}

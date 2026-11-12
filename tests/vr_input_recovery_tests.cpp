#include "../L4D2VR/vr_input_recovery.h"
#include "openvr.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

using WORD = std::uint16_t;
#include "vr_custom_action_binding.inl"

namespace
{
    int checks = 0;
    void Require(bool result, const char* message)
    {
        ++checks;
        if (!result)
        {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    }
}

struct Game
{
    std::vector<std::string> commands;
    void ClientCmd_Unrestricted(const char* command) { commands.emplace_back(command); }
};

struct VR
{
    Game game;
    Game* m_Game = &game;
    l4d2vr_input::HeldCustomActions m_CustomActionInputs;
    bool m_CustomWalkHeld = false;
    bool m_ThirdPersonRenderOnCustomWalk = true;
    bool m_ThirdPersonFrontViewEnabled = false;
    float m_RotationOffset = 0.0f;
    vr::VRActionHandle_t m_CustomAction1 = 1u, m_CustomAction2 = 2u,
        m_CustomAction3 = 3u, m_CustomAction4 = 4u, m_CustomAction5 = 5u;
    CustomActionBinding m_CustomAction1Binding, m_CustomAction2Binding,
        m_CustomAction3Binding, m_CustomAction4Binding, m_CustomAction5Binding;
    struct Response { bool success = true; vr::InputDigitalActionData_t data{}; };
    std::array<Response, 5u> input{};
    std::vector<std::string> keys;

    bool GetDigitalActionData(vr::VRActionHandle_t& action, vr::InputDigitalActionData_t& data)
    {
        const auto& response = input.at(static_cast<std::size_t>(action - 1u));
        data = response.data;
        return response.success;
    }
    void Set(std::size_t index, bool active, bool held, bool changed = false)
    {
        input.at(index).success = true;
        input.at(index).data = {};
        input.at(index).data.bActive = active;
        input.at(index).data.bState = held;
        input.at(index).data.bChanged = changed;
    }
    void SendVirtualKeyDown(WORD key) { keys.push_back("down:" + std::to_string(key)); }
    void SendVirtualKeyUp(WORD key) { keys.push_back("up:" + std::to_string(key)); }
    void SendVirtualKey(WORD key) { SendVirtualKeyDown(key); SendVirtualKeyUp(key); }
    void SendCustomActionTransition(const l4d2vr_input::HeldTarget&, bool);
    void ReleaseCustomActionInputs();
    void RunCustomActions()
    {
        m_CustomWalkHeld = false;
#include "vr_custom_action_handler.inl"
    }
};

#include "vr_custom_action_transitions.inl"

static CustomActionBinding HoldKey(WORD key)
{
    CustomActionBinding binding{};
    binding.virtualKey = key;
    binding.holdVirtualKey = true;
    return binding;
}

static CustomActionBinding HoldCommand(const char* press, const char* release)
{
    CustomActionBinding binding{};
    binding.command = press;
    binding.releaseCommand = release;
    binding.usePressReleaseCommands = true;
    return binding;
}

static void TestCustomActions()
{
    VR vr;
    vr.m_CustomAction1Binding = HoldKey(88u);
    vr.Set(0u, true, true, true);
    vr.RunCustomActions();
    vr.Set(0u, true, true);
    vr.RunCustomActions();
    Require(vr.keys == std::vector<std::string>{"down:88"}, "A held key is injected once");
    vr.ReleaseCustomActionInputs();
    Require(vr.keys.back() == "up:88", "Entering a menu releases the owned key");
    vr.Set(0u, true, false);
    vr.RunCustomActions();
    Require(vr.keys.size() == 2u, "A release missed in a menu does not inject again");
    vr.Set(0u, true, true);
    vr.RunCustomActions();
    Require(vr.keys.back() == "down:88", "A held key resumes even without a runtime change edge");
    vr.Set(0u, false, true, true);
    vr.RunCustomActions();
    Require(vr.keys.back() == "up:88", "An inactive action releases the key despite stale pressed payload");
    vr.Set(0u, true, true);
    vr.RunCustomActions();
    vr.input[0].success = false;
    vr.RunCustomActions();
    Require(vr.keys.back() == "up:88", "A failed action read releases the key");
    const auto afterFailure = vr.keys.size();
    vr.RunCustomActions();
    Require(vr.keys.size() == afterFailure, "Repeated failures do not spam key releases");

    vr.Set(0u, true, true);
    vr.RunCustomActions();
    vr.m_CustomAction1Binding = HoldKey(89u);
    vr.RunCustomActions();
    Require(vr.keys[vr.keys.size()-2u] == "up:88" && vr.keys.back() == "down:89",
        "Rebinding releases the original key before pressing the replacement");
    vr.m_CustomAction2Binding = HoldKey(89u);
    vr.Set(1u, true, true);
    const auto beforeShared = vr.keys.size();
    vr.RunCustomActions();
    vr.Set(0u, true, false);
    vr.RunCustomActions();
    Require(vr.keys.size() == beforeShared, "Shared key owners prevent premature release");
    vr.Set(0u, true, true);
    vr.Set(1u, true, false);
    vr.RunCustomActions();
    Require(vr.keys.size() == beforeShared, "Ownership can transfer to a lower slot without releasing");
    vr.Set(0u, true, false);
    vr.Set(1u, true, true);
    vr.RunCustomActions();
    Require(vr.keys.size() == beforeShared, "Ownership can transfer to a higher slot without releasing");
    vr.ReleaseCustomActionInputs();
    Require(vr.keys.size() == beforeShared + 1u && vr.keys.back() == "up:89",
        "Cleanup releases a shared key only once");

    vr.Set(0u, true, true);
    vr.Set(1u, true, false);
    vr.m_CustomAction1Binding = HoldCommand("+walk", "-walk");
    vr.RunCustomActions();
    Require(vr.game.commands == std::vector<std::string>{"+walk"} && vr.m_CustomWalkHeld,
        "Held custom walk owns the press and updates its render hint");
    vr.m_CustomAction1Binding = HoldCommand("+speed", "-speed");
    vr.RunCustomActions();
    Require(vr.game.commands == std::vector<std::string>{"+walk", "-walk", "+speed"},
        "Command rebinding releases the original command");
    vr.Set(0u, true, false);
    vr.RunCustomActions();
    Require(vr.game.commands.back() == "-speed", "Neutral state releases commands without a change edge");
    vr.Set(0u, true, true);
    vr.RunCustomActions();
    vr.ReleaseCustomActionInputs();
    Require(vr.game.commands.back() == "-speed" && !vr.m_CustomWalkHeld,
        "Suppression or VR disable cleanup releases commands and clears custom walk");

    vr.m_CustomAction1Binding = {};
    vr.m_CustomAction1Binding.command = "thirdpersonfronttoggle";
    vr.Set(0u, false, true, true);
    vr.RunCustomActions();
    Require(!vr.m_ThirdPersonFrontViewEnabled, "Inactive internal commands cannot toggle a view");
    vr.Set(0u, true, true, true);
    vr.RunCustomActions();
    vr.Set(0u, true, true);
    vr.RunCustomActions();
    Require(vr.m_ThirdPersonFrontViewEnabled && vr.m_RotationOffset == 180.0f,
        "Internal one-shot commands preserve change-edge behavior");
}

static void TestControllerReadiness()
{
    l4d2vr_input::ControllerPairReadiness gate;
    Require(!gate.Update(true, true, 1000u, 1u, 1000u), "A single valid snapshot is not stable tracking");
    Require(!gate.Update(true, true, 1100u, 1u, 1100u), "Republishing the same pose token cannot advance stability");
    Require(!gate.Update(true, true, 1151u, 1u, 1151u), "A stalled pose token cannot survive its freshness limit");
    Require(!gate.Update(true, true, 1200u, 2u, 1200u), "Recovery begins a new stability interval");
    Require(!gate.Update(true, true, 1300u, 3u, 1300u), "One hundred milliseconds is insufficient");
    Require(!gate.Update(true, true, 1351u, 4u, 1351u), "Stalled time does not count toward recovery stability");
    Require(gate.Update(true, true, 1400u, 5u, 1400u), "Advancing fresh samples enable the pair after 200 ms");
    Require(gate.StableDurationMs() == 200u, "Stable duration comes from advancing samples");
    Require(gate.Update(true, true, 1450u, 5u, 1450u), "Repeated draws may use a still-fresh ready pair");
    // 2.0.11: a ready pair survives a hitch or a briefly lost controller.
    Require(gate.Update(true, true, 1551u, 5u, 1551u), "A frame hitch holds a ready pair");
    Require(gate.Update(true, false, 1600u, 6u, 1600u), "A briefly lost controller holds a ready pair");
    Require(gate.Update(true, true, 1700u, 7u, 1700u), "A fresh sample within the hold continues without re-stabilising");
    Require(gate.Update(true, true, 1720u, 8u, 1720u), "... and stays ready");
    Require(gate.Update(true, false, 2200u, 9u, 2200u), "Still held 480 ms after the last good sample");
    Require(!gate.Update(true, false, 2221u, 9u, 2221u), "A controller lost longer than the hold hides the arms");
    Require(!gate.Update(true, true, 2230u, 10u, 2230u), "Recovery after the hold does not reuse old stability");
    Require(!gate.Update(true, true, 2330u, 11u, 2330u), "Recovery still requires the full interval");
    Require(gate.Update(true, true, 2430u, 12u, 2430u), "A recovered pair becomes ready");
    Require(gate.Update(true, true, 2900u, 13u, 2900u), "A 470 ms gap between samples continues a ready pair");
    Require(!gate.Update(true, true, 3500u, 14u, 3500u), "A gap longer than the hold re-stabilises");
    Require(!gate.Update(true, true, 3600u, 15u, 3600u), "... for the full interval");
    Require(gate.Update(true, true, 3700u, 16u, 3700u), "Ready again");
    Require(!gate.Update(false, true, 3701u, 17u, 3701u), "Leaving first-person control resets readiness at once");
    Require(!gate.Update(true, true, 3702u, 18u, 3702u), "Returning to first person starts fresh");
    Require(!gate.Update(true, true, 4000u, 19u, 4151u), "Expired tracking snapshots are rejected");
    Require(!gate.Update(true, true, 4200u, 20u, 4199u), "Future timestamps are rejected");
    Require(!gate.Update(true, true, 0u, 21u, 4200u), "Missing capture timestamps are rejected");
    Require(!gate.Update(true, true, 4200u, 0u, 4200u), "Missing pose tokens are rejected");
    Require(!gate.Update(true, true, 4300u, 22u, 4300u), "Valid tracking restarts after invalid metadata");
    Require(!gate.Update(true, true, 4290u, 23u, 4300u), "Timestamp regression resets stability");
    // A timestamp regression resets even a ready pair.
    l4d2vr_input::ControllerPairReadiness ready;
    ready.Update(true, true, 1000u, 1u, 1000u);
    ready.Update(true, true, 1100u, 2u, 1100u);
    Require(ready.Update(true, true, 1200u, 3u, 1200u), "Ready");
    Require(!ready.Update(true, true, 1150u, 4u, 1210u), "A regressed capture time resets a ready pair");
}

int main()
{
    TestCustomActions();
    TestControllerReadiness();
    std::printf("Input recovery passed %d checks\n", checks);
}

// End-to-end harness for the Linux plugin's engine callbacks (32-bit only).
// No server_srv.so is loaded in this process, so the plugin must come up as
// the pose relay (+v3) without any gameplay hooks, map edicts like the proven
// relay and never send VR capability commands to clients that did not say
// hello. Build: see ../run_server_tests.sh.

#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "../l4d2vr_server_linux.cpp"

namespace
{
    struct CommandLayout
    {
        int argc;
        int argv0Size;
        char argSBuffer[512];
        char argvBuffer[512];
        const char* argv[64];
    };
    static_assert(sizeof(CommandLayout) == sizeof(CCommand), "CCommand test layout drifted.");

    class TestCommand
    {
    public:
        TestCommand(std::initializer_list<const char*> arguments)
        {
            std::memset(storage_.data(), 0, storage_.size());
            auto* layout = reinterpret_cast<CommandLayout*>(storage_.data());
            layout->argc = static_cast<int>(arguments.size());
            int index = 0;
            for (const char* argument : arguments) layout->argv[index++] = argument;
        }
        const CCommand& get() const { return *reinterpret_cast<const CCommand*>(storage_.data()); }
    private:
        alignas(CCommand) std::array<unsigned char, sizeof(CCommand)> storage_{};
    };

    class FakeEngine final : public IVEngineServer
    {
    public:
        struct SentCommand { edict_t* entity; std::string text; };
        std::vector<SentCommand> sent;

        void ChangeLevel(const char*, const char*) override {}
        int IsMapValid(const char*) override { return 0; }
        bool IsDedicatedServer() override { return true; }
        int IsInEditMode() override { return 0; }
        void* GetLaunchOptions() override { return nullptr; }
        int PrecacheModel(const char*, bool) override { return 0; }
        int PrecacheSentenceFile(const char*, bool) override { return 0; }
        int PrecacheDecal(const char*, bool) override { return 0; }
        int PrecacheGeneric(const char*, bool) override { return 0; }
        bool IsModelPrecached(const char*) const override { return false; }
        bool IsDecalPrecached(const char*) const override { return false; }
        bool IsGenericPrecached(const char*) const override { return false; }
        int GetClusterForOrigin(const Vector&) override { return 0; }
        int GetPVSForCluster(int, int, unsigned char*) override { return 0; }
        bool CheckOriginInPVS(const Vector&, const unsigned char*, int) override { return false; }
        bool CheckBoxInPVS(const Vector&, const Vector&, const unsigned char*, int) override { return false; }
        int GetPlayerUserId(const edict_t*) override { return 0; }
        const char* GetPlayerNetworkIDString(const edict_t*) override { return ""; }
        bool IsUserIDInUse(int) override { return false; }
        int GetLoadingProgressForUserID(int) override { return 0; }
        int GetEntityCount() override { return 0; }
        void* GetPlayerNetInfo(int) override { return nullptr; }
        edict_t* CreateEdict(int) override { return nullptr; }
        void RemoveEdict(edict_t*) override {}
        void* PvAllocEntPrivateData(long) override { return nullptr; }
        void FreeEntPrivateData(void*) override {}
        void* SaveAllocMemory(std::size_t, std::size_t) override { return nullptr; }
        void SaveFreeMemory(void*) override {}
        void EmitAmbientSound(int, const Vector&, const char*, float, int, int, int, float) override {}
        void FadeClientVolume(const edict_t*, float, float, float, float) override {}
        int SentenceGroupPick(int, char*, int) override { return 0; }
        int SentenceGroupPickSequential(int, char*, int, int, int) override { return 0; }
        int SentenceIndexFromName(const char*) override { return 0; }
        const char* SentenceNameFromIndex(int) override { return ""; }
        int SentenceGroupIndexFromName(const char*) override { return 0; }
        const char* SentenceGroupNameFromIndex(int) override { return ""; }
        float SentenceLength(int) override { return 0.0f; }
        void ServerCommand(const char*) override {}
        void ServerExecute() override {}
        void ClientCommand(edict_t* entity, const char* format, ...) override
        {
            char buffer[256]{};
            va_list arguments;
            va_start(arguments, format);
            const int written = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
            va_end(arguments);
            assert(written >= 0 && written < static_cast<int>(sizeof(buffer)));
            sent.push_back({ entity, buffer });
        }
        std::vector<std::string> To(edict_t* entity) const
        {
            std::vector<std::string> out;
            for (const auto& s : sent) if (s.entity == entity) out.push_back(s.text);
            return out;
        }
    };

    FakeEngine* g_FakeEngine = nullptr;

    void* InterfaceFactory(const char* name, int* result)
    {
        if (name && std::strcmp(name, "VEngineServer022") == 0)
        {
            if (result) *result = IFACE_OK;
            return g_FakeEngine;
        }
        if (result) *result = IFACE_FAILED;
        return nullptr;
    }

    PLUGIN_RESULT Dispatch(IServerPluginCallbacks& plugin, edict_t* entity, std::initializer_list<const char*> args)
    {
        TestCommand command(args);
        return plugin.ClientCommand(entity, command.get());
    }

    std::string ValidPayload()
    {
        l4d2vr_pose::WirePacket packet{};
        l4d2vr_pose::FinalizePacket(packet);
        std::string encoded;
        assert(l4d2vr_pose::EncodePayload(packet, encoded));
        return encoded;
    }
}

int main()
{
    int result = IFACE_FAILED;
    auto* plugin = static_cast<IServerPluginCallbacks*>(CreateInterface("ISERVERPLUGINCALLBACKS003", &result));
    assert(plugin && result == IFACE_OK);
    assert(CreateInterface("ISERVERPLUGINCALLBACKS002", &result) == nullptr && result == IFACE_FAILED);

    FakeEngine engine;
    g_FakeEngine = &engine;
    assert(!plugin->Load(nullptr, nullptr));
    assert(plugin->Load(&InterfaceFactory, nullptr));
    // No server_srv.so in this process: relay only, never the usercmd ack.
    assert(!g_Session.Capabilities().usercmdDecoder);
    assert(g_Session.Capabilities().features == kFeaturePoseV3);

    std::array<edict_t, 5> edicts{};
    plugin->ServerActivate(edicts.data(), static_cast<int>(edicts.size()), 3);
    const auto base = reinterpret_cast<std::uintptr_t>(edicts.data());
    plugin->ClientPutInServer(&edicts[0], "world");
    plugin->ClientPutInServer(&edicts[4], "over-client-max");
    plugin->ClientPutInServer(reinterpret_cast<edict_t*>(base + 1u), "unaligned");
    plugin->ClientPutInServer(reinterpret_cast<edict_t*>(base - sizeof(edict_t)), "before-base");
    assert(engine.sent.empty());

    plugin->ClientPutInServer(&edicts[1], "vr-one");
    plugin->ClientPutInServer(&edicts[2], "vr-two");
    plugin->ClientPutInServer(&edicts[3], "desktop");
    assert(engine.sent.size() == 3u);
    for (const auto& s : engine.sent) assert(s.text == "l4d2vr_pose_ack 2\n");

    engine.sent.clear();
    assert(Dispatch(*plugin, &edicts[1], { "l4d2vr_pose_hello", "2" }) == PLUGIN_STOP);
    assert(Dispatch(*plugin, &edicts[2], { "l4d2vr_pose_hello", "3" }) == PLUGIN_STOP);
    assert(engine.To(&edicts[1]) == std::vector<std::string>{ "l4d2vr_feature_ack 100 1\n" });
    assert(engine.To(&edicts[2]) == std::vector<std::string>{ "l4d2vr_feature_ack 100 1\n" });
    assert(engine.To(&edicts[3]).empty());

    // Ordinary commands pass through; private ones are consumed silently.
    assert(Dispatch(*plugin, &edicts[3], { "say", "hi" }) == PLUGIN_CONTINUE);
    engine.sent.clear();
    assert(Dispatch(*plugin, &edicts[1], { "l4d2vr_mag_action_v2", "1", "1", "12290", "300", "5", "30", "100", "0" }) == PLUGIN_STOP);
    assert(Dispatch(*plugin, &edicts[1], { "l4d2vr_shell_insert_v1", "1", "1", "12290", "300", "3", "3", "50" }) == PLUGIN_STOP);
    assert(engine.sent.empty());

    // v2 relay through the plugin: 1 -> 2 only (3 never said hello).
    const std::string payload = ValidPayload();
    assert(Dispatch(*plugin, &edicts[1], { "l4d2vr_pose_upload", "10", payload.c_str() }) == PLUGIN_STOP);
    assert(engine.sent.size() == 1u && engine.sent[0].entity == &edicts[2] &&
        engine.sent[0].text == "l4d2vr_pose_receive 1 10 " + payload + "\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    engine.sent.clear();
    Dispatch(*plugin, &edicts[2], { "l4d2vr_pose_upload", "3", payload.c_str() });
    assert(engine.sent.size() == 1u && engine.sent[0].entity == &edicts[1]);

    // Disconnect frees the slot; a level change resets everything.
    plugin->ClientDisconnect(&edicts[2]);
    engine.sent.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Dispatch(*plugin, &edicts[1], { "l4d2vr_pose_upload", "11", payload.c_str() });
    assert(engine.sent.empty());
    plugin->LevelInit("c1m2_streets");
    Dispatch(*plugin, &edicts[1], { "l4d2vr_pose_hello", "2" });   // edicts unknown until ServerActivate
    assert(engine.sent.empty());
    plugin->ServerActivate(edicts.data(), static_cast<int>(edicts.size()), 3);
    plugin->ClientPutInServer(&edicts[1], "vr-one");
    assert(engine.To(&edicts[1]) == std::vector<std::string>{ "l4d2vr_pose_ack 2\n" });
    plugin->LevelShutdown();
    plugin->Unload();
    engine.sent.clear();
    Dispatch(*plugin, &edicts[1], { "l4d2vr_pose_hello", "2" });
    assert(engine.sent.empty());
    std::puts("l4d2vr_server_plugin_test passed");
    return 0;
}

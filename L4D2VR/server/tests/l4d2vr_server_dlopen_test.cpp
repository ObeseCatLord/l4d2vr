// Loads the built release plugin with the system dynamic loader (RTLD_NOW,
// like srcds) and calls its factory and a few callbacks through the
// ISERVERPLUGINCALLBACKS003 vtable. Verifies static initialisation, symbol
// binding against this glibc and the exported interface of the shipped .so.
//
//   l4d2vr_server_dlopen_test <path/to/l4d2vr_server.so>

#include <cstdio>
#include <cstring>
#include <dlfcn.h>

namespace
{
    using CreateInterfaceFn = void* (*)(const char*, int*);

    // Only the leading slots are declared; the order matches the engine's.
    class PluginPrefix
    {
    public:
        virtual bool Load(CreateInterfaceFn, CreateInterfaceFn) = 0;
        virtual void Unload() = 0;
        virtual void Pause() = 0;
        virtual void UnPause() = 0;
        virtual const char* GetPluginDescription() = 0;
        virtual void LevelInit(const char*) = 0;
    };

    void* NoInterfaces(const char*, int* code)
    {
        if (code) *code = 1;
        return nullptr;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <l4d2vr_server.so>\n", argv[0]);
        return 2;
    }
    void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module)
    {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    auto factory = reinterpret_cast<CreateInterfaceFn>(dlsym(module, "CreateInterface"));
    if (!factory) { std::fprintf(stderr, "CreateInterface missing\n"); return 1; }
    int code = -1;
    if (factory("ISERVERPLUGINCALLBACKS002", &code) != nullptr || code != 1)
    {
        std::fprintf(stderr, "unexpected interface for 002\n");
        return 1;
    }
    auto* plugin = static_cast<PluginPrefix*>(factory("ISERVERPLUGINCALLBACKS003", &code));
    if (!plugin || code != 0) { std::fprintf(stderr, "ISERVERPLUGINCALLBACKS003 missing\n"); return 1; }
    const char* description = plugin->GetPluginDescription();
    if (!description || std::strstr(description, "L4D2VR") == nullptr)
    {
        std::fprintf(stderr, "bad description\n");
        return 1;
    }
    // Without VEngineServer022 the plugin must refuse to load, cleanly.
    if (plugin->Load(&NoInterfaces, &NoInterfaces))
    {
        std::fprintf(stderr, "Load succeeded without an engine\n");
        return 1;
    }
    plugin->LevelInit("c1m1_hotel");
    // Other exported symbols would collide with the engine's own runtime.
    if (dlsym(module, "_Znwj") || dlsym(module, "_ZSt17__throw_bad_allocv") || dlsym(module, "malloc") != dlsym(RTLD_DEFAULT, "malloc"))
    {
        std::fprintf(stderr, "plugin leaks runtime symbols\n");
        return 1;
    }
    char copy[128];
    std::snprintf(copy, sizeof(copy), "%s", description);   // the string lives in the module
    dlclose(module);
    std::printf("l4d2vr_server_dlopen_test passed: %s\n", copy);
    return 0;
}

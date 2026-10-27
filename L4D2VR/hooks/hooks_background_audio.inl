// Background audio (Workshop 1.1.7): keep game audio playing while the game
// window is unfocused (common in VR, where the desktop window rarely has
// focus). DirectSound secondary buffers are created with DSBCAPS_GLOBALFOCUS.

namespace
{
    constexpr DWORD kDsbcapsPrimaryBuffer = 0x00000001u;
    constexpr DWORD kDsbcapsGlobalFocus = 0x00008000u;
    constexpr size_t kDirectSoundCreateSoundBufferSlot = 3;

    using DirectSoundCreateFn = HRESULT(WINAPI*)(const GUID*, void**, void*);
    using CreateSoundBufferFn = HRESULT(__stdcall*)(void* self, const void* desc, void** buffer, void* outer);

    DirectSoundCreateFn g_BackgroundAudioOriginalCreate8 = nullptr;
    DirectSoundCreateFn g_BackgroundAudioOriginalCreate = nullptr;
    CreateSoundBufferFn g_BackgroundAudioOriginalCreateBuffer = nullptr;
    std::atomic<bool> g_BackgroundAudioActive{ false };

    HRESULT __stdcall BackgroundAudioCreateSoundBuffer(void* self, const void* desc, void** buffer, void* outer)
    {
        // DSBUFFERDESC: dwSize at 0, dwFlags at 4. Copy instead of writing the
        // caller's const descriptor; the primary buffer cannot take the flag.
        if (desc && g_BackgroundAudioActive.load(std::memory_order_acquire))
        {
            DWORD size = 0;
            std::memcpy(&size, desc, sizeof(size));
            if (size >= 8u && size <= 64u)
            {
                unsigned char copy[64]{};
                std::memcpy(copy, desc, size);
                DWORD flags = 0;
                std::memcpy(&flags, copy + 4, sizeof(flags));
                if ((flags & kDsbcapsPrimaryBuffer) == 0u)
                {
                    flags |= kDsbcapsGlobalFocus;
                    std::memcpy(copy + 4, &flags, sizeof(flags));
                    return g_BackgroundAudioOriginalCreateBuffer(self, copy, buffer, outer);
                }
            }
        }
        return g_BackgroundAudioOriginalCreateBuffer(self, desc, buffer, outer);
    }

    void BackgroundAudioPatchDevice(void* device)
    {
        if (!device)
            return;
        void** const vtable = *reinterpret_cast<void***>(device);
        if (!vtable)
            return;
        void** const slot = &vtable[kDirectSoundCreateSoundBufferSlot];
        if (*slot == reinterpret_cast<void*>(&BackgroundAudioCreateSoundBuffer))
            return;
        DWORD oldProtect = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
            return;
        if (!g_BackgroundAudioOriginalCreateBuffer)
            g_BackgroundAudioOriginalCreateBuffer = reinterpret_cast<CreateSoundBufferFn>(*slot);
        *slot = reinterpret_cast<void*>(&BackgroundAudioCreateSoundBuffer);
        DWORD unused = 0;
        VirtualProtect(slot, sizeof(void*), oldProtect, &unused);
        Game::logMsg("[VR][BackgroundAudio] DirectSound buffers use global focus");
    }

    HRESULT WINAPI BackgroundAudioDirectSoundCreate8(const GUID* guid, void** device, void* outer)
    {
        const HRESULT result = g_BackgroundAudioOriginalCreate8(guid, device, outer);
        if (SUCCEEDED(result) && device)
            BackgroundAudioPatchDevice(*device);
        return result;
    }

    HRESULT WINAPI BackgroundAudioDirectSoundCreate(const GUID* guid, void** device, void* outer)
    {
        const HRESULT result = g_BackgroundAudioOriginalCreate(guid, device, outer);
        if (SUCCEEDED(result) && device)
            BackgroundAudioPatchDevice(*device);
        return result;
    }

    void InstallBackgroundAudioHooks(bool enabled)
    {
        g_BackgroundAudioActive.store(enabled, std::memory_order_release);
        if (!enabled)
            return;
        HMODULE dsound = GetModuleHandleW(L"dsound.dll");
        if (!dsound)
            dsound = LoadLibraryW(L"dsound.dll");
        if (!dsound)
        {
            Game::logMsg("[VR][BackgroundAudio] dsound.dll unavailable");
            return;
        }
        struct Target { const char* name; LPVOID detour; DirectSoundCreateFn* original; };
        const Target targets[] = {
            { "DirectSoundCreate8", reinterpret_cast<LPVOID>(&BackgroundAudioDirectSoundCreate8), &g_BackgroundAudioOriginalCreate8 },
            { "DirectSoundCreate", reinterpret_cast<LPVOID>(&BackgroundAudioDirectSoundCreate), &g_BackgroundAudioOriginalCreate },
        };
        for (const Target& target : targets)
        {
            void* const address = reinterpret_cast<void*>(GetProcAddress(dsound, target.name));
            if (!address || *target.original)
                continue;
            if (MH_CreateHook(address, target.detour, reinterpret_cast<LPVOID*>(target.original)) != MH_OK ||
                MH_EnableHook(address) != MH_OK)
            {
                Game::logMsg("[VR][BackgroundAudio] %s hook failed", target.name);
                *target.original = nullptr;
            }
        }
    }
}

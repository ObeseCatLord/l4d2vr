// State shared by the recoil hooks (included before the other hook files;
// the hook bodies are in hooks_recoil.inl).

namespace
{
    // Command number being simulated by client prediction (RunCommand), so a
    // shot kicks once even when prediction replays the command.
    thread_local int g_RecoilClientCommand = 0;
    // Command the listen host is executing for its own local player.
    thread_local int g_RecoilServerCommand = 0;
    thread_local const void* g_RecoilServerOwner = nullptr;

    // Shot pose of each firing command, captured when the command is created.
    // The server fires that command along this pose; sampling the hand again
    // when the command is sent (a choked command goes out frames later) or when
    // the listen host runs it (next frame) would include that shot's own kick.
    struct RecoilShotPose
    {
        int command = 0;
        Vector position{};
        QAngle angles{};
    };
    constexpr size_t kRecoilShotPoseCount = 128;
    RecoilShotPose g_RecoilShotPoses[kRecoilShotPoseCount]{};
    std::mutex g_RecoilShotPoseMutex;

    void StoreRecoilShotPose(int command, const Vector& position, const QAngle& angles)
    {
        if (command <= 0) return;
        std::lock_guard<std::mutex> lock(g_RecoilShotPoseMutex);
        RecoilShotPose& slot = g_RecoilShotPoses[static_cast<size_t>(command) % kRecoilShotPoseCount];
        slot.command = command;
        slot.position = position;
        slot.angles = angles;
    }

    bool LoadRecoilShotPose(int command, Vector& position, QAngle& angles)
    {
        if (command <= 0) return false;
        std::lock_guard<std::mutex> lock(g_RecoilShotPoseMutex);
        const RecoilShotPose& slot = g_RecoilShotPoses[static_cast<size_t>(command) % kRecoilShotPoseCount];
        if (slot.command != command) return false;
        position = slot.position;
        angles = slot.angles;
        return true;
    }

    // Fixed shotgun pellets: SharedRandomFloat draws of the pellet loop.
    thread_local l4d2vr_shotgun_pattern::ShotContext* g_ClientPelletShot = nullptr;
    thread_local l4d2vr_shotgun_pattern::ShotContext* g_ServerPelletShot = nullptr;

    struct PelletShotScope
    {
        l4d2vr_shotgun_pattern::ShotContext shot{};
        l4d2vr_shotgun_pattern::ShotContext** slot = nullptr;
        l4d2vr_shotgun_pattern::ShotContext* previous = nullptr;
        PelletShotScope(l4d2vr_shotgun_pattern::ShotContext*& target, const l4d2vr_shotgun_pattern::Weapon* weapon)
        {
            if (!weapon) return;
            shot.weapon = weapon;
            slot = &target;
            previous = target;
            target = &shot;
        }
        ~PelletShotScope()
        {
            if (slot) *slot = previous;
        }
    };

    // SharedRandomFloat has no exported symbol on Windows. Its callers pass the
    // draw's name as the first argument (push imm32), so find the pellet loop's
    // name string, the push that references it and the call right after it.
    void* FindSharedRandomFloat(const char* moduleName)
    {
        HMODULE module = GetModuleHandleA(moduleName);
        if (!module) return nullptr;
        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return nullptr;
        const uint8_t* base = static_cast<const uint8_t*>(info.lpBaseOfDll);
        const size_t size = info.SizeOfImage;
        const char* name = l4d2vr_shotgun_pattern::kDirectionName;
        const size_t nameLength = std::strlen(name) + 1;
        __try
        {
            for (size_t i = 0; i + nameLength <= size; ++i)
            {
                if (base[i] != static_cast<uint8_t>(name[0]) || std::memcmp(base + i, name, nameLength) != 0)
                    continue;
                const uint32_t address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base + i));
                for (size_t j = 0; j + 5 <= size; ++j)
                {
                    if (base[j] != 0x68 || std::memcmp(base + j + 1, &address, 4) != 0)
                        continue;
                    for (size_t k = j + 5; k < j + 64 && k + 5 <= size; ++k)
                    {
                        if (base[k] != 0xE8) continue;
                        int32_t rel = 0;
                        std::memcpy(&rel, base + k + 1, 4);
                        const uint8_t* target = base + k + 5 + rel;
                        if (target < base || target >= base + size) break;
                        // push ebp; mov ebp, esp
                        if (target[0] == 0x55 && target[1] == 0x8B && target[2] == 0xEC)
                            return const_cast<uint8_t*>(target);
                        break;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return nullptr;
    }

    template <typename Original>
    float PelletDraw(l4d2vr_shotgun_pattern::ShotContext* shot, Original original,
        const char* name, float minValue, float maxValue, int additionalSeed, bool& replaced)
    {
        float value = 0.0f;
        replaced = false;
        if (shot && original && l4d2vr_shotgun_pattern::Override(*shot, name, minValue, maxValue, additionalSeed, value))
        {
            replaced = true;
            return value;
        }
        return original ? original(name, minValue, maxValue, additionalSeed) : minValue;
    }
}


#pragma once

#include <mutex>
#include <string>

// Self-test only (step gloveassets=): a folder the glove meshes load from
// instead of the runtime's resources/rendermodels/vr_glove. Empty = unused.
namespace l4d2vr_hand_test_assets
{
    inline std::mutex& Mutex()
    {
        static std::mutex mutex;
        return mutex;
    }
    inline std::string& Storage()
    {
        static std::string directory;
        return directory;
    }
    inline std::string Directory()
    {
        std::lock_guard<std::mutex> lock(Mutex());
        return Storage();
    }
    inline void SetDirectory(const std::string& directory)
    {
        std::lock_guard<std::mutex> lock(Mutex());
        Storage() = directory;
    }
}

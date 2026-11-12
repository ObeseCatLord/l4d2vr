// Config hot-reload lock (review B05): readers nest, a writer never makes a
// reader block, a parse from inside a frame is deferred, timeouts work.
#include "../L4D2VR/vr_config_lock.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unordered_map>

static int g_Failures = 0;
static int g_Checks = 0;
#define CHECK(cond) do { ++g_Checks; if (!(cond)) { ++g_Failures; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

using l4d2vr_config_lock::ConfigLock;

static void TestNestedReadersAndDeferredWrite()
{
    ConfigLock lock;
    {
        ConfigLock::ReadScope outer(lock);
        ConfigLock::ReadScope inner(lock);   // must not lock twice
        CHECK(ConfigLock::CurrentThreadReads());
        ConfigLock::WriteScope write(lock, std::chrono::milliseconds(0));
        CHECK(!write.Owns());                // a frame cannot reparse in place
    }
    CHECK(!ConfigLock::CurrentThreadReads());
    ConfigLock::WriteScope write(lock);
    CHECK(write.Owns());
    {
        ConfigLock::WriteScope nested(lock);
        CHECK(nested.Owns());                // nested write on the writing thread
        ConfigLock::ReadScope readInWrite(lock);  // and a read inside it: no self-deadlock
        CHECK(ConfigLock::CurrentThreadReads());
    }
}

static void TestWriterTimesOutWhileOtherThreadReads()
{
    ConfigLock lock;
    std::atomic<bool> reading{ false };
    std::atomic<bool> release{ false };
    std::thread reader([&]() {
        ConfigLock::ReadScope read(lock);
        reading = true;
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    while (!reading)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    {
        const auto start = std::chrono::steady_clock::now();
        ConfigLock::WriteScope write(lock, std::chrono::milliseconds(30));
        CHECK(!write.Owns());
        CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(25));
    }
    // A second reader is not blocked by the polling writer.
    std::atomic<bool> secondDone{ false };
    std::thread writer([&]() {
        ConfigLock::WriteScope write(lock);   // waits for the reader
        CHECK(write.Owns());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::thread second([&]() {
        ConfigLock::ReadScope read(lock);
        secondDone = true;
    });
    second.join();
    CHECK(secondDone.load());
    release = true;
    reader.join();
    writer.join();
}

static void TestMapIsNeverSeenHalfRebuilt()
{
    ConfigLock lock;
    std::unordered_map<int, int> settings;
    for (int i = 0; i < 64; ++i)
        settings[i] = i;
    std::atomic<bool> stop{ false };
    std::atomic<int> torn{ 0 };
    std::thread writer([&]() {
        for (int round = 0; round < 300; ++round)
        {
            ConfigLock::WriteScope write(lock);
            settings.clear();
            for (int i = 0; i < 64; ++i)
                settings[i] = i;
        }
        stop = true;
    });
    std::thread reader([&]() {
        while (!stop)
        {
            ConfigLock::ReadScope read(lock);
            if (settings.size() != 64 || settings.find(63) == settings.end())
                ++torn;
        }
    });
    writer.join();
    reader.join();
    CHECK(torn.load() == 0);
}

static void TestMaybeReadScope()
{
    ConfigLock lock;
    {
        ConfigLock::MaybeReadScope none(nullptr);
        CHECK(!ConfigLock::CurrentThreadReads());
    }
    {
        ConfigLock::MaybeReadScope some(&lock);
        CHECK(ConfigLock::CurrentThreadReads());
    }
    CHECK(!ConfigLock::CurrentThreadReads());
}

int main()
{
    TestNestedReadersAndDeferredWrite();
    TestWriterTimesOutWhileOtherThreadReads();
    TestMapIsNeverSeenHalfRebuilt();
    TestMaybeReadScope();
    std::printf("vr_config_lock_tests: %d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}

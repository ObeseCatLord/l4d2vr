#pragma once

// 2.0.11 (review B05): config hot reload used to rewrite the VR settings
// (unordered_maps, strings, vectors) on the watcher thread while the game and
// render threads read them. ParseConfigFile now holds this lock exclusively,
// and the per-frame entry points (VR::Update, RenderView, CreateMove,
// DrawModelExecute) hold it shared for their whole run.
//
// Deadlock rules:
// - The writer never waits for readers: it only polls try_lock(), so a
//   pending writer never makes a new reader block (SRW locks would otherwise
//   queue readers behind it, and a reader that re-enters would deadlock).
// - A thread that already reads keeps its read (depth counter), so nested
//   entry points never lock twice.
// - A parse requested from inside a read (self-test step, overlay code
//   running in a frame) cannot take the write lock on that thread: the caller
//   gets false and hands the parse to the watcher thread.
// The reader blocks only while a parse is actually running (a few ms).

#include <atomic>
#include <chrono>
#include <optional>
#include <shared_mutex>
#include <thread>

namespace l4d2vr_config_lock
{
    class ConfigLock
    {
    public:
        class ReadScope
        {
        public:
            explicit ReadScope(ConfigLock& lock) : m_Lock(lock)
            {
                // The writing thread already has exclusive access.
                if (Depth()++ == 0 && WriteDepth() == 0)
                {
                    m_Lock.m_Mutex.lock_shared();
                    m_Owns = true;
                }
            }
            ~ReadScope()
            {
                --Depth();
                if (m_Owns)
                    m_Lock.m_Mutex.unlock_shared();
            }
            ReadScope(const ReadScope&) = delete;
            ReadScope& operator=(const ReadScope&) = delete;

        private:
            ConfigLock& m_Lock;
            bool m_Owns = false;
        };

        class WriteScope
        {
        public:
            // timeout < 0: wait until the lock is free. A thread that holds
            // another lock a reader may wait for (the config overlay's state
            // mutex) must pass a timeout and handle Owns() == false.
            explicit WriteScope(ConfigLock& lock, std::chrono::milliseconds timeout = std::chrono::milliseconds(-1))
                : m_Lock(lock)
            {
                if (Depth() != 0 || WriteDepth() != 0)
                    return;   // the caller reads (cannot upgrade) or already writes
                const auto deadline = std::chrono::steady_clock::now() + (timeout.count() > 0 ? timeout : std::chrono::milliseconds(0));
                while (!m_Lock.m_Mutex.try_lock())
                {
                    if (timeout.count() >= 0 && std::chrono::steady_clock::now() >= deadline)
                        return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                m_Owns = true;
                ++WriteDepth();
            }
            ~WriteScope()
            {
                if (m_Owns)
                {
                    --WriteDepth();
                    m_Lock.m_Mutex.unlock();
                }
            }
            // False when the calling thread reads (the write must be deferred)
            // or the timeout passed.
            // True when it holds the lock, or an outer WriteScope on this thread does.
            bool Owns() const { return m_Owns || WriteDepth() != 0; }
            WriteScope(const WriteScope&) = delete;
            WriteScope& operator=(const WriteScope&) = delete;

        private:
            ConfigLock& m_Lock;
            bool m_Owns = false;
        };

        // A ReadScope when the lock exists (hooks that may run before VR does).
        class MaybeReadScope
        {
        public:
            explicit MaybeReadScope(ConfigLock* lock)
            {
                if (lock)
                    m_Scope.emplace(*lock);
            }

        private:
            std::optional<ReadScope> m_Scope;
        };

        // True on a thread inside a ReadScope (a parse must be deferred).
        static bool CurrentThreadReads() { return Depth() != 0; }

    private:
        static int& Depth()
        {
            thread_local int depth = 0;
            return depth;
        }
        static int& WriteDepth()
        {
            thread_local int depth = 0;
            return depth;
        }

        std::shared_mutex m_Mutex;
    };
}

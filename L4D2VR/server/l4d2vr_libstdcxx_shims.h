#pragma once

// Keeps the statically linked libstdc++ subset free of exception handling.
//
// The plugin is built with -fno-exceptions and never throws, but the
// libstdc++ container/string code still calls the std::__throw_* helpers and
// the global operator new, whose archive members drag in the whole EH runtime
// (libgcc_eh, eh_globals, COW stdexcept) and with it newer glibc symbol
// versions (__libc_single_threaded@GLIBC_2.32, pthread_once@GLIBC_2.34).
// An exception could not cross the engine's frames anyway, so these
// definitions terminate exactly like an uncaught throw would. Defining them in
// the plugin keeps the archive members out of the link and the plugin's glibc
// requirement at GLIBC_2.17. Visibility is hidden (-fvisibility=hidden plus
// --exclude-libs), so the engine's own allocator and libstdc++ are unaffected.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace l4d2vr_shims
{
    [[noreturn]] inline void Fatal(const char* what)
    {
        std::fprintf(stderr, "L4D2VR server: fatal C++ runtime error: %s\n", what ? what : "?");
        std::abort();
    }
}

namespace std
{
    void __throw_bad_alloc() { l4d2vr_shims::Fatal("bad_alloc"); }
    void __throw_bad_array_new_length() { l4d2vr_shims::Fatal("bad_array_new_length"); }
    void __throw_length_error(const char* what) { l4d2vr_shims::Fatal(what); }
    void __throw_logic_error(const char* what) { l4d2vr_shims::Fatal(what); }
    void __throw_out_of_range(const char* what) { l4d2vr_shims::Fatal(what); }
    void __throw_out_of_range_fmt(const char* what, ...) { l4d2vr_shims::Fatal(what); }
    void __throw_invalid_argument(const char* what) { l4d2vr_shims::Fatal(what); }
}

__attribute__((noinline)) void* operator new(std::size_t size)
{
    void* p = std::malloc(size ? size : 1u);
    if (!p) l4d2vr_shims::Fatal("out of memory");
    return p;
}

__attribute__((noinline)) void* operator new[](std::size_t size)
{
    return operator new(size);
}

__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

#pragma once
// Lets the std-only suites that include sdk/vector.h build with GCC/Clang.
#ifndef _MSC_VER
#ifndef __forceinline
#define __forceinline inline
#endif
#ifndef __declspec
#define __declspec(x)
#endif
#endif

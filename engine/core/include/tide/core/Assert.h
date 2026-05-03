#pragma once

#include "tide/core/Log.h"

#if defined(_MSC_VER)
#define TIDE_DEBUG_BREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
#if defined(__has_builtin) && __has_builtin(__builtin_debugtrap)
#define TIDE_DEBUG_BREAK() __builtin_debugtrap()
#else
#include <signal.h>
#define TIDE_DEBUG_BREAK() raise(SIGTRAP)
#endif
#else
#include <cstdlib>
#define TIDE_DEBUG_BREAK() std::abort()
#endif

#if defined(NDEBUG)
#define TIDE_ASSERT(expr, ...)                                                                     \
    do {                                                                                           \
        (void) sizeof(expr);                                                                       \
    } while (0)
#else
#define TIDE_ASSERT(expr, ...)                                                                     \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ::tide::log::engine().critical(                                                        \
                "Assertion failed: {} at {}:{} | " __VA_ARGS__, #expr, __FILE__, __LINE__          \
            );                                                                                     \
            TIDE_DEBUG_BREAK();                                                                    \
        }                                                                                          \
    } while (0)
#endif

#define TIDE_VERIFY(expr, ...)                                                                     \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ::tide::log::engine().critical(                                                        \
                "Verify failed: {} at {}:{} | " __VA_ARGS__, #expr, __FILE__, __LINE__             \
            );                                                                                     \
            TIDE_DEBUG_BREAK();                                                                    \
        }                                                                                          \
    } while (0)

#define TIDE_UNREACHABLE(...)                                                                      \
    do {                                                                                           \
        ::tide::log::engine().critical(                                                            \
            "Unreachable code at {}:{} | " __VA_ARGS__, __FILE__, __LINE__                         \
        );                                                                                         \
        TIDE_DEBUG_BREAK();                                                                        \
    } while (0)

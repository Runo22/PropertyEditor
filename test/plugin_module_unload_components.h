#pragma once

// Shared by test/plugin_module_unload_*.cpp: a component used by BOTH the host and
// the unloadable plugin. Per the TypeBridge "Plugin-unload rules", it is registered
// (RTTR + TypeBridge) only in the never-unloaded common module.

#include <cstdint>

#if defined(_WIN32)
#define RPE_TEST_EXPORT extern "C" __declspec(dllexport)
#define RPE_TEST_IMPORT extern "C" __declspec(dllimport)
#else
#define RPE_TEST_EXPORT extern "C" __attribute__((visibility("default")))
#define RPE_TEST_IMPORT extern "C"
#endif

struct SharedHealth
{
    int hp = 100;
};

#ifndef RPE_TEST_COMMON_BUILD
// Referenced by the host so the linker keeps the common module (--as-needed).
RPE_TEST_IMPORT int rpe_test_common_registered();
#endif

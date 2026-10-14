// scan_probe: built with SCAN BUILD + BATCH_SIZE (see test/CMakeLists.txt and
// scan_build_test.cmake). Only has to compile and link.
#include <reflect/Runtime.h>

#ifdef _WIN32
#define PROBE_API extern "C" __declspec(dllexport)
#else
#define PROBE_API extern "C" __attribute__((visibility("default")))
#endif

PROBE_API std::size_t probe_type_count()
{
    return reflect::type_count();
}

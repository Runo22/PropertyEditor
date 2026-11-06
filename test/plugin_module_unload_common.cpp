// The never-unloaded "common components" module: it owns SharedHealth's RTTR
// type_data because it registers it first, before any plugin is loaded.
#define RPE_TEST_COMMON_BUILD
#include "plugin_module_unload_components.h"

#include <rpe/core/TypeBridge.h>

#include <rttr/registration.h>

RTTR_REGISTRATION
{
    rttr::registration::class_<SharedHealth>("SharedHealth").property("hp", &SharedHealth::hp);
    rpe::TypeBridge::registerType<SharedHealth>();
}

RPE_TEST_EXPORT int rpe_test_common_registered()
{
    return rpe::TypeBridge::has(rttr::type::get<SharedHealth>()) ? 1 : 0;
}

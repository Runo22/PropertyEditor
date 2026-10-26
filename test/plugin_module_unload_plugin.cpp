// The plugin that is REALLY unloaded. It owns PluginOnly (registered here first)
// and merely uses SharedHealth, which the common module owns.
#include "plugin_module_unload_components.h"

#include <rpe/core/TypeBridge.h>

#include <rttr/registration.h>

struct PluginOnly
{
    float speed = 2.5f;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<PluginOnly>("PluginOnly").property("speed", &PluginOnly::speed);
}

// The plugin's explicit load / shutdown hooks — the host calls these around
// loading and BEFORE unloading. Bridge (un)registration lives here, not in a
// global/static destructor that would run during the unload itself.
RPE_TEST_EXPORT void rpe_test_plugin_load()
{
    rpe::TypeBridge::registerType<PluginOnly>();
}

RPE_TEST_EXPORT void rpe_test_plugin_shutdown()
{
    rpe::TypeBridge::unregisterType<PluginOnly>();
}

// The type ids as seen from INSIDE the plugin (its own type::get<T>() caches).
RPE_TEST_EXPORT std::uintptr_t rpe_test_plugin_shared_type_id()
{
    return rttr::type::get<SharedHealth>().get_id();
}

RPE_TEST_EXPORT std::uintptr_t rpe_test_plugin_own_type_id()
{
    return rttr::type::get<PluginOnly>().get_id();
}

// The plugin reads a shared component through the bridge (common module's hook).
RPE_TEST_EXPORT int rpe_test_plugin_read_shared_hp(void* obj)
{
    const rttr::type t = rttr::type::get<SharedHealth>();
    const rttr::variant ptr = rpe::TypeBridge::wrap(t, obj);
    return ptr.is_valid() ? t.get_property_value("hp", ptr).to_int() : -1;
}

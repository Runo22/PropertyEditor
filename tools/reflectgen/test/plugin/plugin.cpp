#include <reflect/Runtime.h>
#include "game/Components.h"

#ifdef _WIN32
#define PLUGIN_API extern "C" __declspec(dllexport)
#else
#define PLUGIN_API extern "C" __attribute__((visibility("default")))
#endif

PLUGIN_API void plugin_load(flecs::world* w)
{
    reflect::register_types();
    reflect::register_world(*w);
    w->entity("Lamp").set<game::Light>({ 42.f }).add<game::Selected>();
    w->entity("Orc").set<game::Stats>({}).set<game::inner::Stats>({ 3 });
}

PLUGIN_API void plugin_unload(flecs::world* w)
{
    reflect::unregister_world(*w);
    reflect::unregister_types();
}

PLUGIN_API std::size_t plugin_type_count() { return reflect::type_count(); }

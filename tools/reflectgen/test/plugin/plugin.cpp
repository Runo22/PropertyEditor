#include <reflect/Runtime.h>
#include "game/Components.h"
#include "game/More.h"
#include "game/NeedsPrelude.h"

#ifdef _WIN32
#define PLUGIN_API extern "C" __declspec(dllexport)
#else
#define PLUGIN_API extern "C" __attribute__((visibility("default")))
#endif

PLUGIN_API void plugin_load(flecs::world* w)
{
    reflect::register_types();
    reflect::register_world(*w);
    // A reload finds the entities still there; only a first load creates them.
    if (!w->lookup("Lamp"))
        w->entity("Lamp").set<game::Light>({ 42.f }).add<game::Selected>();
    if (!w->lookup("Orc"))
        w->entity("Orc").set<game::Stats>({}).set<game::inner::Stats>({ 3 });
}

PLUGIN_API void plugin_unload(flecs::world* w)
{
    reflect::unregister_world(*w);
    reflect::unregister_types();
}

// Hot reload: the host keeps the string while the plugin is unloaded.
PLUGIN_API void plugin_save(flecs::world* w, std::string* out, std::vector<std::string>* notSaved)
{
    *out = reflect::save_world(*w, notSaved);
}

PLUGIN_API std::size_t plugin_restore(flecs::world* w, const std::string* in, std::vector<std::string>* problems)
{
    return reflect::load_world(*w, *in, problems);
}

PLUGIN_API std::size_t plugin_type_count() { return reflect::type_count(); }

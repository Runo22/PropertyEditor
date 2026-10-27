// Plugin-unload rules (TypeBridge.h), end to end with a REAL module unload.
//
// test/plugin_unload_test.cpp only calls unregisterType — the plugin code never
// leaves memory. Here the plugin is an actual DLL/.so that gets loaded, unloaded
// and loaded again, so RTTR's per-module registration_manager really runs its
// destructor and frees the types that module owns. The layout follows the rules:
//   • SharedHealth — used by the host AND the plugin — is registered (RTTR +
//     TypeBridge) only in a never-unloaded common module, which owns its type_data;
//   • PluginOnly is owned by the plugin, and the plugin unregisters it from an
//     explicit shutdown hook before the host unloads it.
// After each unload the shared type must still be fully usable (is_wrapper(),
// name, properties, bridge lookup and wrap), and the plugin's own type must be
// gone from both RTTR and the bridge. Last, the rule broken on purpose: a plugin
// unloaded without its shutdown, then unregistered late — that call must not
// crash either.
#include "plugin_module_unload_components.h"

#include <rpe/core/TypeBridge.h>

#include <rttr/type>

#include <QDir>
#include <QLibrary>

#include <algorithm>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

// Is the module still mapped in the process? (QLibrary::isLoaded only reports its
// own reference.) Without this the test could pass while nothing was unloaded.
static bool moduleMapped(const QString& path)
{
#if defined(_WIN32)
    return GetModuleHandleW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16())) != nullptr;
#else
    void* h = dlopen(path.toLocal8Bit().constData(), RTLD_NOW | RTLD_NOLOAD);
    if (h)
        dlclose(h);
    return h != nullptr;
#endif
}

using LoadFn = void (*)();
using IdFn = std::uintptr_t (*)();
using ReadFn = int (*)(void*);

int main()
{
    const QString pluginPath = QStringLiteral(RPE_TEST_PLUGIN_PATH);

    check("common module registered SharedHealth before any plugin", rpe_test_common_registered() == 1);
    const rttr::type shared = rttr::type::get<SharedHealth>();

    for (int cycle = 1; cycle <= 2; ++cycle)
    {
        printf("── cycle %d: load / use / shutdown / unload ──\n", cycle);
        QLibrary lib(pluginPath);
        check("plugin loads", lib.load());
        if (!lib.isLoaded())
        {
            printf("  %s\n", qPrintable(lib.errorString()));
            break;
        }
        const auto load = reinterpret_cast<LoadFn>(lib.resolve("rpe_test_plugin_load"));
        const auto shutdown = reinterpret_cast<LoadFn>(lib.resolve("rpe_test_plugin_shutdown"));
        const auto sharedId = reinterpret_cast<IdFn>(lib.resolve("rpe_test_plugin_shared_type_id"));
        const auto ownId = reinterpret_cast<IdFn>(lib.resolve("rpe_test_plugin_own_type_id"));
        const auto readHp = reinterpret_cast<ReadFn>(lib.resolve("rpe_test_plugin_read_shared_hp"));
        check("plugin exports its hooks", load && shutdown && sharedId && ownId && readHp);
        if (!(load && shutdown && sharedId && ownId && readHp))
            break;

        load();

        // The plugin sees the SAME type_data the common module owns — it does not
        // own SharedHealth, so unloading it can't free it.
        check("plugin's type::get<SharedHealth>() is the common module's type", sharedId() == shared.get_id());

        const rttr::type own = rttr::type::get_by_name("PluginOnly");
        check("PluginOnly is registered in RTTR while the plugin is loaded",
              own.is_valid() && own.get_id() == ownId());
        check("PluginOnly is bridged while the plugin is loaded", rpe::TypeBridge::has(own));
        check("PluginOnly resolves by name while the plugin is loaded",
              rpe::TypeBridge::resolveByName("PluginOnly") == own);

        SharedHealth h { 70 + cycle };
        check("plugin reads a shared component through the bridge", readHp(&h) == 70 + cycle);

        // Rule 3: explicit shutdown BEFORE the unload, never a static destructor.
        shutdown();
        check("plugin shutdown removed PluginOnly from the bridge", !rpe::TypeBridge::has(own));

        check("plugin unloads", lib.unload());
        check("plugin module is really gone from the process", !moduleMapped(pluginPath));

        // RTTR freed the plugin-owned type with the module: this is exactly why a
        // type another module uses must not be owned by an unloadable plugin.
        check("RTTR dropped the plugin-owned type on unload",
              !rttr::type::get_by_name("PluginOnly").is_valid());

        // `own` now points at freed type_data. unregisterType must only compare ids
        // (a second shutdown, or one from a late destructor, must not crash).
        rpe::TypeBridge::unregisterType(own);
        const auto types = rpe::TypeBridge::registeredTypes();
        check("no bridge entry left for the plugin-owned type",
              std::none_of(types.begin(), types.end(), [&](const rttr::type& t) { return t.get_id() == own.get_id(); }));

        // The shared type survived the unload untouched.
        check("shared type still valid", shared.is_valid());
        check("shared type: is_wrapper() is safe and false", !shared.is_wrapper());
        check("shared type keeps its name", shared.get_name() == "SharedHealth");
        check("shared type keeps its properties", shared.get_properties().size() == 1);
        check("shared type still bridged", rpe::TypeBridge::has(shared));
        check("shared type still resolves by name", rpe::TypeBridge::resolveByName("SharedHealth") == shared);
        {
            SharedHealth s { 5 * cycle };
            const rttr::variant ptr = rpe::TypeBridge::wrap(shared, &s);
            check("shared value still readable through the bridge",
                  ptr.is_valid() && shared.get_property_value("hp", ptr).to_int() == 5 * cycle);
        }
    }

    // ── The rule broken: unloaded WITHOUT its shutdown, then a late unregister ───
    // The type is still bridged when its module goes. A late unregisterType must
    // still not crash: it sees RTTR no longer has the type and skips the release
    // (reading the freed type_data to release anything is exactly what would crash).
    printf("── late unregister: unloaded without shutdown ──\n");
    {
        QLibrary lib(pluginPath);
        check("plugin loads again", lib.load());
        const auto load = reinterpret_cast<LoadFn>(lib.resolve("rpe_test_plugin_load"));
        if (load)
        {
            load();
            const rttr::type own = rttr::type::get_by_name("PluginOnly");
            check("PluginOnly bridged", rpe::TypeBridge::has(own));
            check("unloaded without its shutdown", lib.unload() && !moduleMapped(pluginPath));
            rpe::TypeBridge::unregisterType(own); // must not touch the freed type_data
            const auto types = rpe::TypeBridge::registeredTypes();
            check("a late unregisterType after the unload is safe and removes the entry",
                  std::none_of(types.begin(), types.end(), [&](const rttr::type& t) { return t.get_id() == own.get_id(); }));
            check("the shared type is untouched", rpe::TypeBridge::has(shared) && shared.get_name() == "SharedHealth");
        }
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

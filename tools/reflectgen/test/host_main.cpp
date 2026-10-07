// Host side of the plugin test: load → inspect → unload → reload.
#include <rpe/core/TypeBridge.h>
#include <rttr/type>
#include <flecs.h>
#include <rpe/core/EditorHints.h>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
static void* openLib(const char* p) { return LoadLibraryA(p); }
static void* sym(void* h, const char* n) { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), n)); }
static bool closeLib(void* h) { return FreeLibrary(static_cast<HMODULE>(h)) != 0; }
static const char* libError() { return "LoadLibrary failed"; }
#else
#include <dlfcn.h>
static void* openLib(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void* sym(void* h, const char* n) { return dlsym(h, n); }
static bool closeLib(void* h) { return dlclose(h) == 0; }
static const char* libError() { return dlerror(); }
#endif
#include <string>

static int fails = 0;
static void check(const char* what, bool ok) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what); fails += !ok; }

struct Plugin
{
    void* h = nullptr;
    void (*load)(flecs::world*) = nullptr;
    void (*unload)(flecs::world*) = nullptr;
    std::size_t (*count)() = nullptr;
    bool open(const char* path)
    {
        h = openLib(path);
        if (!h) { std::printf("load: %s\n", libError()); return false; }
        load = reinterpret_cast<void (*)(flecs::world*)>(sym(h, "plugin_load"));
        unload = reinterpret_cast<void (*)(flecs::world*)>(sym(h, "plugin_unload"));
        count = reinterpret_cast<std::size_t (*)()>(sym(h, "plugin_type_count"));
        return load && unload && count;
    }
};

static double meta(const rttr::type& t, const char* prop, const char* key)
{
    return t.get_property(prop).get_metadata(std::string(key)).to_double(); // as rpe reads it
}

int main(int, char** argv)
{
    flecs::world world;
    for (int round = 1; round <= 2; ++round)
    {
        std::printf("── round %d\n", round);
        Plugin p;
        check("plugin loads", p.open(argv[1]));
        check("7 reflected types linked in the plugin's own list", p.count() == 7);
        p.load(&world);

        rttr::type light = rttr::type::get_by_name("game::Light");
        check("RTTR knows game::Light", light.is_valid());
        check("intensity min/max/step", meta(light, "intensity", "rpe.min") == 0 && meta(light, "intensity", "rpe.max") == 100 && meta(light, "intensity", "rpe.step") == 0.5);
        check("intensity uses slider", light.get_property("intensity").get_metadata(std::string("rpe.editor")).to_string() == "slider");
        check("speed max = kMaxSpeed", meta(light, "speed", "rpe.max") == 250.0);
        check("iconPath label", light.get_property("iconPath").get_metadata(std::string("rpe.label")).to_string() == "Icon");
        check("damage gets flags hint automatically", light.get_property("damage").get_metadata(std::string("rpe.flags")).to_bool());
        // Why generated code uses std::string keys: the old const char* form only
        // matches when the key pointer comes from the same module.
        std::printf("    info: lookup with this module's rpe::hint::Min pointer %s\n",
                    light.get_property("intensity").get_metadata(rpe::hint::Min).is_valid() ? "found it" : "does NOT find it");
        check("non-PROP field 'cache' not registered", !light.get_property("cache").is_valid());

        rttr::type stats = rttr::type::get_by_name("game::Stats");
        check("all-mode: hp, shield, inventory registered", stats.get_property("hp").is_valid() && stats.get_property("shield").is_valid() && stats.get_property("inventory").is_valid());
        check("all-mode: NOPROP debugCounter skipped", !stats.get_property("debugCounter").is_valid());
        check("private secret via REFLECT_FRIEND, readonly", stats.get_property("secret").get_metadata(std::string("rpe.readOnly")).to_bool());

        rttr::type shape = rttr::type::get_by_name("game::Shape");
        check("enum Shape: NOPROP Count skipped", shape.is_enumeration() && shape.get_enumeration().get_names().size() == 2);

        check("rpe resolves flecs name game.Light", rpe::TypeBridge::resolveByName("game.Light") == light);
        check("rpe resolves custom flecs name ai.Stats", rpe::TypeBridge::resolveByName("ai.Stats") == stats);
        check("rpe resolves game.inner.Stats separately", rpe::TypeBridge::resolveByName("game.inner.Stats") == rttr::type::get_by_name("game::inner::Stats"));

        flecs::entity lamp = world.lookup("Lamp");
        flecs::entity lightComp = world.lookup("game::Light");
        check("flecs component game.Light exists", lightComp.is_valid());
        check("flecs custom-named ai.Stats exists", world.lookup("ai::Stats").is_valid());
        check("Lamp has Light", lamp.has(lightComp));
        {
            const void* ptr = lamp.get(lightComp);
            rttr::variant v = rpe::TypeBridge::wrap(light, const_cast<void*>(ptr));
            check("editor reads intensity = 42 through the bridge", light.get_property("intensity").get_value(v).to_float() == 42.f);
        }
#ifdef FLECS_META
        check("flecs meta: Light has members", lightComp.has<flecs::Struct>());
        char* json = ecs_ptr_to_json(world.c_ptr(), lightComp, lamp.get(lightComp));
        std::printf("    flecs json: %s\n", json ? json : "(null)");
        ecs_os_free(json);
#endif

        p.unload(&world);
        check("after unload: flecs component deleted", !world.lookup("game::Light").is_valid() && !world.lookup("ai::Stats").is_valid());
        check("after unload: Lamp kept, only its name left", lamp.is_alive() && std::string(lamp.type().str().c_str()) == "(Identifier,Name)");
        check("after unload: rpe no longer resolves game.Light", !rpe::TypeBridge::resolveByName("game.Light").is_valid());
        check("library unloaded", closeLib(p.h));
        check("after unload: RTTR forgot game::Light", !rttr::type::get_by_name("game::Light").is_valid());
        world.progress();
    }
    std::printf("%s (%d failure(s))\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}

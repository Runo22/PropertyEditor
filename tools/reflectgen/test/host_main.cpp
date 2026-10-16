// Host side of the plugin test: load → inspect → hot reload v1 → v2 → v1.
// argv[1] = game_plugin (v1), argv[2] = game_plugin_v2 (Light gains `extra`,
// `speed` becomes float). Component values the host changed must survive each
// reload through reflect::save_world / load_world.
#include <reflect/Runtime.h> // register_std_types: the host owns the shared std types
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
#include <vector>

static int fails = 0;
static void check(const char* what, bool ok) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what); fails += !ok; }

struct Plugin
{
    void* h = nullptr;
    void (*load)(flecs::world*) = nullptr;
    void (*unload)(flecs::world*) = nullptr;
    std::size_t (*count)() = nullptr;
    void (*save)(flecs::world*, std::string*, std::vector<std::string>*) = nullptr;
    std::size_t (*restore)(flecs::world*, const std::string*, std::vector<std::string>*) = nullptr;
    bool open(const char* path)
    {
        h = openLib(path);
        if (!h) { std::printf("load: %s\n", libError()); return false; }
        load = reinterpret_cast<void (*)(flecs::world*)>(sym(h, "plugin_load"));
        unload = reinterpret_cast<void (*)(flecs::world*)>(sym(h, "plugin_unload"));
        count = reinterpret_cast<std::size_t (*)()>(sym(h, "plugin_type_count"));
        save = reinterpret_cast<decltype(save)>(sym(h, "plugin_save"));
        restore = reinterpret_cast<decltype(restore)>(sym(h, "plugin_restore"));
        return load && unload && count && save && restore;
    }
};

// A metadata string as rpe's metaString reads it (const char* or std::string).
static std::string metaText(const rttr::variant& m)
{
    if (m.is_type<const char*>())
        return m.get_value<const char*>();
    return m.to_string();
}

static double meta(const rttr::type& t, const char* prop, rpe::hint::Key key)
{
    return t.get_property(prop).get_metadata(key).to_double();
}

// Read / write a component member through the editor's path (bridge + RTTR).
static rttr::variant getProp(flecs::world& w, flecs::entity e, const char* comp, const char* type, const char* prop)
{
    rttr::type t = rttr::type::get_by_name(type);
    flecs::entity c = w.lookup(comp);
    if (!t.is_valid() || !c || !e.has(c))
        return {};
    rttr::variant v = rpe::TypeBridge::wrap(t, ecs_get_mut_id(w.c_ptr(), e, c));
    return t.get_property(prop).get_value(v);
}
static bool setProp(flecs::world& w, flecs::entity e, const char* comp, const char* type, const char* prop,
                    rttr::variant value)
{
    rttr::type t = rttr::type::get_by_name(type);
    flecs::entity c = w.lookup(comp);
    if (!t.is_valid() || !c || !e.has(c))
        return false;
    rttr::variant v = rpe::TypeBridge::wrap(t, ecs_get_mut_id(w.c_ptr(), e, c));
    const bool ok = t.get_property(prop).set_value(v, value);
    ecs_modified_id(w.c_ptr(), e, c);
    return ok;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: host <game_plugin> <game_plugin_v2>\n");
        return 2;
    }
    flecs::world world;
    // std::string / std::vector descriptions belong to a module that never
    // unloads: the host, before any plugin.
    reflect::register_std_types(world);
    std::string snapshot; // what the host keeps while a plugin build is unloaded
    const char* builds[] = { argv[1], argv[2], argv[1] };
    for (int round = 1; round <= 3; ++round)
    {
        const bool v2 = round == 2;
        std::printf("── round %d (%s)\n", round, v2 ? "v2" : "v1");
        Plugin p;
        check("plugin loads", p.open(builds[round - 1]));
        check("12 reflected types linked in the plugin's own list", p.count() == 12);
        p.load(&world);
        if (!snapshot.empty())
        {
            std::vector<std::string> problems;
            const std::size_t n = p.restore(&world, &snapshot, &problems);
            for (const auto& m : problems)
                std::printf("    restore note: %s\n", m.c_str());
            check("hot reload: snapshot restored without problems", n > 0 && problems.empty());
        }

        rttr::type light = rttr::type::get_by_name("game::Light");
        check("RTTR knows game::Light", light.is_valid());
        check("intensity min/max/step", meta(light, "intensity", rpe::hint::Min) == 0 && meta(light, "intensity", rpe::hint::Max) == 100 && meta(light, "intensity", rpe::hint::Step) == 0.5);
        check("intensity uses slider", metaText(light.get_property("intensity").get_metadata(rpe::hint::Editor)) == "slider");
        check("speed max = kMaxSpeed", meta(light, "speed", rpe::hint::Max) == 250.0);
        check("iconPath label", metaText(light.get_property("iconPath").get_metadata(rpe::hint::Label)) == "Icon");
        check("damage gets flags hint automatically", light.get_property("damage").get_metadata(rpe::hint::Flags).to_bool());
        check("non-PROP field 'cache' not registered", !light.get_property("cache").is_valid());

        rttr::type stats = rttr::type::get_by_name("game::Stats");
        check("all-mode: hp, shield, inventory registered", stats.get_property("hp").is_valid() && stats.get_property("shield").is_valid() && stats.get_property("inventory").is_valid());
        check("all-mode: NOPROP debugCounter skipped", !stats.get_property("debugCounter").is_valid());
        check("private secret via REFLECT_FRIEND, readonly", stats.get_property("secret").get_metadata(rpe::hint::ReadOnly).to_bool());

        check("second header (unity TU) registered: game::Velocity", rttr::type::get_by_name("game::Velocity").get_property("dx").is_valid());

        check("non-self-contained header parsed with the target's options: game::Named",
              metaText(rttr::type::get_by_name("game::Named").get_property("name").get_metadata(rpe::hint::Label)) == "Name");

        {
            // Template alias, base fields, accessors (Advanced.h).
            rttr::type vec2 = rttr::type::get_by_name("game::Vec2f");
            check("template alias game::Vec2f: x, y registered, notProp not",
                  vec2.get_property("x").is_valid() && vec2.get_property("y").is_valid()
                      && !vec2.get_property("notProp").is_valid() && meta(vec2, "x", rpe::hint::Step) == 0.5);
            rttr::type unit = rttr::type::get_by_name("game::Unit");
            check("base fields on Unit: hp (public) and armor (protected, via REFLECT_FRIEND)",
                  unit.get_property("hp").is_valid() && unit.get_property("armor").is_valid()
                      && meta(unit, "hp", rpe::hint::Min) == 0);
            check("accessor: power is read/write, label is read-only",
                  unit.get_property("power").is_valid() && !unit.get_property("power").is_readonly()
                      && unit.get_property("label").is_readonly() && meta(unit, "power", rpe::hint::Max) == 100);
            // Through a flecs entity, the way the editor writes values.
            flecs::entity e = world.entity();
            e.add(world.lookup("game::Unit"));
            const bool wrote = setProp(world, e, "game::Unit", "game::Unit", "power", 42.f)
                               && setProp(world, e, "game::Unit", "game::Unit", "hp", 7);
            check("accessor setter and base field write through the editor path",
                  wrote && getProp(world, e, "game::Unit", "game::Unit", "power").to_float() == 42.f
                      && getProp(world, e, "game::Unit", "game::Unit", "hp").to_int() == 7
                      && getProp(world, e, "game::Unit", "game::Unit", "armor").to_int() == 2
                      && getProp(world, e, "game::Unit", "game::Unit", "label").to_string() == "unit");
            {
                flecs::entity uc = world.lookup("game::Unit");
                char* json = ecs_ptr_to_json(world.c_ptr(), uc, ecs_get_id(world.c_ptr(), e, uc));
                const std::string j = json ? json : "";
                ecs_os_free(json);
                check("flecs meta of Unit: base members and the alias-typed member",
                      j.find("\"hp\":7") != std::string::npos && j.find("\"armor\":2") != std::string::npos
                          && j.find("\"pos\":{\"x\":0, \"y\":0}") != std::string::npos);
                if (round == 1)
                    std::printf("    Unit json: %s\n", j.c_str());
            }
            e.destruct();
        }

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
        flecs::entity orc = world.lookup("Orc");
        flecs::entity selected = world.lookup("game::Selected");
        if (round == 1)
        {
            check("editor reads intensity = 42 through the bridge",
                  getProp(world, lamp, "game::Light", "game::Light", "intensity").to_float() == 42.f);
            // The host edits values and moves a tag; all of it must survive reloads.
            check("edit intensity → 77", setProp(world, lamp, "game::Light", "game::Light", "intensity", 77.f));
            check("edit speed → 33.5", setProp(world, lamp, "game::Light", "game::Light", "speed", 33.5));
            check("edit Orc hp → 55", setProp(world, orc, "ai::Stats", "game::Stats", "hp", 55));
            check("edit iconPath (std::string)",
                  setProp(world, lamp, "game::Light", "game::Light", "iconPath", std::string("icons/lamp.png")));
            {
                // std::vector members, written through flecs JSON like a level file would.
                flecs::entity soldier = world.entity("Soldier");
                flecs::entity unitComp = world.lookup("game::Unit");
                soldier.add(unitComp);
                const char* r1 = ecs_ptr_from_json(world.c_ptr(), unitComp, ecs_get_mut_id(world.c_ptr(), soldier, unitComp),
                                                   "{\"path\":[{\"x\":1, \"y\":2}, {\"x\":3, \"y\":4}]}", nullptr);
                ecs_modified_id(world.c_ptr(), soldier, unitComp);
                flecs::entity statsComp = world.lookup("ai::Stats");
                const char* r2 = ecs_ptr_from_json(world.c_ptr(), statsComp, ecs_get_mut_id(world.c_ptr(), orc, statsComp),
                                                   "{\"inventory\":[5, 6, 7]}", nullptr);
                ecs_modified_id(world.c_ptr(), orc, statsComp);
                check("flecs JSON writes std::vector<Vec2f> (module-owned) and std::vector<int> (host)", r1 && r2);
            }
            lamp.remove(selected);
            orc.add(selected);
        }
        else
        {
            check("hot reload: intensity is still 77",
                  getProp(world, lamp, "game::Light", "game::Light", "intensity").to_float() == 77.f);
            check("hot reload: speed is still 33.5 (double ↔ float across builds)",
                  getProp(world, lamp, "game::Light", "game::Light", "speed").to_double() == 33.5);
            check("hot reload: Orc hp is still 55", getProp(world, orc, "ai::Stats", "game::Stats", "hp").to_int() == 55);
            check("hot reload: the Selected tag moved to Orc stays there", orc.has(selected) && !lamp.has(selected));
            check("hot reload: iconPath (std::string) is still set",
                  getProp(world, lamp, "game::Light", "game::Light", "iconPath").to_string() == "icons/lamp.png");
            {
                flecs::entity soldier = world.lookup("Soldier");
                flecs::entity unitComp = world.lookup("game::Unit");
                flecs::entity statsComp = world.lookup("ai::Stats");
                auto json = [&](flecs::entity e, flecs::entity c) {
                    char* j = (e && c && e.has(c)) ? ecs_ptr_to_json(world.c_ptr(), c, ecs_get_id(world.c_ptr(), e, c)) : nullptr;
                    std::string out = j ? j : "";
                    ecs_os_free(j);
                    return out;
                };
                const std::string u = json(soldier, unitComp), st = json(orc, statsComp);
                check("hot reload: std::vector<Vec2f> path survived",
                      u.find("\"path\":[{\"x\":1, \"y\":2}, {\"x\":3, \"y\":4}]") != std::string::npos);
                check("hot reload: std::vector<int> inventory survived", st.find("\"inventory\":[5, 6, 7]") != std::string::npos);
                if (round == 2)
                    std::printf("    Soldier: %s\n    Orc stats: %s\n", u.c_str(), st.c_str());
            }
            if (v2)
                check("hot reload: v2's new member `extra` has its default 5",
                      getProp(world, lamp, "game::Light", "game::Light", "extra").to_int() == 5);
            else
                check("hot reload: back on v1, `extra` is gone", !light.get_property("extra").is_valid());
        }
#ifdef FLECS_META
        check("flecs meta: Light has members", lightComp.has<flecs::Struct>());
        char* json = ecs_ptr_to_json(world.c_ptr(), lightComp, lamp.get(lightComp));
        std::printf("    flecs json: %s\n", json ? json : "(null)");
        ecs_os_free(json);
#endif

        {
            std::vector<std::string> notSaved;
            p.save(&world, &snapshot, &notSaved);
            for (const auto& m : notSaved)
                std::printf("    not saved: %s\n", m.c_str());
            check("save_world: every component of the module saved", notSaved.empty());
        }
        p.unload(&world);
        check("after unload: flecs component deleted", !world.lookup("game::Light").is_valid() && !world.lookup("ai::Stats").is_valid());
        check("after unload: Lamp kept, only its name left", lamp.is_alive() && std::string(lamp.type().str().c_str()) == "(Identifier,Name)");
        check("after unload: rpe no longer resolves game.Light", !rpe::TypeBridge::resolveByName("game.Light").is_valid());
        check("library unloaded", closeLib(p.h));
        check("after unload: RTTR forgot game::Light", !rttr::type::get_by_name("game::Light").is_valid());
        world.progress();
    }
    {
        // Plugins only USE the host's std types: one std::vector<int> component in
        // the world, however often plugins were loaded; and the module-owned
        // std::vector<Vec2f> left with the plugin.
        int vecInt = 0, vecVec2 = 0;
        world.each([&](flecs::entity c, flecs::Component&) {
            const char* sym = ecs_get_symbol(world.c_ptr(), c);
            const std::string s = sym ? sym : "";
            if (s.rfind("std.vector<int", 0) == 0)
                ++vecInt;
            if (s.find("vector<game.Vec2T") != std::string::npos)
                ++vecVec2;
        });
        check("one std::vector<int> component in the world (host's)", vecInt == 1);
        check("module-owned std::vector<Vec2f> removed with the plugin", vecVec2 == 0);
    }
    std::printf("%s (%d failure(s))\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}

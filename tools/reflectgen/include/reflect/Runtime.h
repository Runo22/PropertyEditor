#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  Runtime.h — included by the files reflectgen generates. Collects every
//  reflected type of ONE module (the exe or one DLL) and registers / unregisters
//  them with RTTR, the rpe property editor (TypeBridge) and flecs.
//
//  The type list is PER MODULE: nothing here is exported, so on Windows each DLL
//  that includes this header gets its own copy (and on ELF it is forced hidden).
//  Call these from the module that owns the types — i.e. from the plugin's own
//  load/unload entry points, never from the host on the plugin's behalf.
//
//  Plugin lifecycle (see README for the full rules):
//
//      LoadLibrary
//      reflect::register_types();          // RTTR + rpe bridges (process level)
//      reflect::register_world(world);     // flecs components   (per world, sim thread)
//      ...
//      std::string keep = reflect::save_world(world); // (hot reload) component data
//      reflect::unregister_world(world);   // remove + delete components (sim thread,
//                                          //   outside world.progress())
//      reflect::unregister_types();        // drop rpe bridges (GUI stops inspecting)
//      FreeLibrary                          // RTTR undoes this module's registrations
//      ... rebuild, LoadLibrary, register_types(), register_world(world) ...
//      reflect::load_world(world, keep);   // the values are back, by member name
// ─────────────────────────────────────────────────────────────────────────────

#include "reflect/Reflect.h"

// Standard headers first: RTTR 0.9.6's variant.h uses std::string without
// including <string>, which recent MSVC STLs no longer pull in transitively.
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>
#include <type_traits>
#include <vector>

#include <rttr/registration.h>

#if !defined(REFLECT_NO_RPE) && __has_include(<rpe/core/TypeBridge.h>)
#define REFLECT_WITH_RPE 1
#include <rpe/core/FlagsSupport.h>
#include <rpe/core/OptionalSupport.h>
#include <rpe/core/TypeBridge.h>
#include <rpe/core/EditorHints.h>
#else
#define REFLECT_WITH_RPE 0
#endif

#if !defined(REFLECT_NO_FLECS) && __has_include(<flecs.h>)
#define REFLECT_WITH_FLECS 1
#include <flecs.h>
#if REFLECT_WITH_RPE
#include <rpe/ecs/ComponentRegistry.h>
#endif
#else
#define REFLECT_WITH_FLECS 0
#endif

#if defined(__GNUC__) && !defined(_WIN32)
#pragma GCC visibility push(hidden) // keep every symbol below module-local on ELF
#endif

namespace reflect
{

    struct TypeEntry
    {
        const char* name;
        void (*rttr)();     // RTTR registration (once per module load)
        void (*bridge)();   // rpe bridges, may be null
        void (*unbridge)(); // undo bridge(), may be null
#if REFLECT_WITH_FLECS
        void (*flecs)(flecs::world&);   // may be null (not a component)
        void (*unflecs)(flecs::world&); // may be null
        bool (*flecsReady)(flecs::world&); // null = no dependencies; else true once
                                           //   the meta types it embeds are described
        flecs::entity_t (*flecsId)(flecs::world&); // its component in a world, or 0
#endif
        TypeEntry* next = nullptr;
    };

    // &Derived::baseField has the type `M Base::*`; RTTR and flecs need it as a
    // member of the class being registered: `M Derived::*` (same offset).
    template <class D, class M, class B>
    constexpr M D::*as_member(M B::*p) noexcept
    {
        return static_cast<M D::*>(p);
    }

    namespace detail
    {
        inline TypeEntry*& head()
        {
            static TypeEntry* h = nullptr;
            return h;
        }
        inline TypeEntry*& tail()
        {
            static TypeEntry* t = nullptr;
            return t;
        }

        // Generated files link their entries in during static initialisation of
        // THIS module (the .gen.cpp files are compiled into it, not a static lib).
        // Appended, so types keep their declaration order within a header.
        struct AutoLink
        {
            explicit AutoLink(TypeEntry& e)
            {
                (head() ? tail()->next : head()) = &e;
                tail() = &e;
            }
        };

        inline bool& rttrDone()
        {
            static bool done = false;
            return done;
        }

#if REFLECT_WITH_FLECS
        // flecs types this module registered beyond its components (vectors of
        // its own types); removed by unregister_world like the components.
        inline std::vector<void (*)(flecs::world&)>& ownedFlecsTypes()
        {
            static std::vector<void (*)(flecs::world&)> v;
            return v;
        }

        // Once per module and type: a member skipped because the host has not
        // described a std type to flecs.
        inline void missingStdType(const char* type, const char* member)
        {
            static std::set<std::string> told;
            if (told.insert(type).second)
                std::fprintf(stderr,
                             "reflect: %s is not described to flecs, so members like %s are left out of flecs "
                             "meta (and of save_world). Call reflect::register_std_types(world) in the host.\n",
                             type, member);
        }
#endif
    } // namespace detail

    // RTTR + rpe bridges for every type of this module. Safe to call again: RTTR is
    // registered once per load (FreeLibrary undoes it), bridges are idempotent.
    inline void register_types()
    {
        const bool doRttr = !detail::rttrDone();
        detail::rttrDone() = true;
        for (TypeEntry* e = detail::head(); e; e = e->next)
        {
            if (doRttr)
            {
                e->rttr();
            }
            if (e->bridge)
            {
                e->bridge();
            }
        }
    }

    // Remove this module's types from the property editor. Call BEFORE unloading
    // (and let the GUI/mirror drop cached values) — the editor must not call into
    // the module after it is gone.
    inline void unregister_types()
    {
        for (TypeEntry* e = detail::head(); e; e = e->next)
        {
            if (e->unbridge)
            {
                e->unbridge();
            }
        }
    }

    inline std::size_t type_count()
    {
        std::size_t n = 0;
        for (TypeEntry* e = detail::head(); e; e = e->next)
        {
            ++n;
        }
        return n;
    }

#if REFLECT_WITH_FLECS

    // Register T with flecs (optionally under an explicit C++-style path such as
    // "render::Stats"). For an inspectable component (`bind`), also point the rpe
    // alias at the path flecs actually uses, so the editor resolves it exactly.
    template <class T>
    flecs::component<T> flecs_register(flecs::world& w, const char* name, bool bind)
    {
        flecs::component<T> c = name ? w.component<T>(name) : w.component<T>();
#if REFLECT_WITH_RPE
        if constexpr (!std::is_empty_v<T> && !std::is_enum_v<T>)
        {
            if (bind)
            {
                rpe::bindComponent<T>(c);
            }
        }
#else
        (void)bind;
#endif
        return c;
    }

    // The component entity flecs has for T in `w`, or 0. Looked up by the C++
    // symbol, so it also finds a type that ANOTHER module registered (the per-module
    // flecs::_::type<T> id cache would not).
    template <class T>
    flecs::entity_t flecs_find(flecs::world& w)
    {
        char* symbol = ecs_cpp_get_symbol_name(nullptr, flecs::_::type_name<T>(), 0); // "game.Light"
        const flecs::entity_t id = ecs_lookup_symbol(w.c_ptr(), symbol, false, false);
        ecs_os_free(symbol);
        return id;
    }

    // True once T's members are described to flecs (meta). Generated `flecsReady`
    // checks use it so a struct is described only after the structs it embeds.
    template <class T>
    bool flecs_has_members(flecs::world& w)
    {
#ifdef FLECS_META
        const flecs::entity_t id = flecs_find<T>(w);
        return id != 0 && ecs_has_id(w.c_ptr(), id, ecs_id(EcsStruct));
#else
        (void)w;
        return true;
#endif
    }

    // True when T's flecs meta (members) has not been described yet — so a second
    // register_world() call does not add the members twice.
    inline bool flecs_needs_members(flecs::entity c)
    {
#ifdef FLECS_META
        return !c.has<flecs::Struct>();
#else
        (void)c;
        return false;
#endif
    }

    // Strip T from every entity and delete the component. Afterwards no flecs data
    // or hook (ctor/dtor/move — code inside this module) refers to T any more.
    // World must not be in progress()/deferred when this runs.
    template <class T>
    void flecs_unregister(flecs::world& w)
    {
        const flecs::entity_t id = flecs_find<T>(w);
        if (id == 0 || !ecs_is_alive(w.c_ptr(), id))
        {
            return; // never registered in this world, or already removed
        }
        ecs_remove_all(w.c_ptr(), id);
        ecs_delete(w.c_ptr(), id);
    }

#ifdef FLECS_META

    // T is known to flecs with a description (meta struct, enum, opaque, ...).
    template <class T>
    bool flecs_has_type(flecs::world& w)
    {
        const flecs::entity_t id = flecs_find<T>(w);
        return id != 0 && ecs_has_id(w.c_ptr(), id, ecs_id(EcsType));
    }

    namespace detail
    {
        // flecs opaque support for std::vector<E> (as in flecs' ser_std_vector).
        template <class E>
        flecs::opaque<std::vector<E>, E> stdVectorSupport(flecs::world& w)
        {
            using V = std::vector<E>;
            return flecs::opaque<V, E>()
                .as_type(w.vector<E>())
                .serialize([](const flecs::serializer* s, const V* data) {
                    for (const auto& el : *data)
                        s->value(el);
                    return 0;
                })
                .count([](const V* data) { return data->size(); })
                .resize([](V* data, size_t size) { data->resize(size); })
                .ensure_element([](V* data, size_t i) {
                    if (data->size() <= i)
                        data->resize(i + 1);
                    return &(*data)[i];
                });
        }
    } // namespace detail

    // ── std types in flecs meta ───────────────────────────────────────────────────
    //
    // flecs describes std::string / std::vector through "opaque" callbacks, and a
    // component's ctor/dtor/move hooks are code too — all of it lives in whichever
    // module registers the type first. Shared std types must therefore be
    // registered by a module that never unloads: call register_std_types() from
    // the HOST, before loading plugins. Generated plugin code only USES them (a
    // member whose std type the host has not registered is left out of flecs meta
    // with a warning, never registered with the plugin's code).
    //
    // They are found by a name chosen here ("reflect::std::vector<i32>"), not by
    // flecs' C++ type name: compilers spell the same instantiation differently in
    // different modules (GCC: "std::vector<int>" here, "std::vector<int,
    // std::allocator<int> >" there), so a plugin would not find the host's type
    // and flecs would register a second one with the plugin's hooks.

    namespace detail
    {
        template <class E>
        std::string stdElemName()
        {
            if constexpr (std::is_same_v<E, std::string>)
                return "string";
            else if constexpr (std::is_floating_point_v<E>)
                return sizeof(E) == 4 ? "f32" : "f64";
            else
            {
                static_assert(std::is_integral_v<E> && !std::is_same_v<E, bool>,
                              "shared std::vector elements: integers, floating point, std::string");
                return (std::is_signed_v<E> ? "i" : "u") + std::to_string(sizeof(E) * 8);
            }
        }

        template <class T>
        struct StdName;
        template <>
        struct StdName<std::string>
        {
            static std::string get() { return "reflect::std::string"; }
        };
        template <class E>
        struct StdName<std::vector<E>>
        {
            static std::string get() { return "reflect::std::vector<" + stdElemName<E>() + ">"; }
        };
    } // namespace detail

    // Register one more shared std::vector<E> from the host (E: an integer type,
    // float, double or std::string; register_std_types does all of them).
    template <class E>
    void register_std_vector(flecs::world& w)
    {
        const std::string name = detail::StdName<std::vector<E>>::get();
        if (!ecs_lookup_path_w_sep(w.c_ptr(), 0, name.c_str(), "::", "::", false))
            w.component<std::vector<E>>(name.c_str()).opaque(detail::stdVectorSupport<E>);
    }

    inline void register_std_types(flecs::world& w)
    {
        const std::string name = detail::StdName<std::string>::get();
        if (!ecs_lookup_path_w_sep(w.c_ptr(), 0, name.c_str(), "::", "::", false))
        {
            w.component<std::string>(name.c_str())
                .opaque(flecs::String)
                .serialize([](const flecs::serializer* s, const std::string* data) {
                    const char* str = data->c_str();
                    return s->value(flecs::String, &str);
                })
                .assign_string([](std::string* data, const char* value) { *data = value ? value : ""; });
        }
        register_std_vector<std::int8_t>(w);
        register_std_vector<std::int16_t>(w);
        register_std_vector<std::int32_t>(w);
        register_std_vector<std::int64_t>(w);
        register_std_vector<std::uint8_t>(w);
        register_std_vector<std::uint16_t>(w);
        register_std_vector<std::uint32_t>(w);
        register_std_vector<std::uint64_t>(w);
        register_std_vector<float>(w);
        register_std_vector<double>(w);
        register_std_vector<std::string>(w);
    }

    // Use the host's flecs type for a shared std type T in THIS module: found by
    // its stable name and bound to this module's flecs type cache, so member<T>()
    // refers to it instead of registering T again (with this module's hooks).
    // False if the host has not registered it.
    template <class T>
    bool flecs_use_std(flecs::world& w)
    {
        const std::string name = detail::StdName<T>::get();
        const flecs::entity_t id = ecs_lookup_path_w_sep(w.c_ptr(), 0, name.c_str(), "::", "::", false);
        if (id == 0 || !ecs_has_id(w.c_ptr(), id, ecs_id(EcsType)))
            return false;
        flecs_component_ids_set(w.c_ptr(), flecs::_::type<T>::index(), id);
        return true;
    }

    // std::vector<E> of one of this module's types: registered (and owned) by the
    // module if nobody has yet, so unregister_world() removes it with the
    // components. False if E itself is not described to flecs.
    template <class E>
    bool flecs_ensure_vector(flecs::world& w)
    {
        if (flecs_has_type<std::vector<E>>(w))
            return true;
        if (!flecs_has_type<E>(w))
            return false;
        w.component<std::vector<E>>().opaque(detail::stdVectorSupport<E>);
        detail::ownedFlecsTypes().push_back(&flecs_unregister<std::vector<E>>);
        return true;
    }

#endif // FLECS_META

    // Register every flecs component of this module in `w` (sim thread).
    // Structs embedding other meta structs wait until those are described; a
    // dependency that never shows up (e.g. its module is not loaded) cannot block
    // the rest — those entries are registered last, without it.
    inline void register_world(flecs::world& w)
    {
        std::vector<TypeEntry*> pending;
        for (TypeEntry* e = detail::head(); e; e = e->next)
        {
            if (e->flecs)
            {
                pending.push_back(e);
            }
        }
        while (!pending.empty())
        {
            bool progressed = false;
            for (auto it = pending.begin(); it != pending.end();)
            {
                if (!(*it)->flecsReady || (*it)->flecsReady(w))
                {
                    (*it)->flecs(w);
                    it = pending.erase(it);
                    progressed = true;
                }
                else
                {
                    ++it;
                }
            }
            if (!progressed)
            {
                for (TypeEntry* e : pending)
                {
                    e->flecs(w);
                }
                break;
            }
        }
    }

    // Remove every flecs component of this module from `w` (sim thread, outside
    // progress()). Also destroy the module's systems/observers yourself before
    // unloading — their callbacks live in the module too.
    inline void unregister_world(flecs::world& w)
    {
        std::vector<TypeEntry*> order; // reverse: structs go before the types they embed
        for (TypeEntry* e = detail::head(); e; e = e->next)
        {
            if (e->unflecs)
            {
                order.push_back(e);
            }
        }
        for (auto it = order.rbegin(); it != order.rend(); ++it)
        {
            (*it)->unflecs(w);
        }
        // Then the vector types it registered for its own element types.
        auto& owned = detail::ownedFlecsTypes();
        for (auto it = owned.rbegin(); it != owned.rend(); ++it)
        {
            (*it)(w);
        }
        owned.clear();
    }

#if defined(FLECS_META) && defined(FLECS_JSON)

    // ── hot reload: keep component data across unload / reload ─────────────────
    //
    // save_world() writes every value of this module's components (and which
    // entities carry its tags) to a string, keyed by component symbol, entity id
    // and MEMBER NAME (flecs meta JSON). The host keeps the string while the module
    // is unloaded — it holds no pointer into the module. After the new build is
    // loaded and registered, load_world() puts the values back:
    //   * members that still exist get their old value (by name, converted if the
    //     type changed compatibly, e.g. int → float),
    //   * new members keep their default, removed members are ignored,
    //   * a component that no longer exists, or an entity deleted meanwhile, is
    //     skipped and reported.
    // Only components flecs can describe (meta) are saved; the others are reported
    // in `notSaved`. Run both on the sim thread, outside world.progress().

    namespace detail
    {
        // Split a JSON object's top-level members: {"a":1,"b":{"x":2}} →
        // ("a","1"), ("b","{\"x\":2}"). False if `json` is not an object.
        inline bool splitJsonObject(const std::string& json, std::vector<std::pair<std::string, std::string>>& out)
        {
            size_t i = json.find('{');
            if (i == std::string::npos)
                return false;
            ++i;
            while (i < json.size())
            {
                while (i < json.size() && (json[i] == ' ' || json[i] == ',' || json[i] == '\n' || json[i] == '\t'))
                    ++i;
                if (i >= json.size() || json[i] == '}')
                    return true;
                if (json[i] != '"')
                    return false;
                const size_t keyEnd = json.find('"', i + 1);
                if (keyEnd == std::string::npos)
                    return false;
                std::string key = json.substr(i + 1, keyEnd - i - 1);
                i = json.find(':', keyEnd);
                if (i == std::string::npos)
                    return false;
                const size_t valueStart = ++i;
                int depth = 0;
                bool inString = false;
                for (; i < json.size(); ++i)
                {
                    const char c = json[i];
                    if (inString)
                    {
                        if (c == '\\')
                            ++i;
                        else if (c == '"')
                            inString = false;
                        continue;
                    }
                    if (c == '"')
                        inString = true;
                    else if (c == '{' || c == '[')
                        ++depth;
                    else if (c == '}' || c == ']')
                    {
                        if (depth == 0)
                            break; // the object's own closing brace
                        --depth;
                    }
                    else if (c == ',' && depth == 0)
                        break;
                }
                out.emplace_back(std::move(key), json.substr(valueStart, i - valueStart));
            }
            return false;
        }
    } // namespace detail

    inline std::string save_world(flecs::world& w, std::vector<std::string>* notSaved = nullptr)
    {
        ecs_world_t* world = w.c_ptr();
        std::string out = "reflect-snapshot 1\n";
        for (TypeEntry* e = detail::head(); e; e = e->next)
        {
            if (!e->flecsId)
                continue;
            const flecs::entity_t id = e->flecsId(w);
            if (id == 0 || !ecs_is_alive(world, id))
                continue;
            const char* symbol = ecs_get_symbol(world, id);
            if (!symbol)
                continue;
            const ecs_type_info_t* ti = ecs_get_type_info(world, id);
            const bool tag = !ti || ti->size == 0;
            if (!tag && !ecs_has_id(world, id, ecs_id(EcsType)))
            {
                if (notSaved)
                    notSaved->push_back(std::string(e->name) + ": flecs has no description (meta) for it");
                continue;
            }
            out += "C\t";
            out += symbol;
            out += tag ? "\ttag\n" : "\n";
            ecs_iter_t it = ecs_each_id(world, id);
            while (ecs_each_next(&it))
            {
                char* base = tag ? nullptr : static_cast<char*>(ecs_field_w_size(&it, static_cast<size_t>(ti->size), 0));
                for (int32_t i = 0; i < it.count; ++i)
                {
                    out += "E\t" + std::to_string(static_cast<unsigned long long>(it.entities[i]));
                    if (!tag)
                    {
                        char* json = ecs_ptr_to_json(world, id, base + static_cast<size_t>(i) * ti->size);
                        out += '\t';
                        if (json)
                        {
                            // flecs escapes control characters in strings, so a value
                            // never spans lines; strip any raw one defensively.
                            for (const char* c = json; *c; ++c)
                                out += (*c == '\n' || *c == '\r') ? ' ' : *c;
                            ecs_os_free(json);
                        }
                    }
                    out += '\n';
                }
            }
        }
        return out;
    }

    // Returns how many component values / tags were restored.
    inline std::size_t load_world(flecs::world& w, const std::string& snapshot,
                                  std::vector<std::string>* problems = nullptr)
    {
        ecs_world_t* world = w.c_ptr();
        auto report = [&](std::string msg) {
            if (problems)
                problems->push_back(std::move(msg));
        };
        if (snapshot.rfind("reflect-snapshot 1\n", 0) != 0)
        {
            report("not a reflect snapshot (or an unknown version)");
            return 0;
        }
        std::size_t restored = 0;
        flecs::entity_t comp = 0;
        bool tag = false;
        size_t size = 0;
        std::string compName;
        size_t pos = snapshot.find('\n') + 1;
        while (pos < snapshot.size())
        {
            size_t end = snapshot.find('\n', pos);
            if (end == std::string::npos)
                end = snapshot.size();
            const std::string line = snapshot.substr(pos, end - pos);
            pos = end + 1;
            if (line.rfind("C\t", 0) == 0)
            {
                const size_t tab = line.find('\t', 2);
                compName = line.substr(2, tab == std::string::npos ? std::string::npos : tab - 2);
                comp = ecs_lookup_symbol(world, compName.c_str(), false, false);
                tag = tab != std::string::npos;
                if (comp == 0)
                {
                    report(compName + ": component no longer exists; its values are dropped");
                    continue;
                }
                const ecs_type_info_t* ti = ecs_get_type_info(world, comp);
                size = ti ? static_cast<size_t>(ti->size) : 0;
                if (!tag && size == 0)
                {
                    report(compName + ": is a tag now; restoring presence only");
                    tag = true;
                }
                continue;
            }
            if (line.rfind("E\t", 0) != 0 || comp == 0)
                continue;
            const size_t tab = line.find('\t', 2);
            const flecs::entity_t entity = std::strtoull(line.c_str() + 2, nullptr, 10);
            if (!ecs_is_alive(world, entity))
            {
                report(compName + ": entity " + std::to_string(static_cast<unsigned long long>(entity))
                       + " no longer exists");
                continue;
            }
            if (tag)
            {
                ecs_add_id(world, entity, comp);
                ++restored;
                continue;
            }
            if (tab == std::string::npos)
                continue;
            const std::string json = line.substr(tab + 1);
            void* ptr = ecs_ensure_id(world, entity, comp, size);
            if (!ptr)
                continue;
            if (!ecs_ptr_from_json(world, comp, ptr, json.c_str(), nullptr))
            {
                // A member whose type changed incompatibly fails the whole object:
                // restore member by member and keep every one that still fits.
                std::vector<std::pair<std::string, std::string>> members;
                detail::splitJsonObject(json, members);
                for (const auto& [name, value] : members)
                {
                    const std::string one = "{\"" + name + "\":" + value + "}";
                    if (!ecs_ptr_from_json(world, comp, ptr, one.c_str(), nullptr))
                        report(compName + "." + name + " on entity "
                               + std::to_string(static_cast<unsigned long long>(entity))
                               + ": old value does not fit the new type; default kept");
                }
            }
            ecs_modified_id(world, entity, comp);
            ++restored;
        }
        return restored;
    }

#endif // FLECS_META && FLECS_JSON

#endif // REFLECT_WITH_FLECS

} // namespace reflect

#if defined(__GNUC__) && !defined(_WIN32)
#pragma GCC visibility pop
#endif

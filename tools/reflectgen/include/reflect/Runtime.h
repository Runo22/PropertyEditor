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
//      reflect::unregister_world(world);   // remove + delete components (sim thread,
//                                          //   outside world.progress())
//      reflect::unregister_types();        // drop rpe bridges (GUI stops inspecting)
//      FreeLibrary                          // RTTR undoes this module's registrations
// ─────────────────────────────────────────────────────────────────────────────

#include "reflect/Reflect.h"

#include <rttr/registration.h>

#include <cstddef>
#include <string>
#include <type_traits>
#include <vector>

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
#endif
        TypeEntry* next = nullptr;
    };

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
    }

#endif // REFLECT_WITH_FLECS

} // namespace reflect

#if defined(__GNUC__) && !defined(_WIN32)
#pragma GCC visibility pop
#endif

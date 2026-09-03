#pragma once

#include "rpe/core/TypeBridge.h"
#include "rpe/ecs/flecs_prelude.h"

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  Bind a C++ component type to the inspector, deriving the flecs ↔ RTTR mapping
    //  FROM FLECS ITSELF rather than inferring it from name similarity.
    //
    //  Why this exists
    //  ---------------
    //  TypeBridge resolves a flecs component to an RTTR type by NAME. That only works
    //  while the two names stay relatable. They often don't: it is perfectly normal to
    //  register the flecs component under a different namespace (or a different name
    //  entirely) than the RTTR type. When the names don't line up, resolution falls
    //  back to the bare leaf ("Stats") — which is AMBIGUOUS the moment two components
    //  share a leaf, and then one type silently wins for both. Reading a component
    //  through the wrong type shows shifted/garbage data.
    //
    //  world.component<T>() returns the component entity flecs ALREADY has for T —
    //  flecs caches component ids per C++ type, so this never creates a duplicate no
    //  matter what name you originally gave it. Its path is therefore exactly the name
    //  the inspector will see, and registering that path as T's alias makes resolution
    //  exact: namespace-proof, leaf-collision-proof, and independent of how the RTTR
    //  type happens to be named.
    //
    //  Usage — call it once per component, after your flecs registration:
    //      world.component<game::Stats>("render.Stats");   // your own naming
    //      rpe::bindComponents<game::Stats, ai::Stats>(world);
    //
    //  Idempotent: safe to call again (re-registering just refreshes the entry).
    //  Prefer this over a bare TypeBridge::registerType<T>() whenever the flecs and
    //  RTTR names can differ, or two components share a leaf name.
    // ─────────────────────────────────────────────────────────────────────────────
    template <class T>
    flecs::entity bindComponent(flecs::world& world)
    {
        TypeBridge::registerType<T>();
        // Existing id for T when already registered; registers it otherwise.
        flecs::entity comp = world.component<T>();
        const flecs::string path = comp.path(".", "");
        if (const char* p = path.c_str(); p && p[0] != '\0')
        {
            TypeBridge::registerAlias(rttr::type::get<T>(), p);
        }
        return comp;
    }

    template <class... Ts>
    void bindComponents(flecs::world& world)
    {
        (bindComponent<Ts>(world), ...);
    }

} // namespace rpe

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
    //  Binding registers the component's ACTUAL flecs path as T's alias, so resolution
    //  is exact: namespace-proof, leaf-collision-proof, and independent of how the RTTR
    //  type happens to be named.
    //
    //  IMPORTANT — these never register or rename a flecs component.
    //  Naming a component is yours to do; binding only reads the name you gave it.
    //  Registering T a second time under a different name is exactly what makes flecs
    //  raise a component-redefinition error, so bindComponent(world) deliberately does
    //  NOTHING (returning an invalid entity) when flecs doesn't know T yet — call it
    //  after your own registration, or use the entity overload.
    //
    //      world.component<game::Stats>("render.Stats");        // your own naming
    //      rpe::bindComponents<game::Stats, ai::Stats>(world);  // then bind
    //
    //  Idempotent: safe to call again (re-registering just refreshes the entry).
    // ─────────────────────────────────────────────────────────────────────────────

    // Bind T to an already-known flecs component entity. Use this when you have the
    // entity in hand — it does no lookup and cannot register or rename anything.
    template <class T>
    void bindComponent(flecs::entity comp)
    {
        TypeBridge::registerType<T>();
        if (!comp.is_valid())
        {
            return;
        }
        const flecs::string path = comp.path(".", "");
        if (const char* p = path.c_str(); p && p[0] != '\0')
        {
            TypeBridge::registerAlias(rttr::type::get<T>(), p);
        }
    }

    // Bind T using the component flecs ALREADY has for it. Returns that entity, or an
    // invalid entity when flecs hasn't registered T yet — in which case only the
    // bridge entry is added and no alias is bound (call again after registering T).
    template <class T>
    flecs::entity bindComponent(flecs::world& world)
    {
        TypeBridge::registerType<T>();
        if (!flecs::_::type<T>::registered(world.c_ptr()))
        {
            return flecs::entity(); // don't register/rename T — that's the caller's job
        }
        flecs::entity comp = world.component<T>(); // cached id; no re-registration
        bindComponent<T>(comp);
        return comp;
    }

    // Bind a pack. Types flecs doesn't know yet are skipped (see above) rather than
    // aborting the rest — one unregistered type must not leave the others unbound.
    template <class... Ts>
    void bindComponents(flecs::world& world)
    {
        (static_cast<void>(bindComponent<Ts>(world)), ...);
    }

} // namespace rpe

#pragma once

#include <QString>

#include <vector>

#include "rpe/ecs/flecs_prelude.h"

namespace rpe
{

    // One flecs component and whether the inspector can display it (i.e. whether
    // TypeBridge resolves its name to a registered RTTR type).
    struct ComponentResolution
    {
        QString name;     // flecs component name (the unscoped/leaf name)
        QString path;     // flecs full scoped path (e.g. "game.Transform")
        bool bridged = false; // resolves to a bridged RTTR type the inspector can show
        QString rttrType;     // the RTTR type name it resolved to ("" if unresolved)
    };

    // Enumerate the registered flecs components in `world` and report, for each,
    // whether TypeBridge can resolve it to an RTTR type. Use this to debug "my
    // component isn't listed": `bridged == false` means either no RTTR registration
    // or a name mismatch (see TypeBridge::registerType<T>(flecsName) / registerAlias).
    //
    // flecs' own built-in components (the "flecs.*" scope) are skipped by default,
    // since they are never inspectable; pass includeBuiltins = true to see them too.
    //
    // Read-only, but it touches the world — call it on the simulation thread (or
    // with progress() stopped), like any other world query.
    std::vector<ComponentResolution> scanComponents(const flecs::world& world,
                                                    bool includeBuiltins = false);

} // namespace rpe

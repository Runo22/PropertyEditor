#pragma once

#include <QString>
#include <QStringList>

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
        bool tag = false;     // zero-size component: addable presence marker, no data
        QString rttrType;     // the RTTR type name it resolved to ("" if unresolved)
        // Number of properties RTTR reports for the resolved type. A bridged component
        // with ZERO properties is the "it's listed but shows no editors" case: the type
        // resolved (rttr::type::get<T>() is valid for ANY complete type) but nothing is
        // reflected — usually an RTTR_REGISTRATION block that never ran, e.g. dropped by
        // the linker as an unreferenced static initializer in a Release build.
        int propertyCount = 0;
    };

    // True for flecs' OWN scope — "flecs", "flecs.core.Identifier", "flecs::meta…".
    // A user namespace that merely starts with those letters ("flecsx.Foo",
    // "FlecsTools.Bar") is NOT built-in: a plain prefix test used to hide such
    // components from the add menu, the component list and the entity scan alike.
    inline bool isFlecsBuiltinPath(const QString& path)
    {
        return path == QLatin1String("flecs") || path.startsWith(QLatin1String("flecs."))
            || path.startsWith(QLatin1String("flecs::"));
    }

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

    // How a component NAME given by the host ("game::Stats", "game.Stats",
    // "npc::Enemy", "Stats") resolves to one flecs component — the rule the
    // required-component filter applies, most specific first:
    //   1. the full path (either separator; a leading root separator is fine);
    //   2. a scope-aligned suffix ("npc::Enemy" → game.npc.Enemy);
    //   3. the leaf — only when the name has no scope at all.
    // A scoped name never falls back to the leaf (that would quietly pick another
    // namespace's same-named component). Several candidates at one level → the
    // shortest path, then alphabetical — deterministic.
    struct ComponentNameMatch
    {
        enum class Via
        {
            None,
            FullPath,
            ScopeSuffix,
            LeafName
        };
        flecs::entity component; // invalid when via == None
        Via via = Via::None;
        QStringList candidates; // every component matching at that level (dotted paths), in pick order
        bool ambiguous() const
        {
            return candidates.size() > 1;
        }
    };
    ComponentNameMatch matchComponentName(const flecs::world& world, const QString& name);

    // Find the flecs component entity whose name matches `name`. With bridgedOnly
    // (default) only a component that resolves to a registered RTTR type is
    // returned. Used to apply add/remove-component edits given the listed name.
    // Returns an invalid entity if none matches. Touches the world — sim thread.
    flecs::entity findComponentEntity(const flecs::world& world, const QString& name,
                                      bool bridgedOnly = true);

} // namespace rpe

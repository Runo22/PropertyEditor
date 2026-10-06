#pragma once

#include "rpe/ecs/flecs_prelude.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  Health checks — on-demand diagnostics for "why doesn't my component show up /
    //  bind / edit the way I expect?". Nothing here runs by itself and nothing is
    //  logged: call what you need, when you need it, and read the report (or print
    //  report.toText()). No EcsMirror or browser is required.
    //
    //  The world-taking checks READ the world: call them where you may touch it —
    //  on the simulation thread (e.g. after world.progress(), or from a system), or
    //  with progress() stopped. checkTypeRegistry() and checkFlecsBuild() need no
    //  world and are safe anywhere.
    //
    //      auto report = rpe::checkHealth(world, { "game::Player", { "Enemy" } });
    //      if (report.hasWarnings())
    //          qInfo().noquote() << report.toText();
    // ─────────────────────────────────────────────────────────────────────────────

    struct HealthIssue
    {
        enum class Severity
        {
            Info,    // worth knowing; often intentional
            Warning, // probably a mistake: something won't show / bind as expected
            Error    // definitely wrong: values would be read/written incorrectly
        };
        Severity severity = Severity::Info;
        QString check;   // stable id of the check, e.g. "size-mismatch" (see each function)
        QString subject; // what it is about: a component path, a type or tag name
        QString message; // what's wrong
        QString fix;     // what to do about it (may be empty)
    };

    struct HealthReport
    {
        QVector<HealthIssue> issues;

        int count(HealthIssue::Severity atLeast) const;
        bool hasErrors() const
        {
            return count(HealthIssue::Severity::Error) > 0;
        }
        bool hasWarnings() const
        {
            return count(HealthIssue::Severity::Warning) > 0;
        }
        bool isClean() const
        {
            return issues.isEmpty();
        }
        // Issues for one check id / one subject — handy in tests and tools.
        QVector<HealthIssue> byCheck(const QString& check) const;
        QVector<HealthIssue> about(const QString& subject) const;

        // Human-readable, one block per issue, most severe first; issues below
        // `minimum` are left out. "No issues." when there is nothing to show.
        QString toText(HealthIssue::Severity minimum = HealthIssue::Severity::Info) const;

        void append(const HealthReport& other)
        {
            issues += other.issues;
        }
    };

    // The TypeBridge registry on its own (no world):
    //   "duplicate-rttr-name"  Warning  two bridged types share one RTTR name, so a
    //                                   name can't tell them apart (only aliases can)
    //   "no-properties"        Info     a bridged type with no RTTR properties: it
    //                                   lists and can be added/removed, but shows no
    //                                   fields (intended for marker components)
    HealthReport checkTypeRegistry();

    // The flecs build actually running vs the headers rpe was compiled against:
    //   "flecs-version-mismatch"  Error  different flecs versions in one process —
    //                                    header/ABI mismatch, unpredictable crashes
    //   "flecs-debug-build"       Info   flecs is a debug build (slower)
    HealthReport checkFlecsBuild();

    // Every named, non-built-in component in the world against the registry:
    //   "size-mismatch"       Error    the component binds an RTTR type of another
    //                                  size — values would be read through the wrong
    //                                  layout (the classic "data looks shifted")
    //   "same-type-twice"     Warning  two components bind the SAME RTTR type — at
    //                                  most one of them can really be it
    //   "ambiguous-name"      Warning  the component's name matched several bridged
    //                                  types and one was picked by a tie-break
    //   "short-name-only"     Info     it bound only through its leaf name
    //   "unbridged-rttr-type" Warning  RTTR knows a type by this component's FULL
    //                                  name, but TypeBridge doesn't: not shown/addable
    //   "unbridged-maybe"     Info     not bridged; an unbridged RTTR type with the
    //                                  same SHORT name exists — possibly this one
    //   "unused-bridge"       Info     a bridged type no component in this world uses
    //                                  (plugin not loaded? a different flecs name?)
    HealthReport checkComponents(const flecs::world& world);

    // A required-component name, resolved exactly as the browser's filter does
    // (rpe::matchComponentName):
    //   "required-not-found"  Warning  nothing matches — the entity list stays empty
    //   "required-ambiguous"  Warning  several match; the shortest path is used
    //   "required-inexact"    Info     matched by scope suffix / leaf, not full path
    HealthReport checkRequiredComponent(const flecs::world& world, const QString& required);

    // Prefab group tags, resolved exactly as the add-entity picker does:
    //   "group-tag-not-found"   Warning  world.lookup() can't find it (a dotted path
    //                                    is never resolved — use "::" or an alias)
    //   "group-tag-on-no-prefab" Warning it exists but no prefab carries it (added to
    //                                    the instances? used as a pair?)
    HealthReport checkPrefabGroups(const flecs::world& world, const QStringList& tags);

    struct HealthCheckOptions
    {
        QString requiredComponent; // checked when non-empty
        QStringList prefabGroupTags; // checked when non-empty
    };

    // All of the above that apply.
    HealthReport checkHealth(const flecs::world& world, const HealthCheckOptions& options = {});

} // namespace rpe

#pragma once

#include <QPair>
#include <QString>
#include <QStringList>

#include <vector>

#include "rpe/ecs/MirrorChannel.h"
#include "rpe/ecs/flecs_prelude.h"

namespace rpe
{

    // One flecs component and whether the inspector can display it (i.e. whether
    // TypeBridge resolves its name to a registered RTTR type).
    struct ComponentResolution
    {
        QString name;     // flecs component name (the unscoped/leaf name)
        QString path;     // flecs full scoped path (e.g. "game.Transform")
        qulonglong id = 0; // the component entity
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

    // The short name of a scoped name, for showing: its last segment, with any
    // template arguments shortened the same way — "game::Damage" → "Damage",
    // "std.vector<resim.core.ModelValue>" → "vector<ModelValue>". A separator
    // inside "<…>" never splits (a plain last-"." cut gave "ModelValue>"), nor
    // does flecs' escaped "\." inside a name.
    inline QString scopedLeaf(const QString& name)
    {
        int depth = 0;
        int cut = -1;
        int sepLen = 0;
        for (int i = 0; i < name.size(); ++i)
        {
            const QChar c = name.at(i);
            if (c == QLatin1Char('<'))
            {
                ++depth;
            }
            else if (c == QLatin1Char('>'))
            {
                --depth;
            }
            else if (c == QLatin1Char('\\') && i + 1 < name.size() && name.at(i + 1) == QLatin1Char('.'))
            {
                ++i; // flecs' escaped dot: part of the name
            }
            else if (depth == 0 && c == QLatin1Char('.'))
            {
                cut = i;
                sepLen = 1;
            }
            else if (depth == 0 && c == QLatin1Char(':') && i + 1 < name.size() && name.at(i + 1) == QLatin1Char(':'))
            {
                cut = i;
                sepLen = 2;
                ++i;
            }
        }
        const QString last = cut >= 0 ? name.mid(cut + sepLen) : name;
        const int lt = last.indexOf(QLatin1Char('<'));
        if (lt < 0 || !last.endsWith(QLatin1Char('>')))
        {
            return last;
        }
        QStringList args;
        int d = 0;
        int start = lt + 1;
        for (int i = lt + 1; i < last.size() - 1; ++i)
        {
            const QChar c = last.at(i);
            if (c == QLatin1Char('<'))
            {
                ++d;
            }
            else if (c == QLatin1Char('>'))
            {
                --d;
            }
            else if (c == QLatin1Char(',') && d == 0)
            {
                args << scopedLeaf(last.mid(start, i - start).trimmed());
                start = i + 1;
            }
        }
        args << scopedLeaf(last.mid(start, last.size() - 1 - start).trimmed());
        return last.left(lt + 1) + args.join(QStringLiteral(", ")) + QLatin1Char('>');
    }

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

    // ── Pairs ────────────────────────────────────────────────────────────────────

    // The name a pair's TARGET is shown and keyed by: its full path
    // ("elements.Fire"), or "#<id>" when it has no name. The full path — not the
    // leaf — keeps (Damage, a.Fire) and (Damage, b.Fire) apart.
    QString pairTargetName(flecs::entity target);

    // An entity by a path the user typed: "elements.Fire", "elements::Fire",
    // "::elements::Fire" or "#<id>". Invalid if there is none (or it's dead).
    flecs::entity lookupEntityPath(const flecs::world& world, const QString& path);

    // Why (relation, target) can't be added as a pair — or empty when it can. These
    // are flecs' own rules: a relation with the Target trait, a target with the
    // Relationship trait, a OneOf relation's target outside its scope, a dead or
    // wildcard id, flecs' own relationships (ChildOf, IsA, …). A debug flecs build
    // ABORTS on most of them, so check before adding.
    QString pairRejection(const flecs::world& world, flecs::entity relation, flecs::entity target);

    // Why `component` can't be added by itself — it has flecs' Relationship or
    // Target trait — or empty when it can.
    QString plainAddRejection(const flecs::world& world, flecs::entity component);

    // Add (relation, target) to `e`, after pairRejection. With target 0 the target
    // is looked up by `targetPath` (lookupEntityPath). Returns the pair id, or 0 if
    // refused (`why`, when given, says why). Touches the world — sim thread.
    flecs::id_t addPairChecked(const flecs::world& world, flecs::entity e, flecs::entity_t relation,
                               flecs::entity_t target, const QString& targetPath = QString(),
                               QString* why = nullptr);

    // What the "Add component" picker offers: every bridged data component and
    // every tag, plus the RELATIONSHIPS — each with the targets to suggest.
    //
    // A relationship is offered as a pair when it
    //   • has flecs' Relationship trait (pair only),
    //   • is named in `pairRelations` (full path or a trailing part, as in
    //     matchComponentName; plain entities too — "Likes"), or
    //   • is already used as one in the world: (R, *) is in use.
    // A relationship that carries DATA (Damage) is offered BY ITSELF as well —
    // also next to a pair of it; a TAG relationship (Left, Right) is pair only, as
    // is anything with the Relationship trait (flecs forbids the rest). An entity
    // with the Target trait is only ever a target, so it isn't offered at all.
    // Suggested targets: the children of its OneOf scope (and then only those are
    // valid), else the targets it is used with now, at most `maxTargets`.
    // What a relationship's "Other target…" step can offer, sorted by path, at
    // most `limit`: plain entities, tags (`tag`), and YOUR components that carry
    // data (registered with rpe — `carriesData`: a tag relationship such as (Left,
    // HotRequest) takes its data from its target, so those come first there). Never
    // the machinery (flecs' and rpe's own entities, systems, observers, queries,
    // modules), enum constants, std:: / meta types or other unregistered data types
    // (what flecs registers for member fields), Relationships, unnamed entities.
    QVector<MirrorChannel::TargetEntry> pairTargetCandidates(const flecs::world& world, int limit = 5000);

    struct AddCatalogOptions
    {
        QStringList pairRelations;
        int maxTargets = 64;
    };
    QVector<MirrorChannel::CatalogEntry> buildAddCatalog(const flecs::world& world,
                                                         const AddCatalogOptions& options = {});

} // namespace rpe

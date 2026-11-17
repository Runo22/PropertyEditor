#include "rpe/ecs/ComponentScan.h"

#include "rpe/core/TypeBridge.h"

#include <QPair>
#include <QSet>
#include <QVector>

#include <algorithm>

namespace rpe
{

    std::vector<ComponentResolution> scanComponents(const flecs::world& world, bool includeBuiltins)
    {
        std::vector<ComponentResolution> out;

        // Every component type is an entity carrying flecs::Component. Iterate them
        // directly so we report types that exist but are not (yet) on any entity.
        flecs::query<> q = const_cast<flecs::world&>(world)
                               .query_builder()
                               .with<flecs::Component>()
                               .build();

        q.each([&](flecs::entity comp) {
            const char* raw = comp.name();
            if (!raw || raw[0] == '\0')
            {
                return;
            }
            // flecs' own components live under the "flecs" scope (flecs.core, …);
            // they are never inspectable, so skip them unless asked. path() returns
            // an RAII flecs::string (frees itself), implicitly convertible to char*.
            // path(sep, "") → "ns.Leaf" with no leading separator (init_sep empty).
            const flecs::string fullPath = comp.path(".", "");
            const char* pc = fullPath.c_str();
            const QString path = pc ? QString::fromUtf8(pc) : QString();
            if (!includeBuiltins && isFlecsBuiltinPath(path))
            {
                return;
            }

            ComponentResolution r;
            r.name = QString::fromUtf8(raw);
            r.path = path;
            r.id = static_cast<qulonglong>(comp.id());
            const flecs::Component* cd = comp.try_get<flecs::Component>();
            r.tag = !cd || cd->size <= 0;
            // Resolve by the FULL PATH, exactly as the inspector's component listing
            // does — resolving by the leaf here would report a different (ambiguously
            // picked) type than the one actually bound whenever two components share a
            // leaf name in different namespaces, i.e. it would lie in the very case you
            // are most likely debugging. Fall back to the leaf for components whose
            // path doesn't resolve (matches resolveByName's own ordering).
            rttr::type t = TypeBridge::resolveByName(path.toUtf8().constData());
            if (!t.is_valid())
            {
                t = TypeBridge::resolveByName(raw);
            }
            r.bridged = t.is_valid();
            if (r.bridged)
            {
                r.rttrType = QString::fromStdString(t.get_name().to_string());
                r.propertyCount = static_cast<int>(t.get_properties().size());
            }
            out.push_back(std::move(r));
        });

        return out;
    }

    ComponentNameMatch matchComponentName(const flecs::world& world, const QString& name)
    {
        ComponentNameMatch out;
        QString want = name;
        want.replace(QStringLiteral("::"), QStringLiteral("."));
        while (want.startsWith(QLatin1Char('.')))
        {
            want.remove(0, 1);
        }
        if (want.isEmpty())
        {
            return out;
        }
        const bool scoped = want.contains(QLatin1Char('.'));
        const QString suffix = QLatin1Char('.') + want;

        QVector<QPair<QString, flecs::entity>> exact, bySuffix, byLeaf;
        flecs::query<> q = const_cast<flecs::world&>(world).query_builder().with<flecs::Component>().build();
        q.each([&](flecs::entity comp) {
            const char* cn = comp.name();
            if (!cn || cn[0] == '\0')
            {
                return;
            }
            const flecs::string fp = comp.path(".", "");
            const QString path = fp.c_str() ? QString::fromUtf8(fp.c_str()) : QString();
            if (path.isEmpty() || isFlecsBuiltinPath(path))
            {
                return;
            }
            if (path == want)
            {
                exact.append({ path, comp });
            }
            else if (scoped && path.endsWith(suffix))
            {
                bySuffix.append({ path, comp });
            }
            else if (!scoped && QString::fromUtf8(cn) == want)
            {
                byLeaf.append({ path, comp });
            }
        });

        const auto take = [&](QVector<QPair<QString, flecs::entity>>& hits, ComponentNameMatch::Via via) {
            std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
                return a.first.size() != b.first.size() ? a.first.size() < b.first.size() : a.first < b.first;
            });
            out.via = via;
            out.component = hits.first().second;
            for (const auto& h : hits)
            {
                out.candidates << h.first;
            }
        };
        if (!exact.isEmpty())
        {
            take(exact, ComponentNameMatch::Via::FullPath);
        }
        else if (!bySuffix.isEmpty())
        {
            take(bySuffix, ComponentNameMatch::Via::ScopeSuffix);
        }
        else if (!byLeaf.isEmpty())
        {
            take(byLeaf, ComponentNameMatch::Via::LeafName);
        }
        return out;
    }

    flecs::entity findComponentEntity(const flecs::world& world, const QString& name, bool bridgedOnly)
    {
        flecs::entity found;
        flecs::query<> q = const_cast<flecs::world&>(world)
                               .query_builder()
                               .with<flecs::Component>()
                               .build();
        q.each([&](flecs::entity comp) {
            if (found.is_valid())
            {
                return;
            }
            const char* cn = comp.name();
            if (!cn || cn[0] == '\0')
            {
                return;
            }
            // Match either the short (leaf) name or the full scoped path, so callers
            // can pass either "Transform" or "game::Transform" / "game.Transform".
            const QString leaf = QString::fromUtf8(cn);
            bool nameMatch = (name == leaf);
            if (!nameMatch)
            {
                const flecs::string path = comp.path("::", "");
                const flecs::string dotPath = comp.path(".", "");
                nameMatch = (name == QString::fromUtf8(path.c_str())) || (name == QString::fromUtf8(dotPath.c_str()));
            }
            if (!nameMatch)
            {
                return;
            }
            if (bridgedOnly && !TypeBridge::resolveByName(cn).is_valid())
            {
                return;
            }
            found = comp;
        });
        return found;
    }

    QString pairTargetName(flecs::entity target)
    {
        const char* n = target.name();
        if (!n || n[0] == '\0')
        {
            return QStringLiteral("#%1").arg(static_cast<qulonglong>(target.id()));
        }
        const flecs::string p = target.path(".", "");
        return p.c_str() ? QString::fromUtf8(p.c_str()) : QString::fromUtf8(n);
    }

    flecs::entity lookupEntityPath(const flecs::world& world, const QString& pathIn)
    {
        const ecs_world_t* w = world.c_ptr();
        QString p = pathIn.trimmed();
        if (p.startsWith(QLatin1Char('#')))
        {
            bool ok = false;
            const qulonglong v = p.mid(1).toULongLong(&ok);
            if (!ok || v == 0)
            {
                return flecs::entity();
            }
            // Exactly that id, generation included ("#<id>" is how an unnamed
            // entity is shown): a slot recycled since then is another entity.
            const ecs_entity_t e = static_cast<ecs_entity_t>(v);
            return ecs_is_valid(w, e) && ecs_is_alive(w, e) ? flecs::entity(w, e) : flecs::entity();
        }
        p.replace(QStringLiteral("::"), QStringLiteral("."));
        while (p.startsWith(QLatin1Char('.')))
        {
            p.remove(0, 1);
        }
        if (p.isEmpty())
        {
            return flecs::entity();
        }
        const QByteArray utf8 = p.toUtf8();
        const ecs_entity_t e = ecs_lookup_path_w_sep(w, 0, utf8.constData(), ".", nullptr, false);
        return e ? flecs::entity(w, e) : flecs::entity();
    }

    namespace
    {
        QString entityPath(flecs::entity e)
        {
            const flecs::string p = e.path(".", "");
            return p.c_str() ? QString::fromUtf8(p.c_str()) : QString();
        }

        // The scope a OneOf relationship takes its targets from: the relationship
        // itself (OneOf), its (OneOf, X) target, or 0 when it has no such rule.
        ecs_entity_t oneOfScope(const ecs_world_t* w, ecs_entity_t rel)
        {
            if (ecs_has_id(w, rel, EcsOneOf))
            {
                return rel;
            }
            return ecs_get_target(w, rel, EcsOneOf, 0);
        }
    } // namespace

    QString pairRejection(const flecs::world& world, flecs::entity relation, flecs::entity target)
    {
        const ecs_world_t* w = world.c_ptr();
        if (!relation.is_valid() || !ecs_is_valid(w, relation.id()) || !ecs_is_alive(w, relation.id()))
        {
            return QStringLiteral("the relationship no longer exists");
        }
        if (!target.is_valid() || !ecs_is_valid(w, target.id()) || !ecs_is_alive(w, target.id()))
        {
            return QStringLiteral("the target no longer exists");
        }
        if (ecs_id_is_wildcard(relation.id()) || ecs_id_is_wildcard(target.id()))
        {
            return QStringLiteral("a wildcard can't be added");
        }
        const QString relPath = entityPath(relation);
        if (isFlecsBuiltinPath(relPath))
        {
            return QStringLiteral("'%1' is one of flecs' own relationships").arg(relPath);
        }
        if (ecs_has_id(w, relation.id(), EcsTarget))
        {
            return QStringLiteral("'%1' has the Target trait: it can only be a pair's target").arg(relPath);
        }
        if (ecs_has_id(w, target.id(), EcsRelationship) && !ecs_has_id(w, relation.id(), EcsTrait))
        {
            return QStringLiteral("'%1' has the Relationship trait: it can't be a target").arg(entityPath(target));
        }
        if (const ecs_entity_t scope = oneOfScope(w, relation.id()))
        {
            if (!ecs_has_pair(w, target.id(), EcsChildOf, scope))
            {
                return QStringLiteral("'%1' only takes children of '%2' (OneOf)")
                    .arg(relPath, entityPath(flecs::entity(w, scope)));
            }
        }
        if (!ecs_id_is_valid(w, ecs_pair(relation.id(), target.id())))
        {
            return QStringLiteral("(%1, %2) is not a valid pair").arg(relPath, entityPath(target));
        }
        return QString();
    }

    QString plainAddRejection(const flecs::world& world, flecs::entity component)
    {
        const ecs_world_t* w = world.c_ptr();
        if (!component.is_valid() || !ecs_is_alive(w, component.id()))
        {
            return QStringLiteral("the component no longer exists");
        }
        if (ecs_has_id(w, component.id(), EcsRelationship))
        {
            return QStringLiteral("'%1' has the Relationship trait: add it as a pair").arg(entityPath(component));
        }
        if (ecs_has_id(w, component.id(), EcsTarget))
        {
            return QStringLiteral("'%1' has the Target trait: it can only be a pair's target").arg(entityPath(component));
        }
        return QString();
    }

    flecs::id_t addPairChecked(const flecs::world& world, flecs::entity e, flecs::entity_t relation,
                               flecs::entity_t target, const QString& targetPath, QString* why)
    {
        const ecs_world_t* w = world.c_ptr();
        const auto refuse = [why](const QString& reason) -> flecs::id_t {
            if (why)
            {
                *why = reason;
            }
            return 0;
        };
        if (!e.is_valid() || !e.is_alive())
        {
            return refuse(QStringLiteral("the entity no longer exists"));
        }
        // Validity first: ecs_is_alive asserts on an id that isn't a plain entity.
        const flecs::entity rel = (relation && ecs_is_valid(w, relation)) ? flecs::entity(w, relation) : flecs::entity();
        flecs::entity tgt;
        if (target != 0)
        {
            tgt = ecs_is_valid(w, target) ? flecs::entity(w, target) : flecs::entity();
        }
        else
        {
            tgt = lookupEntityPath(world, targetPath);
            if (!tgt.is_valid())
            {
                return refuse(QStringLiteral("no entity named '%1'").arg(targetPath.trimmed()));
            }
        }
        const QString reason = pairRejection(world, rel, tgt);
        if (!reason.isEmpty())
        {
            return refuse(reason);
        }
        e.add(rel.id(), tgt.id());
        return ecs_pair(rel.id(), tgt.id());
    }

    QVector<QPair<qulonglong, QString>> namedEntities(const flecs::world& world, int limit)
    {
        QVector<QPair<qulonglong, QString>> out;
        const_cast<flecs::world&>(world).query_builder().with<flecs::Identifier>(flecs::Name).build().run([&](flecs::iter& it) {
            while (it.next())
            {
                for (const auto i : it)
                {
                    if (out.size() >= limit)
                    {
                        continue; // drain the iterator; nothing more is kept
                    }
                    // Not flecs' own entities, nor rpe's (the mirror's "rpe::EcsMirror"
                    // system), nor anyone's systems / observers / modules — nor a
                    // Relationship, which flecs never takes as a target.
                    const flecs::entity e = it.entity(i);
                    const QString path = pairTargetName(e);
                    if (!isFlecsBuiltinPath(path) && path != QLatin1String("rpe") && !path.startsWith(QLatin1String("rpe."))
                        && !e.has(flecs::Module) && !e.has(flecs::Relationship) && !e.has(ecs_id(EcsPoly), EcsSystem)
                        && !e.has(ecs_id(EcsPoly), EcsObserver))
                    {
                        out.append({ static_cast<qulonglong>(e.id()), path });
                    }
                }
            }
        });
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return a.second.compare(b.second, Qt::CaseInsensitive) < 0;
        });
        return out;
    }

    QVector<MirrorChannel::CatalogEntry> buildAddCatalog(const flecs::world& world, const AddCatalogOptions& options)
    {
        using Entry = MirrorChannel::CatalogEntry;
        using Use = MirrorChannel::PairUse;
        const ecs_world_t* w = world.c_ptr();
        flecs::world& mw = const_cast<flecs::world&>(world);

        // Relationships the host names. A name resolves like a component name
        // first; anything else (a plain entity such as "Likes") by its path.
        QSet<ecs_entity_t> declared;
        for (const QString& name : options.pairRelations)
        {
            const ComponentNameMatch m = matchComponentName(world, name);
            const flecs::entity d = m.component.is_valid() ? m.component : lookupEntityPath(world, name);
            if (d.is_valid())
            {
                declared.insert(d.id());
            }
        }

        const auto pairUse = [&](ecs_entity_t id) {
            if (ecs_has_id(w, id, EcsRelationship))
            {
                return Use::Only;
            }
            const bool pairs = declared.contains(id) || ecs_id_in_use(w, ecs_pair(id, EcsWildcard));
            if (!pairs)
            {
                return Use::None;
            }
            return ecs_id_in_use(w, id) ? Use::Also : Use::Only;
        };

        const auto fillTargets = [&](flecs::entity rel, Entry& entry) {
            entry.exclusive = ecs_has_id(w, rel.id(), EcsExclusive);
            QSet<ecs_entity_t> seen;
            const auto add = [&](flecs::entity t) {
                if (!t.is_valid() || seen.contains(t.id()))
                {
                    return;
                }
                if (entry.targets.size() >= options.maxTargets)
                {
                    entry.moreTargets = true;
                    return;
                }
                seen.insert(t.id());
                entry.targets.append({ static_cast<qulonglong>(t.id()), pairTargetName(t) });
            };
            if (const ecs_entity_t scope = oneOfScope(w, rel.id()))
            {
                entry.targetsFixed = true;
                flecs::entity(w, scope).children([&](flecs::entity child) { add(child); });
            }
            else
            {
                // The targets it's used with now, one per table — cheap.
                flecs::query<> q = mw.query_builder().with(rel.id(), flecs::Wildcard).build();
                q.run([&](flecs::iter& it) {
                    while (it.next())
                    {
                        const flecs::id pair = it.id(0);
                        if (pair.is_pair())
                        {
                            add(pair.second());
                        }
                    }
                });
            }
            std::sort(entry.targets.begin(), entry.targets.end(),
                      [](const MirrorChannel::PairTarget& a, const MirrorChannel::PairTarget& b) {
                          return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
                      });
        };

        QVector<Entry> out;
        QSet<ecs_entity_t> listed;
        for (const ComponentResolution& c : scanComponents(world))
        {
            const ecs_entity_t id = static_cast<ecs_entity_t>(c.id);
            if (!(c.bridged || c.tag) || ecs_has_id(w, id, EcsTarget))
            {
                continue;
            }
            Entry entry;
            entry.path = c.path.isEmpty() ? c.name : c.path;
            entry.tag = c.tag;
            entry.id = c.id;
            entry.pair = pairUse(id);
            if (entry.pair != Use::None)
            {
                fillTargets(flecs::entity(w, id), entry);
            }
            listed.insert(id);
            out.append(entry);
        }

        // Relationships that aren't components: plain entities with the
        // Relationship trait, and the ones the host names.
        const auto addRelationEntity = [&](flecs::entity rel) {
            if (!rel.is_valid() || listed.contains(rel.id()) || ecs_has_id(w, rel.id(), EcsTarget))
            {
                return;
            }
            const char* n = rel.name();
            const QString path = entityPath(rel);
            if (!n || n[0] == '\0' || path.isEmpty() || isFlecsBuiltinPath(path))
            {
                return;
            }
            Entry entry;
            entry.path = path;
            const flecs::Component* cd = rel.try_get<flecs::Component>();
            entry.tag = !cd || cd->size <= 0;
            entry.id = static_cast<qulonglong>(rel.id());
            entry.pair = ecs_has_id(w, rel.id(), EcsRelationship) || !ecs_id_in_use(w, rel.id()) ? Use::Only : Use::Also;
            fillTargets(rel, entry);
            listed.insert(rel.id());
            out.append(entry);
        };
        mw.query_builder().with(flecs::Relationship).build().each([&](flecs::entity rel) { addRelationEntity(rel); });
        for (const ecs_entity_t id : declared)
        {
            addRelationEntity(flecs::entity(w, id));
        }

        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
        return out;
    }

} // namespace rpe

#include "rpe/ecs/HealthCheck.h"

#include "rpe/core/TypeBridge.h"
#include "rpe/core/TypeRenderer.h"
#include "rpe/ecs/ComponentScan.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <map>

namespace rpe
{

    namespace
    {
        using Sev = HealthIssue::Severity;

        HealthIssue issue(Sev sev, const char* check, const QString& subject, const QString& message,
                          const QString& fix = QString())
        {
            return HealthIssue { sev, QString::fromLatin1(check), subject, message, fix };
        }

        QString typeName(rttr::type t)
        {
            return QString::fromStdString(t.get_name().to_string());
        }

        const char* severityLabel(Sev s)
        {
            switch (s)
            {
            case Sev::Error:
                return "ERROR";
            case Sev::Warning:
                return "WARNING";
            default:
                return "INFO";
            }
        }

        // Every type that appears INSIDE a bridged type — as a property, a container
        // element, an optional's value — recursively. Such a type is a VALUE type
        // (Vec3 in Transform::pos, std::optional<int> in Body::tag): RTTR knows it
        // so the editor can show it as a field, and flecs may know it as a
        // component for its own reasons, but it was never meant to be bridged.
        QSet<rttr::type::type_id> valueTypesOfBridged()
        {
            QSet<rttr::type::type_id> seen;
            QVector<rttr::type> work;
            const auto add = [&](rttr::type t) {
                if (t.is_valid() && !seen.contains(t.get_id()))
                {
                    seen.insert(t.get_id());
                    work.append(t);
                }
            };
            const auto addWithParts = [&](rttr::type t) {
                add(t);
                add(TypeRenderer::rawType(t));
                for (const rttr::type& a : t.get_template_arguments())
                {
                    add(a);
                }
            };
            for (const rttr::type& b : TypeBridge::registeredTypes())
            {
                for (const rttr::property& p : TypeRenderer::rawType(b).get_properties())
                {
                    addWithParts(p.get_type());
                }
            }
            for (int i = 0; i < work.size(); ++i) // grows while we walk
            {
                const rttr::type t = work[i]; // copy: `work` may reallocate below
                for (const rttr::property& p : TypeRenderer::rawType(t).get_properties())
                {
                    addWithParts(p.get_type());
                }
                for (const rttr::type& a : t.get_template_arguments())
                {
                    addWithParts(a);
                }
            }
            return seen;
        }

        // Standard-library types (std::optional<int>, std::string, …) are never
        // the application's components to bridge, whatever flecs registered.
        bool isStdPath(const QString& path)
        {
            return path.startsWith(QLatin1String("std.")) || path.startsWith(QLatin1String("std::"));
        }

        // Named, non-built-in component entities: what the browser can list.
        template <class Fn>
        void forEachComponent(const flecs::world& world, Fn&& fn)
        {
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
                const flecs::Component* cd = comp.try_get<flecs::Component>();
                fn(comp, path, QString::fromUtf8(cn), cd ? cd->size : 0);
            });
        }
    } // namespace

    // ── HealthReport ────────────────────────────────────────────────────────────

    int HealthReport::count(HealthIssue::Severity atLeast) const
    {
        return static_cast<int>(std::count_if(issues.cbegin(), issues.cend(), [&](const HealthIssue& i) {
            return static_cast<int>(i.severity) >= static_cast<int>(atLeast);
        }));
    }

    QVector<HealthIssue> HealthReport::byCheck(const QString& check) const
    {
        QVector<HealthIssue> out;
        for (const HealthIssue& i : issues)
        {
            if (i.check == check)
            {
                out.append(i);
            }
        }
        return out;
    }

    QVector<HealthIssue> HealthReport::about(const QString& subject) const
    {
        QVector<HealthIssue> out;
        for (const HealthIssue& i : issues)
        {
            if (i.subject == subject)
            {
                out.append(i);
            }
        }
        return out;
    }

    QString HealthReport::toText(HealthIssue::Severity minimum) const
    {
        QVector<HealthIssue> shown;
        for (const HealthIssue& i : issues)
        {
            if (static_cast<int>(i.severity) >= static_cast<int>(minimum))
            {
                shown.append(i);
            }
        }
        if (shown.isEmpty())
        {
            return QStringLiteral("No issues.");
        }
        std::stable_sort(shown.begin(), shown.end(), [](const HealthIssue& a, const HealthIssue& b) {
            return static_cast<int>(a.severity) > static_cast<int>(b.severity);
        });
        QStringList blocks;
        for (const HealthIssue& i : shown)
        {
            QString b = QStringLiteral("[%1] %2 — %3\n    %4")
                            .arg(QString::fromLatin1(severityLabel(i.severity)), i.check, i.subject, i.message);
            if (!i.fix.isEmpty())
            {
                b += QStringLiteral("\n    fix: ") + i.fix;
            }
            blocks << b;
        }
        return blocks.join(QStringLiteral("\n\n"));
    }

    // ── Registry ────────────────────────────────────────────────────────────────

    HealthReport checkTypeRegistry()
    {
        HealthReport r;
        std::map<std::string, QStringList> byName; // ordered → stable report
        for (const rttr::type& t : TypeBridge::registeredTypes())
        {
            byName[t.get_name().to_string()] << typeName(t);
            if (t.get_properties().empty())
            {
                r.issues.append(issue(Sev::Info, "no-properties", typeName(t),
                                      QStringLiteral("Bridged, but RTTR exposes no properties: the component lists "
                                                     "and can be added/removed, but shows no fields."),
                                      QStringLiteral("Intended for marker components. Otherwise register its "
                                                     "properties with RTTR (in the same module that calls "
                                                     "registerType).")));
            }
        }
        for (const auto& [name, types] : byName)
        {
            if (types.size() > 1)
            {
                r.issues.append(issue(
                    Sev::Warning, "duplicate-rttr-name", QString::fromStdString(name),
                    QStringLiteral("%1 bridged types are registered with RTTR under this same name — a name "
                                   "lookup can't tell them apart.")
                        .arg(types.size()),
                    QStringLiteral("Register each under its full namespaced name, or give each flecs component "
                                   "an explicit alias: registerType<T>(\"flecs::path\") / bindComponent<T>(world).")));
            }
        }
        return r;
    }

    // ── flecs build ─────────────────────────────────────────────────────────────

    HealthReport checkFlecsBuild()
    {
        HealthReport r;
        const ecs_build_info_t* bi = ecs_get_build_info();
        if (!bi)
        {
            return r;
        }
        const QString compiled = QStringLiteral("%1.%2.%3")
                                     .arg(FLECS_VERSION_MAJOR)
                                     .arg(FLECS_VERSION_MINOR)
                                     .arg(FLECS_VERSION_PATCH);
        const QString running = QStringLiteral("%1.%2.%3").arg(bi->version_major).arg(bi->version_minor).arg(bi->version_patch);
        if (compiled != running)
        {
            r.issues.append(issue(Sev::Error, "flecs-version-mismatch", QStringLiteral("flecs"),
                                  QStringLiteral("rpe was compiled against flecs %1, but the process runs flecs %2. "
                                                 "Two flecs versions in one process mismatch headers/ABI — expect "
                                                 "crashes that make no sense.")
                                      .arg(compiled, running),
                                  QStringLiteral("Build rpe against the application's flecs: add rpe with "
                                                 "add_subdirectory after your flecs::flecs target exists, or "
                                                 "configure it with -DFETCHCONTENT_SOURCE_DIR_FLECS=<your flecs>.")));
        }
        if (bi->debug)
        {
            r.issues.append(issue(Sev::Info, "flecs-debug-build", QStringLiteral("flecs"),
                                  QStringLiteral("flecs %1 is a debug build (asserts on, slower).").arg(running)));
        }
        return r;
    }

    // ── Components ──────────────────────────────────────────────────────────────

    HealthReport checkComponents(const flecs::world& world)
    {
        HealthReport r;
        QHash<rttr::type::type_id, QStringList> boundBy; // RTTR type → components binding it
        QHash<rttr::type::type_id, rttr::type> boundType;
        const QSet<rttr::type::type_id> valueTypes = valueTypesOfBridged();

        forEachComponent(world, [&](flecs::entity comp, const QString& path, const QString& leaf, int32_t size) {
            const TypeBridge::ResolveExplanation why = TypeBridge::explainResolve(path.toStdString());
            using Via = TypeBridge::ResolveExplanation::Via;

            if (why.via != Via::None)
            {
                const rttr::type t = why.type;
                boundBy[t.get_id()] << path;
                boundType.insert(t.get_id(), t);

                // Size: a tag (size 0) legitimately binds an empty struct (sizeof 1).
                const size_t rttrSize = t.get_sizeof();
                const bool sizeMismatch = size > 0 && rttrSize > 0 && static_cast<size_t>(size) != rttrSize;
                if (sizeMismatch)
                {
                    r.issues.append(issue(
                        Sev::Error, "size-mismatch", path,
                        QStringLiteral("The component is %1 bytes but binds %2, which is %3 bytes — its values "
                                       "would be read and written through the wrong layout.")
                            .arg(size)
                            .arg(typeName(t))
                            .arg(static_cast<qulonglong>(rttrSize)),
                        QStringLiteral("Bind the right type explicitly: registerType<T>(\"%1\") or "
                                       "rpe::bindComponent<T>(world).")
                            .arg(path)));
                }

                const QString cands = QString::fromStdString(
                    [&] {
                        std::string s;
                        for (const std::string& c : why.candidates)
                            s += (s.empty() ? "" : ", ") + c;
                        return s;
                    }());
                if ((why.via == Via::ShortName || why.via == Via::ScopedSuffix) && why.candidates.size() > 1)
                {
                    r.issues.append(issue(
                        Sev::Warning, "ambiguous-name", path,
                        QStringLiteral("Its name matches %1 bridged types (%2); %3 was picked by a tie-break.")
                            .arg(static_cast<int>(why.candidates.size()))
                            .arg(cands, typeName(t)),
                        QStringLiteral("Make the match exact: registerType<T>(\"%1\") or "
                                       "rpe::bindComponent<T>(world).")
                            .arg(path)));
                }
                else if (why.via == Via::ShortName && !sizeMismatch) // the Error already says it all
                {
                    r.issues.append(issue(
                        Sev::Info, "short-name-only", path,
                        QStringLiteral("Bound to %1 through its short name only. Works today, but a second type "
                                       "with the same short name would make it ambiguous.")
                            .arg(typeName(t)),
                        QStringLiteral("registerType<T>(\"%1\") makes it exact.").arg(path)));
                }
                return;
            }

            if (size <= 0 || isStdPath(path))
            {
                return; // an unbridged TAG is addable without a bridge; std:: types aren't components
            }

            // Not bridged. Only say something when RTTR knows a candidate type that
            // isn't simply a VALUE type of some bridged component (see above) — the
            // false positives an automatic warning used to produce: "Vec3",
            // "std::optional<int>", … known to RTTR as fields, to flecs as components.
            QString full = path;
            full.replace(QLatin1Char('.'), QStringLiteral("::"));
            const rttr::type exact = rttr::type::get_by_name(full.toStdString());
            if (exact.is_valid() && !TypeBridge::has(exact))
            {
                if (valueTypes.contains(exact.get_id()))
                {
                    return;
                }
                // Carried by entities → it IS used as a component, and invisible:
                // a warning. Unused so far → it may still be meant for the Add menu,
                // but nothing is visibly wrong yet: a note.
                const int32_t users = ecs_count_id(world.c_ptr(), comp.id());
                if (users > 0)
                {
                    r.issues.append(issue(Sev::Warning, "unbridged-rttr-type", path,
                                          QStringLiteral("%1 entit%2 carry it and RTTR knows %3, but TypeBridge "
                                                         "doesn't — it is neither shown nor offered in the Add menu.")
                                              .arg(users)
                                              .arg(users == 1 ? QStringLiteral("y") : QStringLiteral("ies"))
                                              .arg(typeName(exact)),
                                          QStringLiteral("Call rpe::TypeBridge::registerType<%1>() (in the same "
                                                         "rpe_core the host uses).")
                                              .arg(typeName(exact))));
                }
                else
                {
                    r.issues.append(issue(Sev::Info, "unbridged-rttr-type", path,
                                          QStringLiteral("RTTR knows %1 and flecs has it as a component, but no "
                                                         "entity carries it and TypeBridge doesn't know it.")
                                              .arg(typeName(exact)),
                                          QStringLiteral("If it's meant to be added from the Add menu, call "
                                                         "rpe::TypeBridge::registerType<%1>().")
                                              .arg(typeName(exact))));
                }
                return;
            }
            // A same-SHORT-name type is only a hint — and only if that type isn't
            // already bridged (then it belongs to some other component), isn't a
            // value type of one, and the component is actually in use.
            const rttr::type byLeaf = rttr::type::get_by_name(leaf.toStdString());
            if (byLeaf.is_valid() && !TypeBridge::has(byLeaf) && !valueTypes.contains(byLeaf.get_id())
                && ecs_count_id(world.c_ptr(), comp.id()) > 0)
            {
                r.issues.append(issue(Sev::Info, "unbridged-maybe", path,
                                      QStringLiteral("Not bridged. RTTR has an unbridged type with the same short "
                                                     "name (%1) — if it is this component's type, it isn't "
                                                     "registered with TypeBridge.")
                                          .arg(typeName(byLeaf)),
                                      QStringLiteral("If so: rpe::TypeBridge::registerType<%1>(\"%2\").")
                                          .arg(typeName(byLeaf), path)));
            }
        });

        for (auto it = boundBy.cbegin(); it != boundBy.cend(); ++it)
        {
            if (it.value().size() > 1)
            {
                QStringList comps = it.value();
                comps.sort();
                r.issues.append(issue(
                    Sev::Warning, "same-type-twice", typeName(boundType.value(it.key(), rttr::type::get<void>())),
                    QStringLiteral("%1 components bind this one type (%2) — at most one of them really is it; "
                                   "the others show the wrong schema.")
                        .arg(comps.size())
                        .arg(comps.join(QStringLiteral(", "))),
                    QStringLiteral("Bind each component to its own type with registerType<T>(\"flecs::path\") "
                                   "or rpe::bindComponent<T>(world).")));
            }
        }

        QStringList unused;
        for (const rttr::type& t : TypeBridge::registeredTypes())
        {
            if (!boundBy.contains(t.get_id()))
            {
                unused << typeName(t);
            }
        }
        unused.sort();
        for (const QString& name : unused)
        {
            r.issues.append(issue(Sev::Info, "unused-bridge", name,
                                  QStringLiteral("Bridged, but no component in this world binds it."),
                                  QStringLiteral("Expected if its plugin isn't loaded or it's only used inside "
                                                 "other types. Otherwise its flecs name doesn't match: "
                                                 "registerType<T>(\"flecs::path\") or bindComponent<T>(world).")));
        }
        return r;
    }

    // ── Required component ─────────────────────────────────────────────────────

    HealthReport checkRequiredComponent(const flecs::world& world, const QString& required)
    {
        HealthReport r;
        if (required.isEmpty())
        {
            return r;
        }
        const ComponentNameMatch m = matchComponentName(world, required);
        using Via = ComponentNameMatch::Via;
        if (m.via == Via::None)
        {
            const bool scoped = required.contains(QLatin1Char('.')) || required.contains(QStringLiteral("::"));
            r.issues.append(issue(Sev::Warning, "required-not-found", required,
                                  QStringLiteral("No component matches — the entity list stays empty until one "
                                                 "is registered.%1")
                                      .arg(scoped ? QStringLiteral(" A scoped name only matches that scope.")
                                                  : QString()),
                                  QStringLiteral("Use the component's full path as flecs knows it.")));
            return r;
        }
        if (m.ambiguous())
        {
            r.issues.append(issue(Sev::Warning, "required-ambiguous", required,
                                  QStringLiteral("%1 components match (%2); \"%3\" is used.")
                                      .arg(m.candidates.size())
                                      .arg(m.candidates.join(QStringLiteral(", ")), m.candidates.first()),
                                  QStringLiteral("Give the full path to choose.")));
        }
        else if (m.via != Via::FullPath)
        {
            r.issues.append(issue(Sev::Info, "required-inexact", required,
                                  QStringLiteral("Matched \"%1\" by %2, not by its full path.")
                                      .arg(m.candidates.first(),
                                           m.via == Via::ScopeSuffix ? QStringLiteral("scope suffix")
                                                                     : QStringLiteral("short name")),
                                  QStringLiteral("Use \"%1\" to make it exact.").arg(m.candidates.first())));
        }
        return r;
    }

    // ── Prefab group tags ───────────────────────────────────────────────────────

    HealthReport checkPrefabGroups(const flecs::world& world, const QStringList& tags)
    {
        HealthReport r;
        flecs::world& w = const_cast<flecs::world&>(world);
        for (const QString& tag : tags)
        {
            const flecs::entity te = w.lookup(tag.toUtf8().constData());
            if (!te.is_valid())
            {
                r.issues.append(issue(Sev::Warning, "group-tag-not-found", tag,
                                      QStringLiteral("world.lookup() can't find this tag, so its prefabs stay "
                                                     "ungrouped.%1")
                                          .arg(tag.contains(QLatin1Char('.')) && !tag.contains(QStringLiteral("::"))
                                                   ? QStringLiteral(" A dotted path is never resolved.")
                                                   : QString()),
                                      QStringLiteral("Pass the tag's full path with \"::\" (\"game::npc::Enemy\") "
                                                     "or a world.use() alias.")));
                continue;
            }
            const flecs::entity_t gid = te.raw_id();
            int onPrefabs = 0;
            bool asPair = false;
            w.query_builder().with(flecs::Prefab).build().each([&](flecs::entity p) {
                onPrefabs += p.has(gid) ? 1 : 0;
                asPair = asPair || p.has(gid, flecs::Wildcard) || p.has(flecs::Wildcard, gid);
            });
            if (onPrefabs > 0)
            {
                continue;
            }
            int onInstances = 0;
            w.query_builder().with(gid).build().each([&](flecs::entity) { ++onInstances; });
            QString why;
            if (onInstances > 0)
            {
                why += QStringLiteral(" It IS on %1 non-prefab entit%2 — added to instances instead?")
                           .arg(onInstances)
                           .arg(onInstances == 1 ? QStringLiteral("y") : QStringLiteral("ies"));
            }
            if (asPair)
            {
                why += QStringLiteral(" A prefab uses it as a PAIR — a pair id is not the tag id.");
            }
            r.issues.append(issue(Sev::Warning, "group-tag-on-no-prefab", tag,
                                  QStringLiteral("The tag exists but no prefab carries it, so no prefab is "
                                                 "grouped under it.")
                                      + why,
                                  QStringLiteral("Add the tag to the PREFAB entity itself (instances inherit it "
                                                 "through is_a).")));
        }
        return r;
    }

    // ── All of it ───────────────────────────────────────────────────────────────

    HealthReport checkHealth(const flecs::world& world, const HealthCheckOptions& options)
    {
        HealthReport r = checkFlecsBuild();
        r.append(checkTypeRegistry());
        r.append(checkComponents(world));
        r.append(checkRequiredComponent(world, options.requiredComponent));
        r.append(checkPrefabGroups(world, options.prefabGroupTags));
        return r;
    }

} // namespace rpe

#include "rpe/ecs/ComponentScan.h"

#include "rpe/core/TypeBridge.h"

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
            if (!includeBuiltins && path.startsWith(QStringLiteral("flecs")))
            {
                return;
            }

            ComponentResolution r;
            r.name = QString::fromUtf8(raw);
            r.path = path;
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

} // namespace rpe

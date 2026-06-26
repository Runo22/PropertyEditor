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
            const flecs::string fullPath = comp.path(".", ".");
            const char* pc = fullPath.c_str();
            const QString path = pc ? QString::fromUtf8(pc) : QString();
            if (!includeBuiltins && path.startsWith(QStringLiteral("flecs")))
            {
                return;
            }

            ComponentResolution r;
            r.name = QString::fromUtf8(raw);
            r.path = path;
            const rttr::type t = TypeBridge::resolveByName(raw);
            r.bridged = t.is_valid();
            if (r.bridged)
            {
                r.rttrType = QString::fromStdString(t.get_name().to_string());
            }
            out.push_back(std::move(r));
        });

        return out;
    }

} // namespace rpe

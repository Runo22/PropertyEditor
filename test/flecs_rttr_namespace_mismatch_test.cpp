// The real-world configuration: RTTR types are registered with their TRUE namespaced
// names, while the flecs components are registered under DIFFERENT namespaces. The two
// names then have nothing in common but the leaf — so name-similarity resolution is
// forced onto the ambiguous leaf and silently picks the first matching type.
//
// bindComponent<T>(world) fixes this at the root: it asks FLECS for the component
// entity it already has for T and registers that exact path as the alias, so the
// mapping is derived, never guessed.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentRegistry.h>

#include <rttr/registration.h>

#include <cstdio>

namespace game
{
    struct Stats
    {
        int hp = 0;
    };
}
namespace ai
{
    struct Stats
    {
        double aggression = 0;
    };
}

RTTR_REGISTRATION
{
    // Registered AS-IS, with the real namespaces (what the user actually does).
    rttr::registration::class_<game::Stats>("game::Stats").property("hp", &game::Stats::hp);
    rttr::registration::class_<ai::Stats>("ai::Stats").property("aggression", &ai::Stats::aggression);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

int main()
{
    flecs::world world;

    // flecs side uses DIFFERENT namespaces than RTTR ("render.*" / "logic.*").
    world.component<game::Stats>("render.Stats");
    world.component<ai::Stats>("logic.Stats");

    const auto gamePath = world.component<game::Stats>().path(".", "");
    const auto aiPath = world.component<ai::Stats>().path(".", "");
    printf("  flecs paths: game::Stats -> \"%s\", ai::Stats -> \"%s\"\n",
           gamePath.c_str(), aiPath.c_str());

    // Bind through flecs, so the alias is the component's ACTUAL path.
    rpe::bindComponents<game::Stats, ai::Stats>(world);

    const rttr::type g = rpe::TypeBridge::resolveByName(gamePath.c_str());
    const rttr::type a = rpe::TypeBridge::resolveByName(aiPath.c_str());
    check("flecs path with a DIFFERENT namespace resolves to game::Stats",
          g == rttr::type::get<game::Stats>());
    check("...and the other one to ai::Stats", a == rttr::type::get<ai::Stats>());
    check("the two mismatched paths resolve to DIFFERENT types", g != a);

    // The resolved types really are the distinct schemas (not one shared by both).
    check("game::Stats keeps its own property (hp)",
          g.get_property("hp").is_valid() && !g.get_property("aggression").is_valid());
    check("ai::Stats keeps its own property (aggression)",
          a.get_property("aggression").is_valid() && !a.get_property("hp").is_valid());

    // Binding is idempotent and never creates a second flecs component for T.
    const auto beforeId = world.component<game::Stats>().id();
    rpe::bindComponent<game::Stats>(world);
    check("re-binding reuses the same flecs component id",
          world.component<game::Stats>().id() == beforeId);
    check("re-binding keeps resolution correct",
          rpe::TypeBridge::resolveByName(gamePath.c_str()) == rttr::type::get<game::Stats>());

    // ── Binding BEFORE flecs knows the type must not register or rename it ──────
    // Registering T under a default name here and letting the caller name it later is
    // exactly what makes flecs raise a component-redefinition error.
    {
        struct Late
        {
            int v = 0;
        };
        flecs::world w2;
        const flecs::entity notYet = rpe::bindComponent<Late>(w2);
        check("binding an unregistered type is a no-op (no flecs registration)",
              !notYet.is_valid());
        check("...so flecs still doesn't know it", !flecs::_::type<Late>::registered(w2.c_ptr()));

        // The caller's own naming therefore still succeeds, and binding after it works.
        w2.component<Late>("custom.Late");
        const flecs::entity bound = rpe::bindComponent<Late>(w2);
        check("naming it afterwards still works, and binding then succeeds", bound.is_valid());
        const auto latePath = w2.component<Late>().path(".", "");
        check("the caller's name is what resolves",
              rpe::TypeBridge::resolveByName(latePath.c_str()) == rttr::type::get<Late>());
    }

    // ── One unregistered type must not abort binding of the rest ────────────────
    {
        struct Unknown
        {
            int v = 0;
        };
        flecs::world w3;
        w3.component<game::Stats>("only.Stats");
        rpe::bindComponents<Unknown, game::Stats>(w3); // Unknown first, deliberately
        const auto p = w3.component<game::Stats>().path(".", "");
        check("a skipped type doesn't prevent later ones from binding",
              rpe::TypeBridge::resolveByName(p.c_str()) == rttr::type::get<game::Stats>());
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

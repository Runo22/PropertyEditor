// Exercises TypeBridge name resolution (scoped RTTR name vs. flecs short name,
// plus explicit aliases) and the scanComponents() diagnostic. Also a smoke check
// that the wrap hook actually produces a usable RTTR instance from a void*.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentScan.h>

#include <rttr/registration.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace game
{
    struct Transform
    {
        double x = 0, y = 0;
    };
} // namespace game

// A type whose flecs name won't share a short name with its RTTR registration.
struct Health
{
    int hp = 100;
};

// Same LEAF ("Panel") in two namespaces — must resolve by full path, not ambiguously.
namespace game
{
    struct Panel
    {
        int a = 0;
    };
}
namespace ui
{
    struct Panel
    {
        int b = 0;
    };
}
// A DEEPER namespace than the flecs path carries — exercises the scoped-suffix match.
namespace app
{
    namespace gfx
    {
        struct Sprite
        {
            int s = 0;
        };
    }
}

RTTR_REGISTRATION
{
    // Scoped RTTR name; flecs will report the short name "Transform".
    rttr::registration::class_<game::Transform>("game::Transform")
        .property("x", &game::Transform::x)
        .property("y", &game::Transform::y);

    // Registered under a name that does NOT match the flecs component name.
    rttr::registration::class_<Health>("rpg::HealthComponent")
        .property("hp", &Health::hp);

    rttr::registration::class_<game::Panel>("game::Panel").property("a", &game::Panel::a);
    rttr::registration::class_<ui::Panel>("ui::Panel").property("b", &ui::Panel::b);
    rttr::registration::class_<app::gfx::Sprite>("app::gfx::Sprite").property("s", &app::gfx::Sprite::s);
}

static int g_fails = 0;
static void check(const char* name, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_fails;
}

int main()
{
    rpe::TypeBridge::registerType<game::Transform>();
    // Map the flecs component name "Health" onto the RTTR type "rpg::HealthComponent".
    rpe::TypeBridge::registerType<Health>("Health");
    rpe::TypeBridge::registerTypes<game::Panel, ui::Panel, app::gfx::Sprite>();

    // 1) Short-name resolution: flecs "Transform" → RTTR "game::Transform".
    const rttr::type t1 = rpe::TypeBridge::resolveByName("Transform");
    check("scoped RTTR type resolves from flecs short name",
          t1.is_valid() && t1 == rttr::type::get<game::Transform>());

    // 2) Explicit alias resolution: flecs "Health" → RTTR "rpg::HealthComponent".
    const rttr::type t2 = rpe::TypeBridge::resolveByName("Health");
    check("aliased type resolves from explicit flecs name",
          t2.is_valid() && t2 == rttr::type::get<Health>());

    // 3) Unknown name resolves to invalid.
    check("unknown component resolves to invalid", !rpe::TypeBridge::resolveByName("Nope").is_valid());

    // 3b) The string_view API accepts std::string / std::string_view directly (no
    //     .c_str()), and an empty view is invalid — not UB.
    const std::string healthStr = "Health";
    const std::string_view healthView = healthStr;
    check("resolveByName accepts std::string", rpe::TypeBridge::resolveByName(healthStr) == rttr::type::get<Health>());
    check("resolveByName accepts std::string_view", rpe::TypeBridge::resolveByName(healthView) == rttr::type::get<Health>());
    check("resolveByName(empty) is invalid", !rpe::TypeBridge::resolveByName(std::string_view {}).is_valid());

    // 3c) Same-leaf types in different namespaces resolve by FULL PATH (no ambiguity).
    check("full path resolves game::Panel",
          rpe::TypeBridge::resolveByName("game.Panel") == rttr::type::get<game::Panel>());
    check("full path resolves ui::Panel (not the other Panel)",
          rpe::TypeBridge::resolveByName("ui.Panel") == rttr::type::get<ui::Panel>());

    // 3d) Scoped-SUFFIX: a flecs path shallower than the RTTR namespace still
    //     disambiguates ("gfx.Sprite" → "app::gfx::Sprite").
    check("scoped-suffix resolves app::gfx::Sprite from \"gfx.Sprite\"",
          rpe::TypeBridge::resolveByName("gfx.Sprite") == rttr::type::get<app::gfx::Sprite>());

    // 3e) Bare-leaf lookup of an ambiguous name is DETERMINISTIC (same every call /
    //     build) and valid — never an unordered_map coin-flip.
    const rttr::type p1 = rpe::TypeBridge::resolveByName("Panel");
    const rttr::type p2 = rpe::TypeBridge::resolveByName("Panel");
    check("ambiguous leaf resolves to a valid type", p1.is_valid());
    check("ambiguous leaf resolution is deterministic", p1 == p2);

    // 4) wrap() yields a working RTTR instance over a live object.
    game::Transform tf { 3.0, 4.0 };
    rttr::variant access = rpe::TypeBridge::wrap(t1, &tf);
    check("wrap() produces a valid variant", access.is_valid());
    rttr::instance inst(access);
    const rttr::property px = t1.get_property("x");
    check("read property through wrapped void*", px.is_valid() && px.get_value(inst).to_double() == 3.0);
    px.set_value(inst, 9.0);
    check("write property through wrapped void* mutates the object", tf.x == 9.0);

    // 5) scanComponents over a world: both components present, both resolved; a
    //    component with no RTTR registration shows up as not bridged.
    flecs::world world;
    world.entity("Player").set<game::Transform>({}).set<Health>({});
    struct Unregistered
    {
        int v = 0;
    };
    world.entity("Ghost").set<Unregistered>({});

    const auto comps = rpe::scanComponents(world);
    bool sawTransform = false, sawHealth = false, sawUnregistered = false;
    for (const auto& c : comps)
    {
        if (c.name == QStringLiteral("Transform"))
            sawTransform = c.bridged && c.rttrType == QStringLiteral("game::Transform");
        if (c.name == QStringLiteral("Health"))
            sawHealth = c.bridged && c.rttrType == QStringLiteral("rpg::HealthComponent");
        if (c.name == QStringLiteral("Unregistered"))
            sawUnregistered = !c.bridged; // present but not inspectable
    }
    check("scan: Transform listed + bridged", sawTransform);
    check("scan: Health listed + bridged via alias", sawHealth);
    check("scan: unregistered component listed as NOT bridged", sawUnregistered);

    // 6) Built-in flecs components are excluded by default.
    bool sawFlecsBuiltin = false;
    for (const auto& c : comps)
    {
        if (c.path.startsWith(QStringLiteral("flecs")))
            sawFlecsBuiltin = true;
    }
    check("scan: flecs built-ins excluded by default", !sawFlecsBuiltin);

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

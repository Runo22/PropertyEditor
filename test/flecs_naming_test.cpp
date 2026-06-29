// Diagnostic: what does flecs name a C++-namespaced component, and does
// TypeBridge::resolveByName() resolve it given various RTTR registration names?
// Mirrors the user's setup (RTTR registered with a SHORT name vs a FULL name).
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentScan.h>

#include <rttr/registration.h>

#include <QString>

#include <cstdio>

namespace game
{
    struct Transform
    {
        double x = 1;
    }; // RTTR registered SHORT: "Transform"
    struct Velocity
    {
        double v = 2;
    }; // RTTR registered FULL:  "game::Velocity"
} // namespace game
struct Health
{
    int hp = 3;
}; // global type, RTTR "Health"

// Collision: two different components whose SHORT name is the same ("Panel").
namespace ui
{
    struct Panel
    {
        int a = 1;
    };
}
namespace hud
{
    struct Panel
    {
        double x = 1, y = 2;
    };
}

RTTR_REGISTRATION
{
    rttr::registration::class_<game::Transform>("Transform").property("x", &game::Transform::x);
    rttr::registration::class_<game::Velocity>("game::Velocity").property("v", &game::Velocity::v);
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
    // Registered with FULL names so the path-based resolver can disambiguate
    // (both share the leaf "Panel").
    rttr::registration::class_<ui::Panel>("ui::Panel").property("a", &ui::Panel::a);
    rttr::registration::class_<hud::Panel>("hud::Panel").property("x", &hud::Panel::x).property("y", &hud::Panel::y);
}

int main()
{
    rpe::TypeBridge::registerType<game::Transform>();
    rpe::TypeBridge::registerType<game::Velocity>();
    rpe::TypeBridge::registerType<Health>();
    rpe::TypeBridge::registerType<ui::Panel>();
    rpe::TypeBridge::registerType<hud::Panel>();

    flecs::world world;
    auto e = world.entity("E").set<game::Transform>({}).set<game::Velocity>({}).set<Health>({});

    printf("flecs component identity (what the inspector sees):\n");
    e.each([&](flecs::id id) {
        if (!id.is_entity())
            return;
        flecs::entity c = id.entity();
        const char* nm = c.name();
        if (!nm || nm[0] == '\0')
            return;
        const flecs::string sym = c.symbol();
        const rttr::type t = rpe::TypeBridge::resolveByName(nm);
        printf("  name()=\"%-14s\" symbol=\"%-22s\" path=\"%s\"\n", nm,
               sym.c_str() ? sym.c_str() : "", c.path(".", "").c_str());
        printf("      resolveByName(name) -> %s  (rttr=\"%s\", props=%zu)\n",
               t.is_valid() ? "VALID" : "INVALID",
               t.is_valid() ? t.get_name().to_string().c_str() : "-",
               t.is_valid() ? t.get_properties().size() : 0u);
    });

    printf("\nscanComponents() catalog view:\n");
    for (const auto& c : rpe::scanComponents(world))
        printf("  name=\"%s\" path=\"%s\" bridged=%d rttr=\"%s\"\n",
               c.name.toUtf8().constData(), c.path.toUtf8().constData(), c.bridged,
               c.rttrType.toUtf8().constData());

    printf("\nSHORT-NAME COLLISION (ui::Panel vs hud::Panel both registered \"Panel\"):\n");
    printf("  rttr::get<ui::Panel>  props=%zu name=\"%s\"\n",
           rttr::type::get<ui::Panel>().get_properties().size(),
           rttr::type::get<ui::Panel>().get_name().to_string().c_str());
    printf("  rttr::get<hud::Panel> props=%zu name=\"%s\"\n",
           rttr::type::get<hud::Panel>().get_properties().size(),
           rttr::type::get<hud::Panel>().get_name().to_string().c_str());
    auto show = [](const char* q) {
        const rttr::type t = rpe::TypeBridge::resolveByName(q);
        printf("  resolveByName(\"%s\") -> rttr=\"%s\" props=%zu\n", q,
               t.is_valid() ? t.get_name().to_string().c_str() : "INVALID",
               t.is_valid() ? t.get_properties().size() : 0u);
    };
    show("Panel");      // leaf — AMBIGUOUS (first match)
    show("ui.Panel");   // full path — should resolve ui::Panel (props=1)
    show("hud.Panel");  // full path — should resolve hud::Panel (props=2)

    return 0;
}

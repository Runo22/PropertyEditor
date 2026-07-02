// Diagnostic: does calling rttr::type::get<T>() (as TypeBridge::registerType does)
// BEFORE rttr::registration::class_<T>(...) drop the registration's name/props?
// Also: does runtime (non-RTTR_REGISTRATION) registration work at all, and what
// happens when a custom short name is used?
#include <rpe/core/TypeBridge.h>

#include <rttr/registration.h>
#include <rttr/type.h>

#include <cstdio>
#include <string>

namespace demo
{
    struct Position
    {
        double x = 1, y = 2;
    };
    struct Velocity
    {
        double dx = 3, dy = 4;
    };
    struct Health
    {
        int hp = 5;
    };
} // namespace demo

static void dump(const char* label, const rttr::type& t)
{
    printf("  %-22s valid=%d name=\"%s\" props=%zu\n", label, t.is_valid(),
           t.get_name().to_string().c_str(), t.get_properties().size());
}

int main()
{
    // ── Case A: bridge/get<T>() FIRST, then runtime class_ with a SHORT name.
    printf("[A] type::get<T>() before registration, custom SHORT name:\n");
    rttr::type a0 = rttr::type::get<demo::Position>(); // what TypeBridge::registerType does
    dump("before class_", a0);
    rttr::registration::class_<demo::Position>("Position")
        .property("x", &demo::Position::x)
        .property("y", &demo::Position::y);
    dump("after class_(short)", rttr::type::get<demo::Position>());
    printf("    get_by_name(\"Position\") props=%zu\n",
           rttr::type::get_by_name("Position").get_properties().size());

    // ── Case B: registration FIRST (no prior get<T>()), custom SHORT name.
    printf("[B] registration first, custom SHORT name:\n");
    rttr::registration::class_<demo::Velocity>("Velocity")
        .property("dx", &demo::Velocity::dx)
        .property("dy", &demo::Velocity::dy);
    dump("after class_(short)", rttr::type::get<demo::Velocity>());

    // ── Case C: bridge/get<T>() FIRST, then class_ with the FULL (namespaced) name.
    printf("[C] type::get<T>() before registration, FULL namespaced name:\n");
    rttr::type c0 = rttr::type::get<demo::Health>();
    dump("before class_", c0);
    rttr::registration::class_<demo::Health>("demo::Health").property("hp", &demo::Health::hp);
    dump("after class_(full)", rttr::type::get<demo::Health>());

    return 0;
}

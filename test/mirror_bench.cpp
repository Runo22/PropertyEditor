// Benchmark: how much does an attached EcsMirror cost per frame, and where?
// Reports ms/frame for the bare world vs. mirror-attached (idle) vs. mirror with
// an active interest, under set_threads(4).
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

struct Position
{
    double x = 0, y = 0, z = 0;
};
struct Velocity
{
    double dx = 1, dy = 1, dz = 1;
};
struct Tag
{
    int v = 0;
};

// Many extra registered types, to stress resolveByName's per-call cost (a large
// registry is the realistic "hundreds of components" case that made the old
// per-entity-component resolveByName catastrophic).
template <int I>
struct Dummy
{
    int a = 0;
};
template <int... Is>
static void registerDummies(std::integer_sequence<int, Is...>)
{
    (rttr::registration::class_<Dummy<Is>>(("Dummy" + std::to_string(Is)).c_str()).property("a", &Dummy<Is>::a), ...);
}

RTTR_REGISTRATION
{
    rttr::registration::class_<Position>("Position").property("x", &Position::x).property("y", &Position::y).property("z", &Position::z);
    rttr::registration::class_<Velocity>("Velocity").property("dx", &Velocity::dx).property("dy", &Velocity::dy).property("dz", &Velocity::dz);
    rttr::registration::class_<Tag>("Tag").property("v", &Tag::v);
    registerDummies(std::make_integer_sequence<int, 200>()); // 200 extra types
}

static double timeFrames(flecs::world& w, int frames)
{
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    for (int i = 0; i < frames; ++i)
        w.progress(0.016f);
    const auto t1 = clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / frames;
}

int main(int argc, char** argv)
{
    const int N = argc > 1 ? std::atoi(argv[1]) : 2000;
    const int F = argc > 2 ? std::atoi(argv[2]) : 120;

    rpe::TypeBridge::registerTypes<Position, Velocity, Tag>();
    // Bridge the 200 dummies too, so the registry is realistically large.
    [&]<int... Is>(std::integer_sequence<int, Is...>) {
        (rpe::TypeBridge::registerType<Dummy<Is>>(), ...);
    }(std::make_integer_sequence<int, 200>());

    flecs::world world;
    world.set_threads(4);

    flecs::entity first;
    for (int i = 0; i < N; ++i)
    {
        auto e = world.entity();
        e.set<Position>({ double(i), 0, 0 }).set<Velocity>({});
        if (i % 3 == 0)
            e.set<Tag>({ i });
        if (i == 0)
            first = e;
    }
    const auto eid = static_cast<qulonglong>(first.id());
    world.system<Position, const Velocity>("move").each([](Position& p, const Velocity& v) { p.x += v.dx; });

    printf("world: %d entities, set_threads(4), %d frames/measurement\n", N, F);
    timeFrames(world, 20); // warm up

    const double base = timeFrames(world, F);
    printf("  baseline (no mirror)          : %7.3f ms/frame  (%.0f fps)\n", base, 1000.0 / base);

    rpe::EcsMirror mirror;
    mirror.attach(&world);
    const double idle = timeFrames(world, F);
    printf("  mirror attached, no interest  : %7.3f ms/frame  (%.0f fps)\n", idle, 1000.0 / idle);

    mirror.setInterest(eid, "Position", { "x", "y", "z" });
    const double active = timeFrames(world, F);
    printf("  mirror attached, with interest: %7.3f ms/frame  (%.0f fps)\n", active, 1000.0 / active);

    // Filtered view: only ~1/3 of entities have Tag → the narrowed query should
    // visit far fewer entities.
    mirror.setRequiredComponent("Tag");
    timeFrames(world, 12); // let the filter apply / query rebuild
    const double filtered = timeFrames(world, F);
    printf("  mirror + required filter (Tag): %7.3f ms/frame  (%.0f fps)\n", filtered, 1000.0 / filtered);

    printf("  overhead: idle=%.3f ms  active=%.3f ms  filtered=%.3f ms per frame\n",
           idle - base, active - base, filtered - base);

    mirror.detach();
    return 0;
}

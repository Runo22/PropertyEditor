// Benchmark: how much does an attached EcsMirror cost per frame, and where?
// Reports ms/frame for the bare world vs. mirror-attached (idle) vs. mirror with
// an active interest, under set_threads(4).
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <cmath>
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

RTTR_REGISTRATION
{
    rttr::registration::class_<Position>("Position").property("x", &Position::x).property("y", &Position::y).property("z", &Position::z);
    rttr::registration::class_<Velocity>("Velocity").property("dx", &Velocity::dx).property("dy", &Velocity::dy).property("dz", &Velocity::dz);
    rttr::registration::class_<Tag>("Tag").property("v", &Tag::v);
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

    flecs::world world;
    world.set_threads(4);

    flecs::entity first;
    for (int i = 0; i < N; ++i)
    {
        auto e = world.entity();
        // Per-entity varying velocity so the heavy loop below is data-dependent and
        // the optimiser can't fold it away.
        e.set<Position>({ double(i), 0, 0 }).set<Velocity>({ 0.5 + double(i % 97), 1.0, 1.0 });
        if (i % 3 == 0)
            e.set<Tag>({ i });
        if (i == 0)
            first = e;
    }
    const auto eid = static_cast<qulonglong>(first.id());
    // Several HEAVY multi-threaded systems, so set_threads(4) actually parallelises
    // real work — this is what exposes a sync-barrier stall from the mirror.
    for (int s = 0; s < 4; ++s)
    {
        world.system<Position, const Velocity>(("move" + std::to_string(s)).c_str())
            .multi_threaded()
            .each([](Position& p, const Velocity& v) { p.x += v.dx; });
    }

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

    // Worst case: the GUI raises resync EVERY frame (re-selection / auto-select).
    // Before the fix this forced a full entity scan per frame (the 2-3 fps stall);
    // now it is a cheap re-publish of the cached lists.
    mirror.setRequiredComponent(""); // back to the unfiltered (heaviest) list
    auto ch = mirror.channel();
    {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        for (int i = 0; i < F; ++i)
        {
            ch->requestResync();
            world.progress(0.016f);
        }
        const double resync = std::chrono::duration<double, std::milli>(clock::now() - t0).count() / F;
        printf("  mirror + resync EVERY frame   : %7.3f ms/frame  (%.0f fps)\n", resync, 1000.0 / resync);
    }

    printf("  overhead: idle=%.3f ms  active=%.3f ms  filtered=%.3f ms per frame\n",
           idle - base, active - base, filtered - base);

    mirror.detach();
    return 0;
}

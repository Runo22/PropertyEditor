// Reproduces the runtime-plugin scenario for rpe::EcsMirror: attach() is called
// from INSIDE a running system (world is readonly / mid-progress), on the sim
// thread, while the world is already advancing. This is what crashed before the
// deferred-install fix. Verifies the mirror installs (via ecs_run_post_frame) and
// then mirrors values across the thread boundary without crashing.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

struct Vec3
{
    double x = 0, y = 0, z = 0;
};
struct Comp
{
    double mass = 1.0;
    Vec3 pos;
};

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<Vec3>("Vec3").property("x", &Vec3::x).property("y", &Vec3::y).property("z", &Vec3::z);
    registration::class_<Comp>("Comp").property("mass", &Comp::mass).property("pos", &Comp::pos);
}

int main()
{
    rpe::TypeBridge::registerType<Comp>();

    flecs::world world;
    auto e = world.entity("E1");
    e.set<Comp>({});
    const auto eid = static_cast<qulonglong>(e.id());

    rpe::EcsMirror mirror;
    std::atomic<bool> running { true };
    std::atomic<bool> attachRequested { false };
    std::atomic<bool> attachDone { false };

    // A bootstrap system that, the first time it runs (DURING progress → world is
    // readonly), calls mirror.attach(). This is the runtime-plugin-load case.
    world.system("bootstrap").kind(flecs::OnUpdate).run([&](flecs::iter& it) {
        while (it.next()) {}
        if (attachRequested.load() && !attachDone.load())
        {
            mirror.attach(&world); // called inside progress(): readonly → deferred install
            mirror.setInterest(eid, "Comp", { "mass", "pos.x" });
            attachDone.store(true);
        }
    });

    // ── simulation thread ─────────────────────────────────────────────────────
    std::thread sim([&] {
        double t = 0.0;
        while (running.load(std::memory_order_relaxed))
        {
            if (auto* c = e.try_get_mut<Comp>())
            {
                t += 0.1;
                c->mass = 10.0 + t;
            }
            world.progress(0.016f);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    });

    // Let the sim run a bit, THEN request the in-frame attach (runtime plugin load).
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    attachRequested.store(true);

    int massSamples = 0;
    double lastMass = -1.0;
    for (int i = 0; i < 250; ++i)
    {
        for (auto& u : mirror.pollValues())
        {
            if (u.path == "mass")
            {
                const double m = u.value.to_double();
                if (m != lastMass)
                {
                    lastMass = m;
                    ++massSamples;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    running.store(false, std::memory_order_relaxed);
    sim.join();
    mirror.detach();

    int fails = 0;
    auto check = [&](const char* n, bool ok) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", n); if (!ok) ++fails; };
    check("attach() ran from inside progress() (readonly)", attachDone.load());
    check("mirror installed + mirrored live values after in-frame attach", massSamples > 5);

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}

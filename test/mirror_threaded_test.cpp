// Validates rpe::EcsMirror under MULTI-THREADED flecs progress (set_threads):
// the mirror's per-frame task runs while flecs splits work across worker threads
// and uses stages. pump() must use the iterator's stage world for entity ops so
// flecs does not assert / race. Verifies mirroring works and nothing crashes.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

struct Comp
{
    double mass = 1.0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Comp>("Comp").property("mass", &Comp::mass);
}

int main()
{
    rpe::TypeBridge::registerType<Comp>();

    flecs::world world;
    world.set_threads(4); // multi-threaded progress: workers + stages

    // Several entities so the mutating system actually runs across workers.
    flecs::entity first;
    for (int i = 0; i < 64; ++i)
    {
        auto e = world.entity((std::string("E") + std::to_string(i)).c_str());
        e.set<Comp>({ static_cast<double>(i) });
        if (i == 0)
            first = e;
    }
    const auto eid = static_cast<qulonglong>(first.id());

    // A multi-threaded system that mutates Comp (forces staging/worker activity).
    world.system<Comp>("bump").each([](Comp& c) { c.mass += 1.0; });

    rpe::EcsMirror mirror;
    mirror.attach(&world);
    mirror.setInterest(eid, "Comp", { "mass" });

    std::atomic<bool> running { true };
    std::thread sim([&] {
        for (int i = 0; i < 120 && running.load(); ++i)
        {
            world.progress(0.016f);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        running.store(false);
    });

    int massSamples = 0;
    double last = -1.0;
    while (running.load())
    {
        for (auto& u : mirror.pollValues())
        {
            if (u.path == "mass")
            {
                const double m = u.value.to_double();
                if (m != last)
                {
                    last = m;
                    ++massSamples;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    sim.join();
    mirror.detach();

    int fails = 0;
    auto check = [&](const char* n, bool ok) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", n); if (!ok) ++fails; };
    check("survived multi-threaded progress (no crash/assert)", true);
    check("mirrored values under multi-threading", massSamples > 5);

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}

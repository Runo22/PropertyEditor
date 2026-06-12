// Lifetime safety for rpe::EcsMirror / MirrorChannel: the GUI consumer holds a
// shared_ptr to the channel and keeps polling it AFTER the EcsMirror is destroyed
// on the sim thread (the real shutdown / plugin-removal order). Must not crash or
// use freed memory; polls just return nothing once the producer is gone.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/MirrorChannel.h>

#include <rttr/registration.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
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
    auto e = world.entity("E1");
    e.set<Comp>({});
    const auto eid = static_cast<qulonglong>(e.id());

    // GUI side keeps the channel; sim side owns the mirror.
    std::shared_ptr<rpe::MirrorChannel> channel;

    std::atomic<bool> running { true };
    std::atomic<bool> mirrorDestroyed { false };

    std::thread sim([&] {
        auto mirror = std::make_unique<rpe::EcsMirror>();
        channel = mirror->channel(); // hand the channel to the GUI side
        mirror->attach(&world);
        mirror->setInterest(eid, "Comp", { "mass" });

        double t = 0.0;
        for (int i = 0; i < 60; ++i)
        {
            if (auto* c = e.try_get_mut<Comp>())
            {
                t += 0.1;
                c->mass = 10.0 + t;
            }
            world.progress(0.016f);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }

        // Destroy the mirror on the SIM thread, while the GUI side still polls.
        mirror.reset();
        mirrorDestroyed.store(true);

        // Keep advancing the world a bit after the producer is gone.
        for (int i = 0; i < 30; ++i)
        {
            world.progress(0.016f);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        running.store(false);
    });

    // GUI thread: poll continuously across the mirror's destruction.
    int pollsAfterDestroy = 0;
    bool sawValueBefore = false;
    while (running.load())
    {
        if (channel)
        {
            for (auto& u : channel->pollValues())
            {
                if (u.path == "mass")
                {
                    sawValueBefore = true;
                }
            }
            channel->setInterest(eid, "Comp", { "mass" }); // also exercise the write side
            if (mirrorDestroyed.load())
            {
                ++pollsAfterDestroy;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }

    sim.join();

    int fails = 0;
    auto check = [&](const char* n, bool ok) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", n); if (!ok) ++fails; };
    check("mirrored values before destroy", sawValueBefore);
    check("kept polling channel after mirror destroyed (no crash)", pollsAfterDestroy > 5);
    check("channel reports producer gone", channel && !channel->producerAlive());

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}

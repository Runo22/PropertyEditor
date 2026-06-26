// Reproduces the "runtime tears down before the editor" crash: an EcsMirror is
// destroyed on the MAIN thread while a separate sim thread keeps calling
// world.progress() (set_threads, the per-frame pump actively running). The sim
// loop is NOT stopped first — destruction races the in-flight pump head-on.
//
// This isolates the TEARDOWN race (the reported problem). attach() itself is a
// structural world change and is done once up front, before the sim loop starts
// (attach() is sim-thread/quiescent-only by contract). Only destruction races
// progress() here.
//
// The mirror must:
//   • never let an in-flight pump touch the half-destroyed object (the pump calls
//     into rpe_core: TypeBridge / TypeRenderer / RttrBridge), and
//   • not destruct its system from the wrong thread while that system's callback
//     may be executing — instead neutralise and let the world reap it.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

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

// One destroy-while-progressing trial. Returns after the world has been advanced
// well past the mirror's death, so a crash in the leftover pump would surface.
static void trial(int seedFrames)
{
    flecs::world world;
    world.set_threads(4);

    flecs::entity first;
    for (int i = 0; i < 64; ++i)
    {
        auto e = world.entity((std::string("E") + std::to_string(i)).c_str());
        e.set<Comp>({ static_cast<double>(i) });
        if (i == 0)
            first = e;
    }
    const auto eid = static_cast<qulonglong>(first.id());
    world.system<Comp>("bump").each([](Comp& c) { c.mass += 1.0; });

    auto mirror = std::make_unique<rpe::EcsMirror>();
    mirror->attach(&world);             // quiescent: no sim thread yet
    mirror->setInterest(eid, "Comp", { "mass" });

    // Seed a few single-threaded frames so the pump records its (this) thread.
    for (int i = 0; i < seedFrames; ++i)
    {
        world.progress(0.016f);
    }

    // Now hand progress() to a sim thread and let it run continuously.
    std::atomic<bool> running { true };
    std::thread sim([&] {
        while (running.load(std::memory_order_acquire))
        {
            world.progress(0.016f);
        }
    });

    // Let the pump run concurrently for a bit, draining like a GUI would.
    for (int k = 0; k < 5; ++k)
    {
        (void)mirror->pollValues();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // The dangerous moment: destroy on THIS (non-sim) thread while sim progresses.
    mirror.reset();

    // Keep progressing after the mirror is gone: a leftover inert system must
    // read only valid memory (its token), never the dead `this`.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    running.store(false, std::memory_order_release);
    sim.join();
    for (int i = 0; i < 10; ++i)
    {
        world.progress(0.016f);
    }
}

int main()
{
    rpe::TypeBridge::registerType<Comp>();

    // Repeat with different seed timings to vary how the destroy interleaves with
    // the sim thread's frame boundary.
    for (int round = 0; round < 40; ++round)
    {
        trial(round % 4);
    }

    printf("[PASS] destroyed mirror mid-progress on a non-sim thread x40 (no crash)\n");
    printf("ALL PASS\n");
    return 0;
}

// Teardown-safety for rpe::EcsMirror, exercised deterministically by performing
// the structural ops from INSIDE a system (world is readonly / mid-progress):
//   #1  detach() an installed mirror while readonly.
//   #2  destroy the mirror in the SAME frame as a deferred (readonly) attach,
//       before the deferred install runs.
// Both must complete without crashing.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <atomic>
#include <cstdio>
#include <memory>

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

    enum class Act
    {
        None,
        DetachInFrame,
        AttachThenKillInFrame
    };
    std::atomic<int> act { static_cast<int>(Act::None) };
    std::unique_ptr<rpe::EcsMirror> m1;
    std::unique_ptr<rpe::EcsMirror> m2;

    // Runs DURING progress (readonly). Performs the dangerous structural ops here.
    world.system("control").kind(flecs::PostUpdate).run([&](flecs::iter& it) {
        while (it.next())
        {
        }
        const Act a = static_cast<Act>(act.exchange(static_cast<int>(Act::None)));
        if (a == Act::DetachInFrame && m1)
        {
            m1->detach(); // #1: detach while readonly → deferred teardown
        }
        else if (a == Act::AttachThenKillInFrame && m2)
        {
            m2->attach(&world); // deferred install (readonly)
            m2.reset();         // #2: destroy same frame, before frame-end
        }
    });

    auto progressN = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
        }
    };

    // ── #1: install between frames, run, then detach mid-frame ────────────────
    m1 = std::make_unique<rpe::EcsMirror>();
    m1->attach(&world);
    m1->setInterest(eid, "Comp", { "mass" });
    progressN(5);
    act = static_cast<int>(Act::DetachInFrame);
    progressN(5); // detach happens readonly; teardown runs at frame-end
    m1.reset();   // final cleanup (between frames)

    // ── #2: deferred attach + same-frame destroy ──────────────────────────────
    m2 = std::make_unique<rpe::EcsMirror>();
    act = static_cast<int>(Act::AttachThenKillInFrame);
    progressN(5); // attach(deferred) + reset in one frame; install must be skipped

    progressN(5); // keep advancing; no dangling system may run

    printf("[PASS] in-readonly detach + deferred-attach-then-destroy survived\n");
    printf("ALL PASS\n");
    return 0;
}

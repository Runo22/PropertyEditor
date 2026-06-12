// Headless concurrency test for rpe::EcsMirror.
//   sim thread : runs world.progress() in a loop, mutating components — NO lock.
//   gui thread : polls mirrored values and queues an edit — never touches world.
// Verifies: live values mirror across the thread boundary, a queued edit reaches
// the real component, and nothing races/crashes.
#include <rpe/ecs/EcsMirror.h>
#include <rpe/core/TypeBridge.h>

#include <rttr/registration.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

struct Vec3 { double x = 0, y = 0, z = 0; };
struct Comp { double mass = 1.0; Vec3 pos; int editable = 0; };

RTTR_REGISTRATION {
    using namespace rttr;
    registration::class_<Vec3>("Vec3")
        .property("x", &Vec3::x).property("y", &Vec3::y).property("z", &Vec3::z);
    registration::class_<Comp>("Comp")
        .property("mass", &Comp::mass).property("pos", &Comp::pos)
        .property("editable", &Comp::editable);
}

int main()
{
    rpe::TypeBridge::registerType<Comp>();

    flecs::world world;
    auto e = world.entity("E1");
    e.set<Comp>({});
    const auto eid = static_cast<qulonglong>(e.id());

    rpe::EcsMirror mirror;
    mirror.attach(&world);                       // registers per-frame system
    mirror.setInterest(eid, "Comp", { "mass", "pos.x", "editable" });

    std::atomic<bool> running{true};

    // ── simulation thread: mutate the world, NO mutex around the loop ─────────
    std::thread sim([&] {
        double t = 0.0;
        while (running.load(std::memory_order_relaxed)) {
            if (auto* c = e.try_get_mut<Comp>()) { // sim-thread-only world access
                t += 0.1;
                c->mass    = 10.0 + t;
                c->pos.x   = t * 2.0;
            }
            world.progress(0.016f);                // mirror system runs here
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    });

    // ── gui thread (this thread): poll + edit, never touch the world ──────────
    int    massSamples = 0;
    bool   sawEdit     = false;
    bool   editQueued  = false;
    double lastMass    = -1.0;

    for (int i = 0; i < 250; ++i) {
        for (auto& u : mirror.pollValues()) {
            if (u.path == "mass") {
                const double m = u.value.to_double();
                if (m != lastMass) { lastMass = m; ++massSamples; }
            }
            if (u.path == "editable" && u.value.to_int() == 777)
                sawEdit = true;
        }
        if (i == 100 && !editQueued) {
            mirror.queueEdit("editable", rttr::variant(777));   // GUI -> sim
            editQueued = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    running.store(false, std::memory_order_relaxed);
    sim.join();
    mirror.detach();

    // After join it is safe to read the world directly.
    const int finalEditable = e.try_get<Comp>()->editable;

    int fails = 0;
    auto check = [&](const char* n, bool ok) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", n); if (!ok) ++fails; };

    check("mirrored live mass changes (>5 distinct samples)", massSamples > 5);
    check("queued edit observed via mirror (editable==777)",  sawEdit);
    check("queued edit applied to real component",            finalEditable == 777);

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}

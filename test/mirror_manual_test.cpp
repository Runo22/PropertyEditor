// Verifies PumpMode::Manual (no per-frame system → no immediate() sync barrier)
// and the wall-clock rate cap. In Manual mode the host calls pump() after
// progress(); values must still mirror. In System mode a system IS registered.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <cstdio>

struct Comp
{
    double mass = 3.0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Comp>("Comp").property("mass", &Comp::mass);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

int main()
{
    rpe::TypeBridge::registerType<Comp>();

    // ── System mode: a system IS registered. ─────────────────────────────────
    {
        flecs::world w;
        w.set_threads(4);
        rpe::EcsMirror m;
        m.attach(&w); // default = System
        w.progress(0.016f);
        check("System mode registers the rpe::EcsMirror system", w.lookup("rpe::EcsMirror").is_valid());

        // The rate cap must gate SYSTEM-mode pumps too: a tight progress() loop
        // (well under 1/30 s of wall clock) may execute at most a couple of pumps;
        // the rest must be counted as skipped. pumpStats() exposes both.
        m.setMaxPumpRateHz(30.0);
        for (int i = 0; i < 50; ++i)
        {
            w.progress(0.001f);
        }
        const auto s = m.pumpStats();
        check("System mode: rate cap skips most pumps", s.pumps <= 5 && s.skipped >= 40);
        check("pump stats record durations", s.pumps == 0 || s.maxPumpMs > 0.0);
        m.detach();
    }

    // ── Manual mode: NO system; pump() drives it. ─────────────────────────────
    flecs::world world;
    world.set_threads(4);
    auto e = world.entity("E").set<Comp>({ 42.0 });
    const auto eid = static_cast<qulonglong>(e.id());

    rpe::EcsMirror mirror;
    mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);
    check("Manual mode registers NO system (no per-frame barrier)", !world.lookup("rpe::EcsMirror").is_valid());

    mirror.setInterest(eid, "Comp", { "mass" });

    // The loop the host would run: progress(), then pump() (world is merged, workers
    // idle → safe, no barrier).
    double seen = -1;
    for (int i = 0; i < 10; ++i)
    {
        world.progress(0.016f);
        mirror.pump();
        for (auto& u : mirror.pollValues())
            if (u.path == "mass")
                seen = u.value.to_double();
    }
    check("Manual pump mirrors the value (mass==42)", seen == 42.0);

    // ── Rate cap: with a 1 Hz cap, rapid pump() calls mostly skip. ────────────
    mirror.setMaxPumpRateHz(1.0);
    e.set<Comp>({ 7.0 });     // change the value
    mirror.clearInterest();   // reset dedup path
    mirror.setInterest(eid, "Comp", { "mass" });
    int updates = 0;
    for (int i = 0; i < 20; ++i)
    {
        world.progress(0.016f);
        mirror.pump(); // called 20x in a tight loop; the 1 Hz cap allows ~1
        updates += static_cast<int>(mirror.pollValues().size());
    }
    check("rate cap throttles rapid pumps (<=2 updates for 20 calls)", updates <= 2 && updates >= 1);

    mirror.detach();

    // ── Temporary-wrapper attach (plugin pattern) ─────────────────────────────
    // A plugin typically receives a raw ecs_world_t* and wraps it in a STACK
    // flecs::world that dies right after attach() returns. The mirror must store
    // the underlying world pointer, never the wrapper's address — otherwise the
    // frame-end pump dereferences a dangling wrapper (access violation in
    // ecs_get_world, e.g. from scanComponents).
    {
        flecs::world host; // the engine-owned world (outlives everything)
        host.set_threads(4);
        auto he = host.entity("H").set<Comp>({ 5.5 });
        rpe::EcsMirror m2;
        {
            flecs::world tempWrapper(host.c_ptr()); // scoped wrapper: dies below
            m2.attach(&tempWrapper);                // default System mode
        } // tempWrapper destroyed here — before any pump ran
        m2.setInterest(static_cast<qulonglong>(he.id()), "Comp", { "mass" });
        double got = -1;
        for (int i = 0; i < 10; ++i)
        {
            host.progress(0.016f); // frame-end pump must not touch the dead wrapper
            for (auto& u : m2.pollValues())
                if (u.path == "mass")
                    got = u.value.to_double();
        }
        check("attach via temporary wrapper survives + mirrors (mass==5.5)", got == 5.5);
        m2.detach();
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

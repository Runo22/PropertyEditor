// Incremental (budgeted) entity scan: a big world is labelled in slices across
// pumps instead of one monolithic pass; a zero budget restores single-shot.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <cstdio>

struct Marker
{
    int v = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Marker>("Marker").property("v", &Marker::v);
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
    rpe::TypeBridge::registerType<Marker>();

    flecs::world world;
    constexpr int N = 3000;
    for (int i = 0; i < N; ++i)
    {
        world.entity().set<Marker>({ i });
    }

    rpe::EcsMirror mirror;
    mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);
    mirror.setScanIntervalsMs(0, 1000000); // entity cycles back-to-back; no catalog noise
    // Microscopic budget → the 32-entity progress guarantee dominates: a cycle
    // needs ~N/32 pumps, so completion MUST span many pumps.
    mirror.setScanBudgetMsPerPump(0.000001);

    QVector<rpe::EcsMirror::EntityEntry> ents;
    world.progress(0.016f);
    mirror.pump();
    check("first pump does NOT publish (cycle incomplete)", !mirror.pollEntities(ents));

    int pumps = 1;
    bool published = false;
    for (; pumps < 400 && !published; ++pumps)
    {
        world.progress(0.016f);
        mirror.pump();
        published = mirror.pollEntities(ents);
    }
    check("cycle completes across pumps and publishes", published);
    check("published list is complete (all N entities)", ents.size() == N);
    check("completion genuinely spanned many pumps", pumps > 10);
    printf("  (cycle took %d pumps; lastScanMs=%.3f)\n", pumps, mirror.pumpStats().lastScanMs);

    // ── Zero budget → single-shot: a list change publishes after ONE pump ─────
    // (The incremental cycle above just completed, so no cycle is in flight.)
    mirror.setScanBudgetMsPerPump(0.0);
    world.entity("Straggler").set<Marker>({ 9999 }); // change the set
    world.progress(0.016f);
    mirror.pump(); // begin + complete in the same pump
    check("zero budget completes a cycle in one pump",
          mirror.pollEntities(ents) && ents.size() == N + 1);

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

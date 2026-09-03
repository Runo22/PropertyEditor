// A required-component filter narrows the entity list. If the SELECTED entity stops
// passing the filter through a DIRECT world edit (its required component removed, or
// the entity destroyed) — one the mirror never routed as a structural change — the
// list must drop it PROMPTLY, within a pump or two, not only on the next wall-clock
// scan (~0.5s later). Otherwise a non-matching entity stays selected with its
// components on show.
//
// Manual pump + a realistic (non-zero) scan interval make this deterministic: only
// the prompt-rescan path can drop the entity within a few quick pumps; the ordinary
// wall-clock scan cannot (no real time elapses between the pumps).
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <QApplication>

#include <cstdio>

struct Position
{
    float x = 0;
};
struct Velocity
{
    float v = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Position>("Position").property("x", &Position::x);
    rttr::registration::class_<Velocity>("Velocity").property("v", &Velocity::v);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

using Entry = rpe::MirrorChannel::EntityEntry;

static bool listed(rpe::EcsMirror& m, qulonglong id)
{
    QVector<Entry> ents;
    m.pollEntities(ents); // latest published set
    for (const auto& e : ents)
        if (e.id == id)
            return true;
    return false;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    rpe::TypeBridge::registerTypes<Position, Velocity>();

    flecs::world world;
    auto alpha = world.entity("Alpha").set<Position>({ 1 }).set<Velocity>({ 9 });
    world.entity("Beta").set<Position>({ 2 });
    world.entity("Gamma").set<Position>({ 3 });
    const auto aid = static_cast<qulonglong>(alpha.id());

    rpe::EcsMirror mirror;
    mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);
    mirror.setScanIntervalsMs(500, 2000); // realistic gate: quick pumps never trip it
    auto ch = mirror.channel();
    auto pump = [&] { world.progress(0.016f); mirror.pump(); };

    // Filter to entities with Velocity, and select Alpha (the only match).
    ch->setRequiredComponent(QStringLiteral("Velocity"));
    mirror.setInterest(aid, QStringLiteral("Position"), { QStringLiteral("x") });
    pump(); // required changed → scan → list = [Alpha]
    check("filter=Velocity: Alpha is listed", listed(mirror, aid));

    // ── Direct world edit: remove Velocity from the SELECTED Alpha ──────────────
    // No structural queue, no filter change — only the per-pump interest check can
    // notice. A couple of quick pumps (well under the 0.5s scan gap) must drop it.
    alpha.remove<Velocity>();
    pump(); // interest check sees Alpha fails the filter → forces a rescan
    pump(); // forced rescan runs → Alpha dropped from the list
    check("removing the required component drops the selected entity promptly", !listed(mirror, aid));
    {
        QStringList comps;
        const bool got = mirror.pollComponents(comps);
        check("its component panel feed is cleared, not left on stale rows", !got || comps.isEmpty());
    }

    // ── Re-add Velocity → Alpha qualifies again and re-lists ───────────────────
    alpha.set<Velocity>({ 5 });
    mirror.setInterest(aid, QStringLiteral("Position"), { QStringLiteral("x") });
    pump();
    pump();
    check("re-adding the required component re-lists the entity", listed(mirror, aid));

    // ── Direct destroy of the selected entity → dropped promptly too ───────────
    alpha.destruct();
    pump();
    pump();
    check("destroying the selected entity drops it promptly", !listed(mirror, aid));

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

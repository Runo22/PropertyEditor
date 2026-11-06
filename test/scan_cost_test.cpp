// Periodic sim-thread stalls from name resolution.
//
// Reported: an uncapped sim drops to ~2 fps at regular intervals. The add-component
// catalog walked EVERY component in the world every 2 s, unsliced, on the sim thread
// — and resolved each name against the TypeBridge registry, where a MISS (the
// common case: most flecs components have no RTTR bridge) walked the whole registry
// three times allocating per entry. 3000 components cost ~0.5 s per scan in RELEASE.
//
// Pins down:
//   • resolveByName is memoised, and the memo can never serve a stale answer
//     (registration/alias/unregister all invalidate it);
//   • the catalog is rescanned when the component set changes — and ONLY then;
//   • a scan over thousands of unbridged components is cheap once warm.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentScan.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <QCoreApplication>

#include <chrono>
#include <cstdio>
#include <string>

struct Late
{
    int v = 0;
};
struct Health
{
    int hp = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Late>("game::Late").property("v", &Late::v);
    rttr::registration::class_<Health>("game::Health").property("hp", &Health::hp);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    rpe::TypeBridge::registerType<Health>();

    // ── The memo never serves a stale answer ────────────────────────────────────
    {
        check("an unregistered name misses", !rpe::TypeBridge::resolveByName("game.Late").is_valid());
        check("...and the (memoised) miss is repeatable", !rpe::TypeBridge::resolveByName("game.Late").is_valid());

        rpe::TypeBridge::registerType<Late>();
        check("registering the type invalidates a memoised MISS",
              rpe::TypeBridge::resolveByName("game.Late") == rttr::type::get<Late>());

        rpe::TypeBridge::registerAlias(rttr::type::get<Late>(), "legacy.LateName");
        check("a new alias resolves immediately", rpe::TypeBridge::resolveByName("legacy.LateName") == rttr::type::get<Late>());

        rpe::TypeBridge::unregisterType<Late>();
        check("unregistering invalidates a memoised HIT", !rpe::TypeBridge::resolveByName("game.Late").is_valid());
        check("...including through its alias", !rpe::TypeBridge::resolveByName("legacy.LateName").is_valid());

        rpe::TypeBridge::registerType<Late>(); // back, for the mirror section below
        check("re-registering resolves again", rpe::TypeBridge::resolveByName("game::Late") == rttr::type::get<Late>());
    }

    // ── A scan over thousands of UNBRIDGED components is cheap once warm ───────
    {
        flecs::world w;
        for (int i = 0; i < 3000; ++i)
        {
            w.entity(("sim::Comp" + std::to_string(i)).c_str()).set<flecs::Component>({ 4, 4 });
        }
        using clk = std::chrono::steady_clock;
        rpe::scanComponents(w); // cold: fills the memo
        const auto t0 = clk::now();
        const auto res = rpe::scanComponents(w);
        const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        printf("  warm scan of %zu components: %.2f ms\n", res.size(), ms);
        // Generous bound (debug builds, loaded CI): the old path was ~500 ms in release.
        check("a warm scan of 3000 unbridged components stays well under 100 ms", ms < 100.0);
    }

    // ── The catalog rescans on a component-set change, and ONLY then ───────────
    {
        flecs::world w;
        w.component<Health>("game::Health");
        w.entity("Hero").set<Health>({ 1 });

        rpe::EcsMirror mirror;
        mirror.attach(&w);
        mirror.setScanIntervalsMs(0, 0); // every OTHER scan runs every pump — worst case
        auto pump = [&](int n) {
            for (int i = 0; i < n; ++i)
                w.progress(0.016f);
        };

        pump(5);
        const auto scansAfterStart = mirror.pumpStats().catalogScans;
        check("the catalog is scanned at start", scansAfterStart >= 1);

        pump(100);
        check("100 idle pumps (even with 0 ms intervals) do NOT rescan the catalog",
              mirror.pumpStats().catalogScans == scansAfterStart);

        QVector<rpe::MirrorChannel::CatalogEntry> cat;
        mirror.channel()->pollCatalogEntries(cat);
        bool hasLate = false;
        for (const auto& e : cat)
            hasLate = hasLate || e.path.endsWith(QStringLiteral("Late"));
        check("a component type not yet in the world is not in the catalog", !hasLate);

        // A new component TYPE appears at runtime (plugin load): the catalog follows.
        w.component<Late>("game::Late");
        pump(3);
        check("registering a component type triggers a rescan",
              mirror.pumpStats().catalogScans > scansAfterStart);
        cat.clear();
        mirror.channel()->pollCatalogEntries(cat);
        for (const auto& e : cat)
            hasLate = hasLate || e.path.endsWith(QStringLiteral("Late"));
        check("...and the new component is offered", hasLate);

        mirror.detach();
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

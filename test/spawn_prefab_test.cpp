// Add-entity / prefab spawning through the mirror:
//   • the producer lists spawnable prefabs, filtered by the required component
//     (reusing the entity-list filter) and grouped by the host's group tags;
//   • queueSpawnPrefab instantiates a new entity via is_a and then runs the host's
//     sim-thread configurator so it can set/override components on the instance.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <QApplication>

#include <cstdio>

struct Health
{
    int hp = 100;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

using Prefab = rpe::MirrorChannel::PrefabEntry;

static const Prefab* find(const QVector<Prefab>& v, const QString& name)
{
    for (const auto& p : v)
        if (p.name == name)
            return &p;
    return nullptr;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerTypes<Health>();

    flecs::world world;
    auto enemy = world.entity("Enemy"); // group tags
    auto prop = world.entity("Prop");

    auto goblin = world.prefab("Goblin").set<Health>({ 100 });
    goblin.add(enemy);
    auto orc = world.prefab("Orc").set<Health>({ 200 });
    orc.add(enemy);
    auto chest = world.prefab("Chest").set<Health>({ 30 });
    chest.add(prop);
    auto decoration = world.prefab("Decoration"); // NO Health → filtered out
    (void) decoration;

    rpe::EcsMirror mirror;
    mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);
    mirror.setScanIntervalsMs(0, 0);
    mirror.setScanBudgetMsPerPump(0); // single-shot entity scan → _reqId resolves at once
    auto ch = mirror.channel();

    ch->setPrefabGroupTags({ QStringLiteral("Enemy"), QStringLiteral("Prop") });
    mirror.setRequiredComponent(QStringLiteral("Health"));

    // A couple of pumps: the first resolves the required filter id, the catalog-
    // cadence prefab scan then publishes.
    for (int i = 0; i < 3; ++i)
    {
        world.progress(0.016f);
        mirror.pump();
    }

    // ── 1. Prefab catalog: filtered by required + grouped by tag ────────────────
    QVector<Prefab> prefabs;
    check("prefab catalog published", ch->pollPrefabs(prefabs));
    check("Goblin listed under the Enemy group",
          find(prefabs, QStringLiteral("Goblin")) && find(prefabs, QStringLiteral("Goblin"))->group == QStringLiteral("Enemy"));
    check("Orc listed under the Enemy group",
          find(prefabs, QStringLiteral("Orc")) && find(prefabs, QStringLiteral("Orc"))->group == QStringLiteral("Enemy"));
    check("Chest listed under the Prop group",
          find(prefabs, QStringLiteral("Chest")) && find(prefabs, QStringLiteral("Chest"))->group == QStringLiteral("Prop"));
    check("Decoration filtered out (no Health)", find(prefabs, QStringLiteral("Decoration")) == nullptr);

    const Prefab* g = find(prefabs, QStringLiteral("Goblin"));
    check("prefab entry carries a spawn id", g && g->id != 0);

    // ── 2. Spawn: is_a + the sim-thread configurator override ──────────────────
    qulonglong spawnedId = 0;
    int seenBaseHp = -1;
    mirror.setSpawnConfigurator([&](flecs::entity e) {
        // The instance already inherits the prefab's Health (100) via is_a; the
        // configurator sees that and overrides it.
        seenBaseHp = e.get<Health>().hp;
        e.set<Health>({ 5 });
        spawnedId = static_cast<qulonglong>(e.id());
    });

    ch->queueSpawnPrefab(g->id);
    world.progress(0.016f);
    mirror.pump();

    check("configurator ran on the sim thread", spawnedId != 0);
    check("configurator saw the inherited prefab value (100)", seenBaseHp == 100);
    {
        flecs::entity ne = world.entity(static_cast<flecs::entity_t>(spawnedId));
        check("spawned entity is alive", ne.is_alive());
        check("spawned entity is_a the prefab", ne.has(flecs::IsA, goblin));
        check("configurator override took (hp == 5)", ne.is_alive() && ne.get<Health>().hp == 5);
    }

    // ── 3. A spawn with NO configurator still instantiates the prefab ──────────
    mirror.setSpawnConfigurator({});
    const Prefab* c = find(prefabs, QStringLiteral("Chest"));
    int chestCount0 = world.count(flecs::IsA, chest);
    ch->queueSpawnPrefab(c->id);
    world.progress(0.016f);
    mirror.pump();
    check("spawn without a configurator still creates an instance",
          world.count(flecs::IsA, chest) == chestCount0 + 1);

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

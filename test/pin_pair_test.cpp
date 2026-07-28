// Two fixes verified end-to-end through the mirror:
//   1. A data-carrying pair's property can be PINNED (resolved by pair id +
//      ecs_get_typeid on the producer, not by name).
//   2. Removing a component by its flecs id removes exactly that component —
//      never a different (e.g. the first) one.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <QApplication>

#include <cstdio>

struct Health
{
    int hp = 100;
};
struct Armor
{
    int def = 5;
};
struct Damage // carried by a pair (Damage, Fire)
{
    int amount = 10;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
    rttr::registration::class_<Armor>("Armor").property("def", &Armor::def);
    rttr::registration::class_<Damage>("Damage").property("amount", &Damage::amount);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

using Row = rpe::MirrorChannel::ComponentRow;
using Kind = rpe::MirrorChannel::RowKind;
using PinKey = rpe::MirrorChannel::PinKey;

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerTypes<Health, Armor, Damage>();

    flecs::world world;
    auto fire = world.entity("Fire");
    auto e = world.entity("E").set<Health>({ 70 }).set<Armor>({ 3 });
    e.set<Damage>(fire, { 10 }); // data-carrying pair

    rpe::EcsMirror mirror;
    mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);
    mirror.setScanIntervalsMs(0, 0);
    // Selecting the entity publishes its component listing (needed for the ids below).
    mirror.setInterest(static_cast<qulonglong>(e.id()), "Health", { "hp" });
    world.progress(0.016f);
    mirror.pump();
    auto ch = mirror.channel();

    // Grab the published rows to learn the pair id and the Health component id.
    QVector<Row> rows;
    ch->pollComponentRows(rows);
    qulonglong pairId = 0, healthId = 0, armorId = 0;
    for (const auto& r : rows)
    {
        if (r.kind == Kind::PairData && r.name == QStringLiteral("Damage"))
            pairId = r.rawId;
        if (r.kind == Kind::Data && r.name == QStringLiteral("Health"))
            healthId = r.rawId;
        if (r.kind == Kind::Data && r.name == QStringLiteral("Armor"))
            armorId = r.rawId;
    }
    check("pair row carries a flecs id", pairId != 0);
    check("data rows carry their component ids", healthId != 0 && armorId != 0);

    // ── 1. Pin the PAIR's property (rawId set → resolved by id, not by name) ────
    {
        PinKey key;
        key.entity = static_cast<qulonglong>(e.id());
        key.component = QStringLiteral("Damage (Fire)"); // unique per pair, not a flecs name
        key.path = QStringLiteral("amount");
        key.rawId = pairId;
        mirror.setPins({ key });
        world.progress(0.016f);
        mirror.pump();

        const auto pins = mirror.pollPinValues();
        check("pinned pair value flows (amount == 10)",
              !pins.empty() && pins[0].key.entity == key.entity && pins[0].value.to_int64() == 10);

        // Edit through the pin → reaches the exact pair instance on the world.
        mirror.queuePinEdit(key, rttr::variant(42));
        world.progress(0.016f);
        mirror.pump();
        check("pinned pair edit reached the world (amount == 42)", e.get<Damage>(fire).amount == 42);
    }

    // ── 2. Remove ONE component by id → only it goes, others survive ───────────
    {
        ch->queueStructuralById(rpe::MirrorChannel::StructuralKind::RemoveComponent,
                                static_cast<qulonglong>(e.id()), healthId);
        world.progress(0.016f);
        mirror.pump();
        check("removed component is gone", !e.has<Health>());
        check("the OTHER data component survived", e.has<Armor>() && e.get<Armor>().def == 3);
        check("the pair survived the unrelated removal", e.has<Damage>(fire));
    }

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

// A pinned watch must disappear when the thing it watches is deleted: the entity
// is destroyed, or the component is removed from it. A pin whose bridge type is
// merely not registered YET (plugin load order) must NOT be pruned.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/PinnedPropertiesWidget.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>

#include <cstdio>

struct Health
{
    int hp = 100;
};
struct Mana
{
    int mp = 50;
};
struct Later // registered in RTTR, bridged only midway (plugin load-order case)
{
    int v = 7;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
    rttr::registration::class_<Mana>("Mana").property("mp", &Mana::mp);
    rttr::registration::class_<Later>("Later").property("v", &Later::v);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

using PinKey = rpe::MirrorChannel::PinKey;

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerTypes<Health, Mana>(); // Later bridged later, on purpose

    flecs::world world;
    auto a = world.entity("A").set<Health>({ 70 });
    auto b = world.entity("B").set<Health>({ 30 }).set<Mana>({ 12 });
    auto c = world.entity("C").set<Later>({ 9 });
    const auto aid = static_cast<qulonglong>(a.id());
    const auto bid = static_cast<qulonglong>(b.id());
    const auto cid = static_cast<qulonglong>(c.id());

    rpe::EcsMirror mirror;
    mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);
    mirror.setScanIntervalsMs(0, 0);
    auto pump = [&] { world.progress(0.016f); mirror.pump(); };

    rpe::PinnedPropertiesWidget w;
    w.setChannel(mirror.channel());
    w.pin(aid, QStringLiteral("A"), QStringLiteral("Health"), QStringLiteral("hp"));  // entity to destroy
    w.pin(bid, QStringLiteral("B"), QStringLiteral("Mana"), QStringLiteral("mp"));    // component to remove
    w.pin(bid, QStringLiteral("B"), QStringLiteral("Health"), QStringLiteral("hp"));  // stays alive
    w.pin(cid, QStringLiteral("C"), QStringLiteral("Later"), QStringLiteral("v"));    // bridge pending
    pump();
    w.pollNow();
    check("all four pins are present initially", w.pins().size() == 4);

    // ── Destroy entity A → its pin is pruned ──────────────────────────────────
    a.destruct();
    pump();
    w.pollNow();
    check("destroying an entity removes its pinned row",
          !w.isPinned(aid, QStringLiteral("Health"), QStringLiteral("hp")));
    check("other pins are untouched by the destroy", w.pins().size() == 3);

    // ── Remove the Mana component from B → that pin is pruned, B/Health stays ──
    b.remove<Mana>();
    pump();
    w.pollNow();
    check("removing a component removes only that pinned row",
          !w.isPinned(bid, QStringLiteral("Mana"), QStringLiteral("mp")));
    check("the entity's other pinned component survives",
          w.isPinned(bid, QStringLiteral("Health"), QStringLiteral("hp")));
    check("pin count reflects exactly one more removal", w.pins().size() == 2);

    // ── The unbridged-yet pin (C/Later) must NOT be pruned — it's only pending ──
    check("a pin awaiting its bridge type is kept (not pruned)",
          w.isPinned(cid, QStringLiteral("Later"), QStringLiteral("v")));

    // And once the bridge registers, that pin resumes and shows its value.
    rpe::TypeBridge::registerType<Later>();
    mirror.channel()->requestResync();
    c.set<Later>({ 21 });
    pump();
    w.pollNow();
    check("the previously-pending pin is still present after its bridge registers",
          w.isPinned(cid, QStringLiteral("Later"), QStringLiteral("v")));

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

// Two entities each carry a DIFFERENT namespaced component that shares a leaf name
// ("Stats"). Switching entity WITHOUT re-clicking the component (the auto carry-over
// path) must bind the NEW entity's own component type — not keep the previous
// entity's same-leaf type (which would shift/garble the data).
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>
#include <rpe/gui/PropertyEditor.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QCoreApplication>
#include <QListWidget>
#include <QThread>

#include <cstdio>

namespace game
{
    struct Stats
    {
        int hp = 0;
        int mana = 0;
    };
}
namespace ai
{
    struct Stats
    {
        double aggression = 0;
    };
}
struct Zzz
{
    int z = 0;
};
struct Aaa
{
    int a = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<game::Stats>("game::Stats").property("hp", &game::Stats::hp).property("mana", &game::Stats::mana);
    rttr::registration::class_<ai::Stats>("ai::Stats").property("aggression", &ai::Stats::aggression);
    rttr::registration::class_<Zzz>("Zzz").property("z", &Zzz::z);
    rttr::registration::class_<Aaa>("Aaa").property("a", &Aaa::a);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerType<game::Stats>();
    rpe::TypeBridge::registerType<ai::Stats>();
    rpe::TypeBridge::registerTypes<Zzz, Aaa>();

    flecs::world world;
    world.entity("Hero").set<game::Stats>({ 77, 30 });
    world.entity("Bot").set<ai::Stats>({ 9.5 });
    // Multi-component: the same-leaf "Stats" sits at a DIFFERENT row on each entity,
    // so the neighbour fallback would land on the wrong one — only a leaf match picks
    // the right namespaced component.
    world.entity("HeroM").set<game::Stats>({ 1, 2 }).set<Zzz>({ 9 }); // sorted: Stats, Zzz
    world.entity("BotM").set<Aaa>({ 8 }).set<ai::Stats>({ 3.5 });     // sorted: Aaa, Stats

    rpe::EcsMirror mirror;
    mirror.attach(&world);
    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    browser.show();

    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(8);
        }
    };
    auto* el = browser.entityList()->findChild<QListWidget*>();
    auto selectEntity = [&](const QString& name) {
        for (int i = 0; i < el->count(); ++i)
            if (el->item(i)->text() == name)
                el->setCurrentRow(i);
    };
    auto leaves = [&] { return browser.propertyEditor()->visibleLeafPaths(false); };

    pump(25);

    // Hero → auto-selects its Stats (game::Stats).
    selectEntity(QStringLiteral("Hero"));
    pump(25);
    check("Hero binds game::Stats (hp/mana)",
          leaves().contains(QStringLiteral("hp")) && !leaves().contains(QStringLiteral("aggression")));

    // Switch to Bot WITHOUT touching the component list. It must bind ai::Stats.
    selectEntity(QStringLiteral("Bot"));
    pump(25);
    check("Bot binds ITS OWN ai::Stats (aggression), not the carried-over game::Stats",
          leaves().contains(QStringLiteral("aggression")) && !leaves().contains(QStringLiteral("hp")));

    // Switch back to Hero, again without touching the component list.
    selectEntity(QStringLiteral("Hero"));
    pump(25);
    check("back to Hero binds game::Stats again",
          leaves().contains(QStringLiteral("hp")) && !leaves().contains(QStringLiteral("aggression")));

    // ── Multi-component: select "Stats" on HeroM, then switch to BotM. The same
    //    LEAF ("Stats") must carry over to BotM's ai::Stats — not BotM's first row
    //    (Aaa), and not HeroM's game::Stats. ─────────────────────────────────────
    auto* cl = browser.componentList()->findChild<QListWidget*>();
    auto selectComp = [&](const QString& leaf) {
        for (int i = 0; i < cl->count(); ++i)
            if (cl->item(i)->text() == leaf)
                cl->setCurrentRow(i);
    };
    selectEntity(QStringLiteral("HeroM"));
    pump(25);
    selectComp(QStringLiteral("Stats")); // HeroM's game::Stats
    pump(25);
    check("HeroM/Stats binds game::Stats (hp)", leaves().contains(QStringLiteral("hp")));

    selectEntity(QStringLiteral("BotM"));
    pump(25);
    check("switching entity carries the LEAF: BotM/Stats binds ai::Stats (aggression)",
          leaves().contains(QStringLiteral("aggression")));
    check("...not BotM's first component (Aaa)", !leaves().contains(QStringLiteral("a")));
    check("...and not the carried-over game::Stats (hp)", !leaves().contains(QStringLiteral("hp")));

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

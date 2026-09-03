// End-to-end MIRROR MODE: the selection policy must hold when driven by a real
// EcsMirror producer + EntityComponentBrowser (not just the widget's setEntries feed).
//   • a world spawn never steals the current selection;
//   • removing another entity leaves the selection put;
//   • deleting the SELECTED entity moves to its neighbour, not the first row;
//   • the component panel follows the surviving/neighbour selection.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QCoreApplication>
#include <QListWidget>
#include <QThread>

#include <cstdio>

struct Mark
{
    int v = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Mark>("Mark").property("v", &Mark::v);
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
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    rpe::TypeBridge::registerTypes<Mark>();

    flecs::world world;
    auto alpha = world.entity("Alpha").set<Mark>({ 1 });
    world.entity("Beta").set<Mark>({ 2 });
    auto gamma = world.entity("Gamma").set<Mark>({ 3 });
    world.entity("Delta").set<Mark>({ 4 });
    const auto gid = static_cast<qulonglong>(gamma.id());

    rpe::EcsMirror mirror;
    mirror.attach(&world);
    mirror.setScanIntervalsMs(0, 0); // scan every pump → world edits reflect immediately

    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    browser.resize(420, 640);
    browser.show();

    auto pump = [&](int iters) {
        for (int i = 0; i < iters; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(8);
        }
    };
    auto* elist = browser.entityList()->findChild<QListWidget*>();
    auto* clist = browser.componentList()->findChild<QListWidget*>();
    auto curId = [&]() -> qulonglong {
        return elist->currentItem() ? elist->currentItem()->data(Qt::UserRole).toULongLong() : 0;
    };

    // Baseline: the four entities list; the top is auto-selected and its panel shows.
    pump(20);
    check("mirror: entities listed", elist->count() == 4);
    check("mirror: a component is shown for the auto-selected entity", clist->count() > 0);

    // Select Gamma (a MIDDLE entity) so a later delete can be told from a snap-to-top.
    check("mirror: selectEntity(Gamma)", browser.selectEntity(gid) && curId() == gid);

    // ── A world SPAWN must not steal the selection ─────────────────────────────
    auto zeta = world.entity("Zeta").set<Mark>({ 9 }); // sorts last
    world.entity("Aaa").set<Mark>({ 8 });              // sorts FIRST, and is newest by id
    const auto zid = static_cast<qulonglong>(zeta.id());
    pump(20);
    check("mirror: spawns appeared in the list", elist->count() == 6);
    check("mirror: a world spawn does not steal the selection", curId() == gid);

    // ── Removing ANOTHER entity leaves the selection put ───────────────────────
    alpha.destruct();
    pump(20);
    check("mirror: removing another entity keeps the selection", curId() == gid);

    // ── Deleting the SELECTED entity → its NEIGHBOUR, not the first row ────────
    // Remaining sorted: Aaa, Beta, Delta, Gamma, Zeta → Gamma sits at index 3, so the
    // row that shifts into its slot (its neighbour) is Zeta — NOT the first row (Aaa).
    gamma.destruct();
    pump(20);
    check("mirror: Gamma is gone", curId() != gid && elist->count() == 4);
    check("mirror: deleting the selected entity selects the neighbour, not the first",
          curId() == zid);
    check("mirror: the neighbour's component panel is populated", clist->count() > 0);

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

// When the SELECTED entity is destroyed (e.g. its plugin unloads), the list falls
// back to a neighbour — and the COMPONENT PANEL must refresh to that neighbour's
// components, not stay stuck on the gone entity's (or blank). Distinct component
// sets per entity make "which entity's components are shown" observable.
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

struct Aaa
{
    int a = 0;
};
struct Bbb
{
    int b = 0;
};
struct Ccc
{
    int c = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Aaa>("Aaa").property("a", &Aaa::a);
    rttr::registration::class_<Bbb>("Bbb").property("b", &Bbb::b);
    rttr::registration::class_<Ccc>("Ccc").property("c", &Ccc::c);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static QStringList compTexts(QListWidget* lw)
{
    QStringList out;
    for (int i = 0; i < lw->count(); ++i)
        out << lw->item(i)->text();
    return out;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    rpe::TypeBridge::registerTypes<Aaa, Bbb, Ccc>();

    flecs::world world;
    // "Plug" has Aaa+Bbb (stands in for a plugin entity); "Core" has Ccc only.
    auto plug = world.entity("Plug").set<Aaa>({ 1 }).set<Bbb>({ 2 });
    world.entity("Core").set<Ccc>({ 3 });
    const auto plugId = static_cast<qulonglong>(plug.id());

    rpe::EcsMirror mirror;
    mirror.attach(&world);
    // Default (~0.5s) scan interval on purpose — the real app's timing. The prompt
    // rescan for a destroyed/unqualified selected entity must carry the refresh.

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

    // Select Plug (Core sorts first, so it isn't the default) → its components show.
    pump(20);
    browser.selectEntity(plugId);
    pump(15);
    check("Plug is selected, its components show",
          elist->currentItem() && elist->currentItem()->text() == QStringLiteral("Plug")
              && compTexts(clist).contains(QStringLiteral("Aaa")));

    // ── Plugin unload: destroy the selected entity. Core is the only survivor. ──
    plug.destruct();
    pump(30);

    check("the gone entity dropped, Core is now selected",
          elist->count() == 1 && elist->currentItem()
              && elist->currentItem()->text() == QStringLiteral("Core"));
    // The heart of the report: the panel must show Core's component (Ccc), not the
    // destroyed Plug's stale ones (Aaa/Bbb), and must not be blank.
    check("component panel refreshed to the neighbour's components (Ccc)",
          compTexts(clist).contains(QStringLiteral("Ccc")));
    check("stale components of the destroyed entity are gone",
          !compTexts(clist).contains(QStringLiteral("Aaa"))
              && !compTexts(clist).contains(QStringLiteral("Bbb")));

    // And the property tree binds to the surviving component (shows its leaf).
    check("property panel bound to the surviving component",
          browser.propertyEditor()->visibleLeafPaths(false).contains(QStringLiteral("c")));

    // ── Delete the LAST entity → panel clears; then a NEW entity must select and
    //    show its components (the "then I can't see the new entity" part). ────────
    world.entity(static_cast<flecs::entity_t>(
                     browser.entityList()->findChild<QListWidget*>()->currentItem()->data(Qt::UserRole).toULongLong()))
        .destruct(); // destroy Core (the only remaining, selected entity)
    pump(20);
    check("deleting the last entity empties the list", elist->count() == 0);
    check("empty list → component panel cleared", clist->count() == 0);

    world.entity("Fresh").set<Aaa>({ 7 });
    pump(25); // ~200ms: the eager empty-state scan (≤100ms) must catch it
    check("a new entity appears and is auto-selected",
          elist->count() == 1 && elist->currentItem() && elist->currentItem()->text() == QStringLiteral("Fresh"));
    check("the new entity's components are shown", compTexts(clist).contains(QStringLiteral("Aaa")));

    // ── Faithful plugin unload: destroy the entity AND unregister its bridged
    //    types (registry generation bumps) — the panel must still clear/refresh. ──
    {
        auto keeper = world.entity("Keeper").set<Ccc>({ 5 });
        pump(15);
        browser.selectEntity(static_cast<qulonglong>(
            world.lookup("Fresh").id())); // select the Aaa-typed entity
        pump(15);
        check("Fresh selected before unload", compTexts(clist).contains(QStringLiteral("Aaa")));

        world.lookup("Fresh").destruct();     // the "plugin" entity leaves
        rpe::TypeBridge::unregisterType<Aaa>(); // and its bridged type is torn down
        pump(25);
        check("after unload, the survivor (Keeper) is selected",
              elist->count() == 1 && elist->currentItem()->text() == QStringLiteral("Keeper"));
        check("after unload, the survivor's components show (Ccc)",
              compTexts(clist).contains(QStringLiteral("Ccc")));
        rpe::TypeBridge::registerType<Aaa>(); // restore for any later use
        (void)keeper;
    }

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

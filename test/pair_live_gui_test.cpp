// End-to-end (offscreen) mirror test for a DATA-CARRYING PAIR selected in the
// browser: selecting the pair row binds its carried type's rows, the initial
// value populates, AND live sim-side changes to the pair keep updating the
// value column (the reported "pair components don't update").
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
#include <QTreeView>

#include <cstdio>
#include <functional>

struct Health
{
    int hp = 100;
};
struct Damage
{
    int amount = 10;
    float crit = 1.0f; // second field: exercises multi-leaf pair updates
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
    rttr::registration::class_<Damage>("Damage")
        .property("amount", &Damage::amount)
        .property("crit", &Damage::crit);
}

static int g_fails = 0;
static void check(const char* name, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_fails;
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerTypes<Health, Damage>();

    flecs::world world;
    auto fire = world.entity("Fire");
    auto ice = world.entity("Ice");
    auto e = world.entity("E").set<Health>({ 70 });
    e.set<Damage>(fire, { 10, 1.0f }); // data-carrying pair
    e.set<Damage>(ice, { 3, 2.0f });   // a SECOND pair, same relation, other target

    rpe::EcsMirror mirror;
    mirror.attach(&world);

    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    browser.resize(400, 600);
    browser.show();

    auto pump = [&](int iters) {
        for (int i = 0; i < iters; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(8);
        }
    };

    pump(25);
    QListWidget* entityList = browser.entityList()->findChild<QListWidget*>();
    check("entity list populated", entityList && entityList->count() > 0);
    if (entityList && entityList->count() > 0)
        entityList->setCurrentRow(0);

    pump(15);
    QListWidget* compList = browser.componentList()->findChild<QListWidget*>();
    check("component list populated", compList && compList->count() > 0);

    // Select the PAIR row ("Damage (Fire)") — find it by its UserRole key.
    int pairRow = -1;
    for (int i = 0; compList && i < compList->count(); ++i)
        if (compList->item(i)->data(Qt::UserRole).toString() == QStringLiteral("Damage (Fire)"))
            pairRow = i;
    check("pair row present in the component list", pairRow >= 0);
    if (pairRow >= 0)
        compList->setCurrentRow(pairRow);

    pump(15);
    const QStringList leaves = browser.propertyEditor()->visibleLeafPaths(false);
    check("pair's carried-type rows bound (amount visible)", leaves.contains(QStringLiteral("amount")));

    QTreeView* tree = browser.propertyEditor()->findChild<QTreeView*>();
    auto valueOf = [&](const QString& leaf) -> QString {
        QString found;
        if (!tree || !tree->model())
            return found;
        auto* m = tree->model();
        std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
            for (int r = 0; r < m->rowCount(parent); ++r)
            {
                const QModelIndex name = m->index(r, 0, parent);
                if (name.data(Qt::DisplayRole).toString() == leaf)
                    found = m->index(r, 1, parent).data(Qt::DisplayRole).toString();
                walk(name);
            }
        };
        walk(QModelIndex());
        return found;
    };

    pump(25);
    check("initial pair value shows (amount == 10)", valueOf(QStringLiteral("amount")) == QStringLiteral("10"));

    // ── LIVE UPDATE: the sim changes the pair's value → the panel must follow ──
    e.set<Damage>(fire, { 77 });
    pump(25);
    check("live sim change reflects in the panel (amount == 77)", valueOf(QStringLiteral("amount")) == QStringLiteral("77"));

    e.set<Damage>(fire, { 123, 1.0f });
    pump(25);
    check("a second live change also reflects (amount == 123)", valueOf(QStringLiteral("amount")) == QStringLiteral("123"));

    // ── The pair's OTHER leaf (crit) must update live too ──────────────────────
    check("second leaf initial (crit == 1)", valueOf(QStringLiteral("crit")) == QStringLiteral("1"));
    e.set<Damage>(fire, { 123, 4.5f });
    pump(25);
    check("second leaf updates live (crit == 4.5)", valueOf(QStringLiteral("crit")) == QStringLiteral("4.5"));

    // ── Switch to the OTHER pair of the same relation (Damage, Ice) ────────────
    int icePairRow = -1;
    for (int i = 0; compList && i < compList->count(); ++i)
        if (compList->item(i)->data(Qt::UserRole).toString() == QStringLiteral("Damage (Ice)"))
            icePairRow = i;
    check("second pair (Damage, Ice) present", icePairRow >= 0);
    if (icePairRow >= 0)
    {
        compList->setCurrentRow(icePairRow);
        pump(25);
        check("switching to the Ice pair shows ITS value (amount == 3)",
              valueOf(QStringLiteral("amount")) == QStringLiteral("3"));
        e.set<Damage>(ice, { 88, 2.0f });
        pump(25);
        check("the Ice pair updates live (amount == 88)", valueOf(QStringLiteral("amount")) == QStringLiteral("88"));
        // And the Fire pair must be untouched by the Ice edits.
        check("the Fire pair kept its own value", e.get<Damage>(fire).amount == 123);
    }

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

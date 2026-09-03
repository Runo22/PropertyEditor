// Selection stability + stale-component clearing.
//
//  A) EntityListWidget: adding/removing entities must NOT move the selection while
//     the selected entity is still present (and must do so silently, without
//     re-emitting a selection signal a host might act on); only when the selection
//     genuinely disappears does it fall back to the first entity, and an empty list
//     deselects.
//  B) EntityComponentBrowser (mirror mode): when a required-component filter empties
//     the entity list, the component panel must clear — not keep showing the
//     just-excluded entity's components.
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

struct Position
{
    float x = 0;
};
struct Velocity
{
    float v = 0;
};
struct Ghost // registered, but placed on no entity — used to empty a filtered list
{
    int g = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Position>("Position").property("x", &Position::x);
    rttr::registration::class_<Velocity>("Velocity").property("v", &Velocity::v);
    rttr::registration::class_<Ghost>("Ghost").property("g", &Ghost::g);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static qulonglong curId(rpe::EntityListWidget& w)
{
    auto* lw = w.findChild<QListWidget*>();
    return (lw && lw->currentItem()) ? lw->currentItem()->data(Qt::UserRole).toULongLong() : 0;
}

static void partA()
{
    rpe::EntityListWidget list; // no world → mirror-feed mode (setEntries)
    int idSel = 0;
    int deselect = 0;
    qulonglong lastId = 0;
    QObject::connect(&list, &rpe::EntityListWidget::entityIdSelected, &list, [&](qulonglong id) { ++idSel; lastId = id; });
    QObject::connect(&list, &rpe::EntityListWidget::entityDeselected, &list, [&] { ++deselect; });

    auto feed = [&](std::initializer_list<QPair<qulonglong, QString>> rows) {
        list.setEntries(QVector<QPair<qulonglong, QString>>(rows));
    };

    // First populate → the top (alphabetically Alpha) is auto-selected and notified.
    feed({ { 10, QStringLiteral("Alpha") }, { 20, QStringLiteral("Beta") }, { 30, QStringLiteral("Gamma") } });
    check("A: first populate auto-selects the top entity", curId(list) == 10 && idSel == 1 && lastId == 10);

    // Move to a MIDDLE entity so removals can be told apart from a snap-to-first.
    check("A: selectById(Gamma) selects it", list.selectById(30) && curId(list) == 30);
    const int selBefore = idSel;

    // Add an entity (e.g. a world spawn) → the current selection is UNTOUCHED, and no
    // selection signal fires. A newcomer never steals selection, even a top-sorted or
    // high-id one — only an explicit selectById() (below) changes it on appearance.
    feed({ { 10, QStringLiteral("Alpha") }, { 20, QStringLiteral("Beta") }, { 30, QStringLiteral("Gamma") }, { 40, QStringLiteral("Delta") } });
    check("A: adding an entity keeps the current selection", curId(list) == 30);
    feed({ { 99, QStringLiteral("Aaa") }, { 10, QStringLiteral("Alpha") }, { 20, QStringLiteral("Beta") }, { 30, QStringLiteral("Gamma") }, { 40, QStringLiteral("Delta") } });
    check("A: a newly spawned entity never steals the selection", curId(list) == 30);
    check("A: ...silently (no redundant selection signal on add)", idSel == selBefore);

    // Explicit selectById() for an entity not present yet is honoured when it appears.
    check("A: selectById(absent) is pending", !list.selectById(77));
    feed({ { 77, QStringLiteral("Spawned") }, { 10, QStringLiteral("Alpha") }, { 20, QStringLiteral("Beta") }, { 30, QStringLiteral("Gamma") }, { 40, QStringLiteral("Delta") } });
    check("A: an explicitly requested entity IS selected when it appears", curId(list) == 77);
    check("A: selectById(Gamma) again", list.selectById(30) && curId(list) == 30);
    const int selBefore2 = idSel;

    // Remove some OTHER entity → the current selection is untouched, silently.
    feed({ { 10, QStringLiteral("Alpha") }, { 20, QStringLiteral("Beta") }, { 30, QStringLiteral("Gamma") }, { 40, QStringLiteral("Delta") } });
    check("A: removing another entity keeps the current selection", curId(list) == 30 && idSel == selBefore2);

    // Remove the SELECTED entity (Gamma, middle) → its NEIGHBOUR (Delta, which took
    // its slot) is selected — NOT the first entity (Alpha).
    feed({ { 10, QStringLiteral("Alpha") }, { 20, QStringLiteral("Beta") }, { 40, QStringLiteral("Delta") } });
    check("A: removing the selected entity picks the neighbour, not the first",
          curId(list) == 40 && lastId == 40);

    // Empty the list → deselect.
    const int deBefore = deselect;
    feed({});
    check("A: emptying the list deselects", curId(list) == 0 && deselect == deBefore + 1);
}

static void partB()
{
    flecs::world world;
    world.component<Ghost>(); // exists as a component, on no entity
    world.entity("Ball").set<Position>({ 1 }).set<Velocity>({ 2 });
    world.entity("Rock").set<Position>({ 3 });

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

    // Filter to entities that have Velocity → only Ball qualifies.
    auto s = browser.settings();
    s.requiredComponent = QStringLiteral("Velocity");
    s.requiredComponentEnabled = true;
    browser.setSettings(s);
    pump(25);

    auto* entityList = browser.entityList()->findChild<QListWidget*>();
    auto* compList = browser.componentList()->findChild<QListWidget*>();
    check("B: required=Velocity lists only the matching entity", entityList && entityList->count() == 1);
    if (entityList && entityList->count() == 1)
    {
        entityList->setCurrentRow(0);
    }
    pump(15);
    check("B: the matching entity's components are shown", compList && compList->count() > 0);

    // Switch the filter to a component NO entity carries → the list empties.
    s.requiredComponent = QStringLiteral("Ghost");
    browser.setSettings(s);
    pump(25);
    check("B: a filter matching no entity empties the entity list", entityList && entityList->count() == 0);
    // The bug: the panel kept showing the just-excluded entity's components.
    check("B: the component panel is cleared (no stale components)", compList && compList->count() == 0);

    mirror.detach();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerTypes<Position, Velocity, Ghost>();

    partA();
    partB();

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

// Verifies GUI-driven add/remove of components in mirror mode: the request is
// queued on the GUI thread and applied on the simulation thread by the mirror's
// per-frame system, and the change is reflected back to the component list and
// the add catalog.
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
    double x = 0, y = 0;
};
struct Velocity
{
    double dx = 0, dy = 0;
};
struct Health
{
    int hp = 100;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Position>("Position").property("x", &Position::x).property("y", &Position::y);
    rttr::registration::class_<Velocity>("Velocity").property("dx", &Velocity::dx).property("dy", &Velocity::dy);
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static QStringList rowsOf(QListWidget* l)
{
    QStringList s;
    for (int i = 0; i < l->count(); ++i)
        s << l->item(i)->text();
    return s;
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerType<Position>();
    rpe::TypeBridge::registerType<Velocity>();
    rpe::TypeBridge::registerType<Health>();

    flecs::world world;
    // Register the component types with flecs so they exist as components even
    // before any entity uses them (this is what makes them "addable"). In a real
    // app this happens wherever components are declared/registered.
    world.component<Position>();
    world.component<Velocity>();
    world.component<Health>();

    auto e = world.entity("Hero");
    e.set<Position>({ 1, 2 }); // starts with only Position

    rpe::EcsMirror mirror;
    mirror.attach(&world);

    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    browser.setComponentEditingEnabled(true);
    browser.show();

    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(8);
        }
    };

    pump(25);
    QListWidget* entityList = browser.entityList()->findChild<QListWidget*>();
    check("entity listed", entityList && entityList->count() == 1);
    entityList->setCurrentRow(0);
    pump(20);

    QListWidget* compList = browser.componentList()->findChild<QListWidget*>();
    check("entity starts with only Position", rowsOf(compList) == QStringList { QStringLiteral("Position") });

    // Add Velocity through the browser slot (as the "+" menu would).
    browser.componentList()->addComponentRequested(QStringLiteral("Velocity"));
    pump(25);
    check("after add, component list has Position + Velocity",
          rowsOf(compList).contains(QStringLiteral("Position")) && rowsOf(compList).contains(QStringLiteral("Velocity")));
    {
        bool hasVel = false;
        world.entity(static_cast<flecs::entity_t>(e.id())).each([&](flecs::id id) {
            if (id.is_entity() && id.entity().name() && QString::fromUtf8(id.entity().name()) == QStringLiteral("Velocity"))
                hasVel = true;
        });
        check("Velocity actually added on the entity in the world", hasVel);
    }

    // Remove Position through the per-row × path.
    browser.componentList()->removeComponentRequested(QStringLiteral("Position"));
    pump(25);
    check("after remove, Position is gone from the list",
          !rowsOf(compList).contains(QStringLiteral("Position")) && rowsOf(compList).contains(QStringLiteral("Velocity")));
    {
        bool hasPos = false;
        world.entity(static_cast<flecs::entity_t>(e.id())).each([&](flecs::id id) {
            if (id.is_entity() && id.entity().name() && QString::fromUtf8(id.entity().name()) == QStringLiteral("Position"))
                hasPos = true;
        });
        check("Position actually removed from the entity in the world", !hasPos);
    }

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

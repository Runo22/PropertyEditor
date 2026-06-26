// Headless (offscreen) integration test for mirror-mode property display.
// Mimics the user's setup: a component whose RTTR type is registered SCOPED
// ("Game::Physics") while flecs knows it by its short name ("Physics"). Drives
// the browser end-to-end — select entity, select component — and asserts that:
//   • selecting the component binds its property rows (issue: "can't see
//     component properties when clicked or selected"), and
//   • the mirrored values actually populate the value column.
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

struct Physics
{
    bool isStatic = true;
    double mass = 7.5;
};

RTTR_REGISTRATION
{
    // Deliberately SCOPED name — flecs will report the short name "Physics".
    rttr::registration::class_<Physics>("Game::Physics")
        .property("isStatic", &Physics::isStatic)
        .property("mass", &Physics::mass);
}

static int g_fails = 0;
static void check(const char* name, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++g_fails;
}

// Find the inner QListWidget of an EntityListWidget / ComponentListWidget.
template <class W>
static QListWidget* listOf(W* w)
{
    return w->template findChild<QListWidget*>();
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerType<Physics>();

    flecs::world world;
    auto ball = world.entity("Ball");
    ball.set<Physics>({ true, 7.5 });

    rpe::EcsMirror mirror;
    mirror.attach(&world);

    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    browser.resize(400, 600);
    browser.show();

    // Advance the sim + spin the Qt event loop so the mirror's polls land.
    auto pump = [&](int iters) {
        for (int i = 0; i < iters; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(8);
        }
    };

    // 1) Entity list should populate from the mirror, then select the entity.
    pump(25);
    QListWidget* entityList = listOf(browser.entityList());
    check("entity list populated", entityList && entityList->count() > 0);
    if (entityList && entityList->count() > 0)
    {
        entityList->setCurrentRow(0); // → entityIdSelected → interest pushed
    }

    // 2) Component list should populate (the scoped type resolved by short name).
    pump(15);
    QListWidget* compList = listOf(browser.componentList());
    const bool compShown = compList && compList->count() > 0;
    check("component list shows the scoped-name component", compShown);
    if (compShown)
    {
        check("component labelled by flecs short name 'Physics'",
              compList->item(0)->text() == QStringLiteral("Physics"));
        compList->setCurrentRow(0); // → componentNameSelected → bindType
    }

    // 3) Selecting the component must bind its property rows.
    pump(15);
    const QStringList leaves = browser.propertyEditor()->visibleLeafPaths(false);
    check("component properties are bound (rows visible)",
          leaves.contains(QStringLiteral("isStatic")) && leaves.contains(QStringLiteral("mass")));

    // 4) The mirrored values must populate the value column.
    pump(25);
    QTreeView* tree = browser.propertyEditor()->findChild<QTreeView*>();
    bool sawMass = false, sawStatic = false;
    if (tree && tree->model())
    {
        auto* m = tree->model();
        std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
            for (int r = 0; r < m->rowCount(parent); ++r)
            {
                const QModelIndex name = m->index(r, 0, parent);
                const QModelIndex val = m->index(r, 1, parent);
                const QString n = name.data(Qt::DisplayRole).toString();
                const QString v = val.data(Qt::DisplayRole).toString();
                if (n == QStringLiteral("mass") && v.contains(QStringLiteral("7.5")))
                    sawMass = true;
                if (n == QStringLiteral("isStatic") && (v == QStringLiteral("true") || v == QStringLiteral("True")))
                    sawStatic = true;
                walk(name);
            }
        };
        walk(QModelIndex());
    }
    check("mirrored value populated: mass == 7.5", sawMass);
    check("mirrored value populated: isStatic == true", sawStatic);

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

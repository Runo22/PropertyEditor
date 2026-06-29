// Proves the inspector resolves the CORRECT component type when two components
// share a leaf name ("Stats") but live in different namespaces — the user's
// "I can't see the property editor" case. The list shows the short leaf, but
// selection/resolution use the full flecs path, so the right schema + values bind.
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

RTTR_REGISTRATION
{
    // Registered with FULL namespaced names (recommended). Same leaf "Stats".
    rttr::registration::class_<game::Stats>("game::Stats").property("hp", &game::Stats::hp).property("mana", &game::Stats::mana);
    rttr::registration::class_<ai::Stats>("ai::Stats").property("aggression", &ai::Stats::aggression);
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

    flecs::world world;
    world.entity("Hero").set<game::Stats>({ 77, 30 });
    world.entity("Bot").set<ai::Stats>({ 9.5 });

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

    QListWidget* el = browser.entityList()->findChild<QListWidget*>();
    QListWidget* cl = browser.componentList()->findChild<QListWidget*>();
    QTreeView* tree = browser.propertyEditor()->findChild<QTreeView*>();

    auto leafRows = [&]() {
        QStringList r;
        for (int i = 0; i < cl->count(); ++i)
            r << cl->item(i)->text();
        return r;
    };
    auto value = [&](const QString& leaf) -> QString {
        QString found;
        auto* m = tree->model();
        std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& p) {
            for (int i = 0; i < m->rowCount(p); ++i)
            {
                const QModelIndex n = m->index(i, 0, p);
                if (n.data(Qt::DisplayRole).toString() == leaf)
                    found = m->index(i, 1, p).data(Qt::DisplayRole).toString();
                walk(n);
            }
        };
        walk(QModelIndex());
        return found;
    };
    auto selectEntity = [&](const QString& name) {
        for (int i = 0; i < el->count(); ++i)
            if (el->item(i)->text() == name)
                el->setCurrentRow(i);
    };

    pump(25);

    // Hero → its "Stats" must be game::Stats (hp/mana), NOT ai::Stats.
    selectEntity(QStringLiteral("Hero"));
    pump(25);
    check("Hero's component displays leaf \"Stats\"", leafRows() == QStringList { QStringLiteral("Stats") });
    cl->setCurrentRow(0);
    pump(25);
    QStringList heroLeaves = browser.propertyEditor()->visibleLeafPaths(false);
    check("Hero/Stats binds game::Stats schema (hp, mana)",
          heroLeaves.contains(QStringLiteral("hp")) && heroLeaves.contains(QStringLiteral("mana")) && !heroLeaves.contains(QStringLiteral("aggression")));
    check("Hero/Stats value hp == 77", value(QStringLiteral("hp")) == QStringLiteral("77"));

    // Bot → its "Stats" must be ai::Stats (aggression), NOT game::Stats.
    selectEntity(QStringLiteral("Bot"));
    pump(25);
    cl->setCurrentRow(0);
    pump(25);
    QStringList botLeaves = browser.propertyEditor()->visibleLeafPaths(false);
    check("Bot/Stats binds ai::Stats schema (aggression)",
          botLeaves.contains(QStringLiteral("aggression")) && !botLeaves.contains(QStringLiteral("hp")));
    check("Bot/Stats value aggression == 9.5", value(QStringLiteral("aggression")).contains(QStringLiteral("9.5")));

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

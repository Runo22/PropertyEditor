// End-to-end coverage for the right-click menus of both lists, driven through the
// real EntityComponentBrowser:
//   • built-in "Delete entity" / "Remove component" fire the structural change,
//   • static addEntityAction / addComponentAction entries appear and invoke their
//     callback with the right ids,
//   • setEntityMenuHook / setComponentMenuHook run at open time with the clicked
//     entity/component,
//   • and direct-mode entity deletion destructs under the guard.
// Menus are exec()-modal, so a 0-delay timer (fires inside exec's nested loop)
// finds the active menu, optionally triggers an entry, and dismisses it.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QCoreApplication>
#include <QListWidget>
#include <QMenu>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <functional>

struct Health
{
    int hp = 100;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

// Open a list's context menu at `row`, run `onMenu` inside exec (to inspect/trigger
// entries), then dismiss it. `listWidget` is the EntityListWidget/ComponentListWidget
// whose private `_onContextMenu(QPoint)` slot we invoke.
static void openMenu(QWidget* listWidget, QListWidget* lw, int row, std::function<void(QMenu*)> onMenu)
{
    const QRect r = lw->visualItemRect(lw->item(row));
    const QPoint pos = r.center();
    QTimer::singleShot(0, [onMenu] {
        auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (m && onMenu)
            onMenu(m);
        if (m)
            m->close();
    });
    QMetaObject::invokeMethod(listWidget, "_onContextMenu", Qt::DirectConnection, Q_ARG(QPoint, pos));
}

static void trigger(QMenu* m, const QString& label)
{
    for (QAction* a : m->actions())
        if (a->text() == label)
        {
            a->trigger();
            return;
        }
}
static bool hasEntry(QMenu* m, const QString& label)
{
    for (QAction* a : m->actions())
        if (a->text() == label)
            return true;
    return false;
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    rpe::TypeBridge::registerTypes<Health>();

    // ── Mirror mode: entity + component menus, built-ins + custom ──────────────
    {
        flecs::world world;
        world.entity("Alpha").set<Health>({ 70 });
        auto beta = world.entity("Beta").set<Health>({ 40 });

        rpe::EcsMirror mirror;
        mirror.attach(&world);
        rpe::EntityComponentBrowser browser;
        browser.setMirror(&mirror);
        browser.setEntityRemovingEnabled(true);
        browser.setComponentEditingEnabled(true);
        browser.resize(360, 520);
        browser.show();

        qulonglong entityActed = 0, entityHookId = 0;
        browser.addEntityAction({ QStringLiteral("Focus"), QIcon(), [&](qulonglong id) { entityActed = id; } });
        browser.setEntityMenuHook([&](qulonglong id, QMenu&) { entityHookId = id; });

        qulonglong compEntId = 0, compHookEntId = 0;
        QString compKey, compHookKey;
        browser.addComponentAction({ QStringLiteral("Copy"), QIcon(),
                                     [&](qulonglong e, const QString& c, qulonglong) { compEntId = e; compKey = c; } });
        browser.setComponentMenuHook([&](qulonglong e, const QString& c, qulonglong, QMenu&) {
            compHookEntId = e;
            compHookKey = c;
        });

        auto pump = [&](int n) {
            for (int i = 0; i < n; ++i) { world.progress(0.016f); QCoreApplication::processEvents(); QThread::msleep(6); }
        };
        pump(30);

        auto* el = browser.entityList()->findChild<QListWidget*>();
        check("entity list populated", el && el->count() >= 2);
        // Select "Beta" so the component panel binds it; find its row.
        int betaRow = -1;
        for (int i = 0; i < el->count(); ++i)
            if (el->item(i)->text() == QStringLiteral("Beta"))
                betaRow = i;
        el->setCurrentRow(betaRow);
        pump(15);
        const qulonglong betaId = static_cast<qulonglong>(beta.id());

        // ── Entity menu: static action + hook + built-in Delete present ────────
        bool sawDelete = false, sawFocus = false;
        openMenu(browser.entityList(), el, betaRow, [&](QMenu* m) {
            sawDelete = hasEntry(m, QStringLiteral("Delete entity"));
            sawFocus = hasEntry(m, QStringLiteral("Focus"));
            trigger(m, QStringLiteral("Focus")); // fire the static action
        });
        check("entity menu has built-in 'Delete entity'", sawDelete);
        check("entity menu has the custom 'Focus' action", sawFocus);
        check("entity static action fired with the clicked id", entityActed == betaId);
        check("entity menu hook ran with the clicked id", entityHookId == betaId);

        // ── Component menu (while Beta is alive + selected) ────────────────────
        auto* cl = browser.componentList()->findChild<QListWidget*>();
        check("component list populated for the selected entity", cl && cl->count() >= 1);
        bool sawRemove = false, sawCopy = false;
        openMenu(browser.componentList(), cl, 0, [&](QMenu* m) {
            sawRemove = hasEntry(m, QStringLiteral("Remove component"));
            sawCopy = hasEntry(m, QStringLiteral("Copy"));
            trigger(m, QStringLiteral("Copy"));
        });
        check("component menu has built-in 'Remove component'", sawRemove);
        check("component menu has the custom 'Copy' action", sawCopy);
        check("component static action fired with entity id + key",
              compEntId == betaId && compKey == QStringLiteral("Health"));
        check("component menu hook ran with entity id + key",
              compHookEntId == betaId && compHookKey == QStringLiteral("Health"));

        // ── Built-in "Remove component" removes it (Beta still selected) ──────
        openMenu(browser.componentList(), cl, 0, [&](QMenu* m) { trigger(m, QStringLiteral("Remove component")); });
        pump(20);
        check("'Remove component' removed Health from the entity", beta.is_alive() && !beta.has<Health>());

        // ── Built-in "Delete entity" destroys the entity (on Alpha) ───────────
        int alphaRow = -1;
        for (int i = 0; i < el->count(); ++i)
            if (el->item(i)->text() == QStringLiteral("Alpha"))
                alphaRow = i;
        auto alpha = world.lookup("Alpha");
        check("Alpha still listed", alphaRow >= 0 && alpha.is_alive());
        if (alphaRow >= 0)
        {
            openMenu(browser.entityList(), el, alphaRow, [&](QMenu* m) { trigger(m, QStringLiteral("Delete entity")); });
            pump(20);
            check("'Delete entity' destroyed the entity", !alpha.is_alive());
        }

        mirror.detach();
    }

    // ── Direct mode: entity deletion destructs under the guard ─────────────────
    {
        flecs::world world;
        auto a = world.entity("Keep").set<Health>({ 5 });
        auto b = world.entity("Kill").set<Health>({ 6 });

        rpe::EntityComponentBrowser browser;
        browser.setWorld(&world);
        browser.setEntityRemovingEnabled(true);
        browser.resize(360, 400);
        browser.show();
        QCoreApplication::processEvents();

        auto* el = browser.entityList()->findChild<QListWidget*>();
        int killRow = -1;
        for (int i = 0; el && i < el->count(); ++i)
            if (el->item(i)->text() == QStringLiteral("Kill"))
                killRow = i;
        check("direct-mode entity list has 'Kill'", killRow >= 0);
        if (killRow >= 0)
        {
            openMenu(browser.entityList(), el, killRow, [](QMenu* m) { trigger(m, QStringLiteral("Delete entity")); });
            QCoreApplication::processEvents();
            check("direct-mode delete destructs the entity", !b.is_alive());
            check("direct-mode delete leaves the other entity", a.is_alive());
        }
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

// End-to-end coverage for the right-click menus of both lists, driven through the
// real EntityComponentBrowser:
//   • built-in "Delete entity" / "Remove component" fire the structural change,
//   • static addEntityAction / addComponentAction entries appear and invoke their
//     callback with the right ids,
//   • setEntityMenuHook / setComponentMenuHook run at open time with the clicked
//     entity/component,
//   • and direct-mode entity deletion destructs under the guard.
//   • submenus: an action's `menu` path, rpe::subMenu() merging levels across
//     static actions and several hooks, labels with '/', '&' mnemonics;
//   • several add*MenuHook hooks run in order; remove* handles take entries away.
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
static QMenu* subOf(QMenu* m, const QString& title)
{
    for (QAction* a : m->actions())
        if (a->menu() && a->text() == title)
            return a->menu();
    return nullptr;
}
static int countSubs(QMenu* m, const QString& title)
{
    int n = 0;
    for (QAction* a : m->actions())
        if (a->menu() && a->text() == title)
            ++n;
    return n;
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

    // ── rpe::subMenu on its own ────────────────────────────────────────────────
    {
        QMenu root;
        QMenu* a = rpe::subMenu(root, { QStringLiteral("Debug"), QStringLiteral("Physics") });
        QMenu* b = rpe::subMenu(root, { QStringLiteral("Debug"), QStringLiteral("Physics") });
        check("subMenu reuses an existing path", a == b && countSubs(&root, QStringLiteral("Debug")) == 1);
        check("subMenu with an empty path is the root", rpe::subMenu(root, {}) == &root);
        check("blank levels are skipped",
              rpe::subMenu(root, { QStringLiteral(" "), QStringLiteral("Debug"), QString() }) == subOf(&root, QStringLiteral("Debug")));
        QMenu* amp = root.addMenu(QStringLiteral("&Tools"));
        check("a level matches a title with an '&' mnemonic", rpe::subMenu(root, { QStringLiteral("Tools") }) == amp);
        check("a '/' inside a level is just text (no splitting)",
              rpe::subMenu(root, { QStringLiteral("I/O") })->title() == QStringLiteral("I/O"));
    }

    // ── Submenus, several hooks, removal (direct mode) ─────────────────────────
    {
        flecs::world world;
        auto e = world.entity("Solo").set<Health>({ 1 });
        rpe::EntityComponentBrowser browser;
        browser.setWorld(&world);
        browser.setComponentEditingEnabled(true);
        browser.resize(360, 400);
        browser.show();
        QCoreApplication::processEvents();
        auto* el = browser.entityList()->findChild<QListWidget*>();
        browser.selectEntity(e);
        for (int i = 0; i < 20; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        const qulonglong soloId = static_cast<qulonglong>(e.id());

        qulonglong wokeId = 0;
        QStringList order;
        const int wake = browser.addEntityAction({ QStringLiteral("Wake"), QIcon(), [&](qulonglong id) { wokeId = id; },
                                                   { QStringLiteral("Debug"), QStringLiteral("Physics") } });
        browser.addEntityAction({ QStringLiteral("Save/Load"), QIcon(), [](qulonglong) {} });
        browser.setEntityMenuHook([&](qulonglong, QMenu& m) {
            order << QStringLiteral("set");
            rpe::subMenu(m, { QStringLiteral("Debug") })->addAction(QStringLiteral("FromSetHook"));
        });
        const int h1 = browser.addEntityMenuHook([&](qulonglong, QMenu& m) {
            order << QStringLiteral("h1");
            rpe::subMenu(m, { QStringLiteral("Debug"), QStringLiteral("Physics") })->addAction(QStringLiteral("FromPluginA"));
        });
        browser.addEntityMenuHook([&](qulonglong, QMenu& m) {
            order << QStringLiteral("h2");
            rpe::subMenu(m, { QStringLiteral("Debug") })->addAction(QStringLiteral("FromPluginB"));
        });

        bool oneDebug = false, physicsMerged = false, debugMerged = false, slashTop = false;
        openMenu(browser.entityList(), el, 0, [&](QMenu* m) {
            oneDebug = countSubs(m, QStringLiteral("Debug")) == 1;
            QMenu* dbg = subOf(m, QStringLiteral("Debug"));
            QMenu* phys = dbg ? subOf(dbg, QStringLiteral("Physics")) : nullptr;
            physicsMerged = phys && countSubs(dbg, QStringLiteral("Physics")) == 1 && hasEntry(phys, QStringLiteral("Wake"))
                && hasEntry(phys, QStringLiteral("FromPluginA"));
            debugMerged = dbg && hasEntry(dbg, QStringLiteral("FromSetHook")) && hasEntry(dbg, QStringLiteral("FromPluginB"));
            slashTop = hasEntry(m, QStringLiteral("Save/Load"));
            if (phys)
                trigger(phys, QStringLiteral("Wake"));
        });
        check("static + hook entries share ONE 'Debug' submenu", oneDebug);
        check("Debug ▸ Physics holds the static action AND a hook's entry", physicsMerged);
        check("Debug holds entries from the set-hook and another plugin's hook", debugMerged);
        check("a label with '/' stays a top-level entry", slashTop);
        check("an action inside a submenu fires with the clicked id", wokeId == soloId);
        check("hooks run set-hook first, then added hooks in order",
              order == QStringList({ QStringLiteral("set"), QStringLiteral("h1"), QStringLiteral("h2") }));

        browser.removeEntityAction(wake);
        browser.removeMenuHook(h1);
        browser.setEntityMenuHook(nullptr);
        order.clear();
        bool physicsGone = false, bStill = false;
        openMenu(browser.entityList(), el, 0, [&](QMenu* m) {
            QMenu* dbg = subOf(m, QStringLiteral("Debug"));
            physicsGone = dbg && !subOf(dbg, QStringLiteral("Physics"));
            bStill = dbg && hasEntry(dbg, QStringLiteral("FromPluginB")) && !hasEntry(dbg, QStringLiteral("FromSetHook"));
        });
        check("removed action + hook leave no (empty) Physics submenu", physicsGone);
        check("the other plugin's hook still runs; the cleared set-hook doesn't", bStill && order == QStringList { QStringLiteral("h2") });

        // Component side: a submenu path + an added hook with the entity id.
        auto* cl = browser.componentList()->findChild<QListWidget*>();
        for (int i = 0; i < 50 && cl && cl->count() == 0; ++i)
        {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check("direct mode lists the selected entity's component", cl && cl->count() > 0);
        qulonglong compHookEnt = 0;
        QString copied;
        const int cAct = browser.addComponentAction({ QStringLiteral("Copy JSON"), QIcon(),
                                                      [&](qulonglong, const QString& c, qulonglong) { copied = c; },
                                                      { QStringLiteral("Export") } });
        const int cHook = browser.addComponentMenuHook([&](qulonglong ent, const QString&, qulonglong, QMenu& m) {
            compHookEnt = ent;
            rpe::subMenu(m, { QStringLiteral("Export") })->addAction(QStringLiteral("Copy YAML"));
        });
        bool exportBoth = false;
        if (cl && cl->count() > 0)
            openMenu(browser.componentList(), cl, 0, [&](QMenu* m) {
                QMenu* ex = subOf(m, QStringLiteral("Export"));
                exportBoth = ex && countSubs(m, QStringLiteral("Export")) == 1 && hasEntry(ex, QStringLiteral("Copy JSON"))
                    && hasEntry(ex, QStringLiteral("Copy YAML"));
                if (ex)
                    trigger(ex, QStringLiteral("Copy JSON"));
            });
        check("component action + hook share the 'Export' submenu", exportBoth);
        check("component submenu action fires with its key", copied == QStringLiteral("Health"));
        check("added component hook gets the selected entity id", compHookEnt == soloId);
        browser.removeComponentAction(cAct);
        browser.removeMenuHook(cHook);
        bool exportGone = false;
        if (cl && cl->count() > 0)
            openMenu(browser.componentList(), cl, 0, [&](QMenu* m) { exportGone = !subOf(m, QStringLiteral("Export")); });
        check("removing them takes the Export submenu away", exportGone);
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

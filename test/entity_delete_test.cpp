// Entity deletion: the entity-list trash glyph (two-step confirm) emits
// removeEntityRequested with the row's id, and the mirror's DestroyEntity
// structural actually destroys the entity on the sim thread.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityListWidget.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QStyleOptionViewItem>
#include <QTimer>

#include <cstdio>

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

// Send a real press+release at the trash glyph of `row` (the row's right-hand
// square). Goes through the view + the widget's event filter, exactly like a click.
static void sendGlyphClick(QListWidget* lw, int row)
{
    const QRect vr = lw->visualRect(lw->model()->index(row, 0));
    const QPointF p(vr.right() - vr.height() / 2.0, vr.center().y());
    const QPoint g = lw->viewport()->mapToGlobal(p.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, p, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(lw->viewport(), &press);
    QMouseEvent rel(QEvent::MouseButtonRelease, p, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(lw->viewport(), &rel);
}

static int rowOfId(QListWidget* lw, qulonglong id)
{
    for (int i = 0; i < lw->count(); ++i)
        if (lw->item(i)->data(Qt::UserRole).toULongLong() == id)
            return i;
    return -1;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // ── 1. Widget: the trash's two-step confirm emits removeEntityRequested with
    //        THAT row's id (arm, then delete). ──────────────────────────────────
    {
        rpe::EntityListWidget w;
        w.setEntityRemovingEnabled(true);
        w.setEntries({ { 10, QStringLiteral("Alpha") }, { 20, QStringLiteral("Beta") }, { 30, QStringLiteral("Gamma") } });
        w.resize(240, 200);
        w.show();
        QApplication::processEvents();
        auto* lw = w.findChild<QListWidget*>();

        qulonglong removed = 0;
        QObject::connect(&w, &rpe::EntityListWidget::removeEntityRequested, &w, [&](qulonglong id) { removed = id; });

        const int r = rowOfId(lw, 20);
        check("Beta row present", r >= 0);
        sendGlyphClick(lw, r); // first click → arm only
        check("first click only arms, does not delete", removed == 0ull);
        sendGlyphClick(lw, r); // second click → delete
        check("trash delete emits the row's exact entity id", removed == 20ull);

        // Disabling removal hides the affordance → clicks do nothing.
        w.setEntityRemovingEnabled(false);
        removed = 0;
        sendGlyphClick(lw, rowOfId(lw, 10));
        sendGlyphClick(lw, rowOfId(lw, 10));
        check("no delete when removing is disabled", removed == 0ull);
    }

    // ── 1b. Clicking the trash must NOT change the selection — you can delete an
    //        UNSELECTED entity while another stays selected. Uses REAL viewport mouse
    //        events (selection is decided on press, which the widget must swallow). ─
    {
        rpe::EntityListWidget w;
        w.setEntityRemovingEnabled(true);
        w.setEntries({ { 10, QStringLiteral("Alpha") }, { 20, QStringLiteral("Beta") }, { 30, QStringLiteral("Gamma") } });
        w.resize(240, 200);
        w.show();
        QApplication::processEvents();
        auto* lw = w.findChild<QListWidget*>();

        lw->setCurrentRow(rowOfId(lw, 10)); // select Alpha
        const qulonglong selBefore = lw->currentItem()->data(Qt::UserRole).toULongLong();

        qulonglong removed = 0;
        QObject::connect(&w, &rpe::EntityListWidget::removeEntityRequested, &w, [&](qulonglong id) { removed = id; });

        // Click the trash glyph of Beta (NOT the selected row) via real press+release.
        const int br = rowOfId(lw, 20);
        sendGlyphClick(lw, br); // arm
        check("clicking an entity's trash does not select it",
              lw->currentItem() && lw->currentItem()->data(Qt::UserRole).toULongLong() == selBefore);
        sendGlyphClick(lw, br); // confirm → delete
        check("the unselected entity's trash still deletes it", removed == 20ull);
        check("the selection is unchanged after the delete",
              lw->currentItem() && lw->currentItem()->data(Qt::UserRole).toULongLong() == selBefore);
    }

    // ── 2. The dynamic context-menu hook is invoked with the clicked entity id ─
    {
        rpe::EntityListWidget w;
        w.setEntries({ { 10, QStringLiteral("Alpha") }, { 20, QStringLiteral("Beta") } });
        w.resize(220, 200);
        w.show();
        QApplication::processEvents();
        auto* lw = w.findChild<QListWidget*>();

        qulonglong hookedId = 0;
        int menuItemsSeen = -1;
        w.setContextActions({ { QStringLiteral("Focus"), QIcon(), [](qulonglong) {} } });
        w.setContextMenuHook([&](qulonglong id, QMenu& menu) {
            hookedId = id;                              // host sees the clicked entity
            menu.addAction(QStringLiteral("Custom op")); // and can add its own buttons
            menuItemsSeen = menu.actions().size();       // static "Focus" + this one
        });

        // Menu is exec()-modal; a 0-delay timer (fires inside exec's nested loop)
        // dismisses it so the test doesn't block.
        QTimer::singleShot(0, [] {
            if (auto* pop = QApplication::activePopupWidget())
                pop->close();
        });
        const QModelIndex idx = lw->model()->index(0, 0); // Alpha, id 10
        const QPoint pos = lw->visualRect(idx).center();
        QMetaObject::invokeMethod(&w, "_onContextMenu", Qt::DirectConnection, Q_ARG(QPoint, pos));

        check("menu hook runs with the clicked entity id", hookedId == 10ull);
        check("host could add a button (menu had the custom entry)", menuItemsSeen >= 2);
    }

    // ── 3. Mirror: DestroyEntity actually destroys the entity ──────────────────
    {
        rpe::TypeBridge::registerTypes<Health>();
        flecs::world world;
        auto keep = world.entity("Keep").set<Health>({ 70 });
        auto doomed = world.entity("Doomed").set<Health>({ 10 });

        rpe::EcsMirror mirror;
        mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);
        mirror.setScanIntervalsMs(0, 0);
        auto ch = mirror.channel();
        world.progress(0.016f);
        mirror.pump();

        ch->queueDestroyEntity(static_cast<qulonglong>(doomed.id()));
        world.progress(0.016f);
        mirror.pump();

        check("targeted entity destroyed", !doomed.is_alive());
        check("other entity untouched", keep.is_alive() && keep.get<Health>().hp == 70);
        mirror.detach();
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

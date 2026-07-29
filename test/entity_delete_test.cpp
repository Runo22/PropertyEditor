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

// Drive the trash delegate's two-step confirm on `row` (arm then delete).
static void clickTrashTwice(QListWidget* lw, int row)
{
    auto* del = lw->itemDelegate();
    QStyleOptionViewItem opt;
    opt.rect = QRect(0, 0, 200, 20);        // glyphRect() = right 20px square
    const QPointF pos(190, 10);             // inside it
    const QModelIndex idx = lw->model()->index(row, 0);
    for (int i = 0; i < 2; ++i)
    {
        QMouseEvent ev(QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        del->editorEvent(&ev, lw->model(), opt, idx);
    }
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

    // ── 1. Widget: the trash emits removeEntityRequested with THAT row's id ─────
    {
        rpe::EntityListWidget w;
        w.setEntityRemovingEnabled(true);
        w.setEntries({ { 10, QStringLiteral("Alpha") }, { 20, QStringLiteral("Beta") }, { 30, QStringLiteral("Gamma") } });
        auto* lw = w.findChild<QListWidget*>();

        qulonglong removed = 0;
        QObject::connect(&w, &rpe::EntityListWidget::removeEntityRequested, &w, [&](qulonglong id) { removed = id; });

        const int r = rowOfId(lw, 20);
        check("Beta row present", r >= 0);
        clickTrashTwice(lw, r);
        check("trash delete emits the row's exact entity id", removed == 20ull);

        // A single click only ARMS (no delete yet).
        removed = 0;
        auto* del = lw->itemDelegate();
        QStyleOptionViewItem opt;
        opt.rect = QRect(0, 0, 200, 20);
        QMouseEvent ev(QEvent::MouseButtonRelease, QPointF(190, 10), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        del->editorEvent(&ev, lw->model(), opt, lw->model()->index(rowOfId(lw, 30), 0));
        check("first click only arms, does not delete", removed == 0ull);

        // Disabling removal hides the affordance → clicks do nothing.
        w.setEntityRemovingEnabled(false);
        removed = 0;
        clickTrashTwice(lw, rowOfId(lw, 10));
        check("no delete when removing is disabled", removed == 0ull);
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

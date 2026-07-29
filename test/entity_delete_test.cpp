// Entity deletion: the entity-list trash glyph (two-step confirm) emits
// removeEntityRequested with the row's id, and the mirror's DestroyEntity
// structural actually destroys the entity on the sim thread.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityListWidget.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QListWidget>
#include <QMouseEvent>
#include <QStyleOptionViewItem>

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

    // ── 2. Custom context actions are accepted (stored without crashing) ───────
    {
        rpe::EntityListWidget w;
        qulonglong acted = 0;
        w.setContextActions({ { QStringLiteral("Focus"), QIcon(), [&](qulonglong id) { acted = id; } } });
        check("setContextActions accepts a custom entry", acted == 0); // not invoked yet
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

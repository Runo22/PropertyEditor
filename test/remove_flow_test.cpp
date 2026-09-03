// Comprehensive removal-flow coverage for the by-id routing:
//   • the trash button on a data / tag / pair row emits removeComponentIdRequested
//     with THAT row's exact flecs id (never a neighbour's), driven through the real
//     delegate confirm flow (arm click → confirm click);
//   • a row with no id (legacy setComponentNames) falls back to the by-name signal;
//   • applying the id removal to a live world removes exactly that component and
//     leaves the others (data, tag, pair) intact — in direct mode.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>

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
struct Armor
{
    int def = 5;
};
struct Damage
{
    int amount = 10;
};
struct Burning
{
}; // zero-size tag

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
    rttr::registration::class_<Armor>("Armor").property("def", &Armor::def);
    rttr::registration::class_<Damage>("Damage").property("amount", &Damage::amount);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

// Drive the real RemoveButtonDelegate: arm (1st click) then confirm (2nd click) on
// the trash glyph of `row`. opt.rect/pos are synthetic but satisfy glyphRect(), so
// the test is deterministic and independent of on-screen layout.
static void clickTrashTwice(QListWidget* lw, int row)
{
    auto* del = lw->itemDelegate();
    QStyleOptionViewItem opt;
    opt.rect = QRect(0, 0, 200, 20);            // glyphRect() = right 20px square
    const QPointF pos(190, 10);                 // inside that square
    const QModelIndex idx = lw->model()->index(row, 0);
    for (int i = 0; i < 2; ++i)
    {
        QMouseEvent ev(QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        del->editorEvent(&ev, lw->model(), opt, idx);
    }
}

static int rowOf(QListWidget* lw, const QString& text)
{
    for (int i = 0; i < lw->count(); ++i)
        if (lw->item(i)->text().contains(text))
            return i;
    return -1;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerTypes<Health, Armor, Damage>();

    flecs::world world;
    world.component<Burning>();
    auto fire = world.entity("Fire");
    auto likes = world.entity("Likes");
    auto bob = world.entity("Bob");
    auto e = world.entity("E").set<Health>({ 70 }).set<Armor>({ 3 });
    e.add<Burning>();
    e.add(likes, bob);           // dataless pair
    e.set<Damage>(fire, { 10 }); // data pair

    rpe::ComponentListWidget w;
    w.setComponentEditingEnabled(true);
    w.setEntity(&world, e);
    auto* lw = w.findChild<QListWidget*>();

    // Record which signal fired, and APPLY the removal like the browser handler does.
    qulonglong lastId = 0;
    QString lastName;
    QObject::connect(&w, &rpe::ComponentListWidget::removeComponentIdRequested, &w, [&](qulonglong id) {
        lastId = id;
        lastName.clear();
        e.remove(static_cast<flecs::id_t>(id)); // _onRemoveComponentId (direct mode)
        w.setEntity(&world, e);
    });
    QObject::connect(&w, &rpe::ComponentListWidget::removeComponentRequested, &w, [&](const QString& n) {
        lastName = n;
        lastId = 0;
    });

    const auto healthId = static_cast<qulonglong>(world.component<Health>().raw_id());

    // ── 1. Data row → id signal carrying THAT component's id; only it is removed ──
    {
        lastId = 0;
        const int r = rowOf(lw, QStringLiteral("Health"));
        check("Health row present", r >= 0);
        clickTrashTwice(lw, r);
        check("data trash emits the id signal (not by-name)", lastId != 0 && lastName.isEmpty());
        check("the emitted id is Health's exact component id", lastId == healthId);
        check("Health removed from the world", !e.has<Health>());
        check("Armor survived", e.has<Armor>() && e.get<Armor>().def == 3);
        check("tag survived", e.has<Burning>());
        check("dataless pair survived", e.has(likes, bob));
        check("data pair survived", e.has<Damage>(fire));
    }

    // ── 2. Tag row → id signal; only the tag is removed ──────────────────────────
    {
        lastId = 0;
        const int r = rowOf(lw, QStringLiteral("Burning"));
        check("tag row present", r >= 0);
        clickTrashTwice(lw, r);
        check("tag trash emits the id signal", lastId != 0 && lastName.isEmpty());
        check("tag removed", !e.has<Burning>());
        check("Armor still there after tag removal", e.has<Armor>());
        check("data pair still there after tag removal", e.has<Damage>(fire));
    }

    // ── 3. Data-pair row → id signal; the exact pair is removed ───────────────────
    {
        lastId = 0;
        const int r = rowOf(lw, QStringLiteral("Damage"));
        check("data-pair row present", r >= 0);
        clickTrashTwice(lw, r);
        check("data-pair trash emits the id signal", lastId != 0 && lastName.isEmpty());
        check("data pair removed", !e.has<Damage>(fire));
        check("Armor is the last survivor", e.has<Armor>());
    }

    // ── 4. Legacy row (no id) → by-name fallback signal ──────────────────────────
    {
        rpe::ComponentListWidget w2;
        w2.setComponentEditingEnabled(true);
        w2.setComponentNames({ QStringLiteral("Foo"), QStringLiteral("Bar") }); // rawId == 0
        auto* lw2 = w2.findChild<QListWidget*>();
        qulonglong id2 = 999;
        QString name2;
        QObject::connect(&w2, &rpe::ComponentListWidget::removeComponentIdRequested, &w2, [&](qulonglong id) { id2 = id; });
        QObject::connect(&w2, &rpe::ComponentListWidget::removeComponentRequested, &w2, [&](const QString& n) {
            name2 = n;
            id2 = 0;
        });
        const int r = rowOf(lw2, QStringLiteral("Foo"));
        clickTrashTwice(lw2, r);
        check("idless row falls back to the by-name signal", name2 == QStringLiteral("Foo") && id2 == 0);
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

// Renders two screenshots for review:
//   • the "Add entity" prefab picker (grouped, with group icons), and
//   • the entity right-click menu (built-in Delete + host custom actions).
// Usage: rpe_context_menu_shot <output-dir>
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>
#include <rpe/gui/DarkStyle.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
#include <QIcon>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QThread>
#include <QToolButton>

#include <cstdio>

struct Health
{
    int hp = 100;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
}

// A tiny round dot icon in a given colour, so the shots have distinct glyphs.
static QIcon dot(const QColor& c)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(1, 1, 12, 12);
    return QIcon(pm);
}

static void save(QWidget* w, const QString& path)
{
    const QPixmap pm = w->grab();
    printf("%s %s (%dx%d)\n", pm.save(path, "PNG") ? "saved" : "FAILED", path.toUtf8().constData(), pm.width(), pm.height());
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setStyleSheet(rpe::darkStyleSheet());
    const QString dir = argc > 1 ? QString::fromUtf8(argv[1]) : QStringLiteral(".");

    rpe::TypeBridge::registerTypes<Health>();

    flecs::world world;
    auto enemy = world.entity("Enemy");
    auto prop = world.entity("Prop");
    world.prefab("Goblin Prefab").set<Health>({ 100 }).add(enemy);
    world.prefab("Orc Prefab").set<Health>({ 200 }).add(enemy);
    world.prefab("Skeleton Prefab").set<Health>({ 60 }).add(enemy);
    world.prefab("Chest Prefab").set<Health>({ 30 }).add(prop);
    world.prefab("Barrel Prefab").set<Health>({ 20 }).add(prop);
    // A few live entities so the list isn't empty.
    world.entity("Player").set<Health>({ 100 });
    world.entity("Goblin_501").set<Health>({ 100 });

    rpe::EcsMirror mirror;
    mirror.attach(&world);

    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    browser.setEntityAddingEnabled(true);
    browser.setEntityRemovingEnabled(true);
    browser.setPrefabGroups({ { QStringLiteral("Enemy"), dot(QColor(0xE0, 0x6C, 0x5A)) },
                              { QStringLiteral("Prop"), dot(QColor(0x6C, 0x9C, 0xE0)) } });
    browser.addEntityAction({ QStringLiteral("Focus camera"), dot(QColor(0x66, 0xBB, 0x6A)), [](qulonglong) {} });
    browser.addEntityAction({ QStringLiteral("Duplicate"), dot(QColor(0xF0, 0xC0, 0x50)), [](qulonglong) {} });
    browser.resize(360, 520);
    browser.show();

    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(6);
        }
    };
    pump(30);

    // ── 1. Add-entity prefab picker ────────────────────────────────────────────
    if (auto* btn = browser.entityList()->findChild<QToolButton*>())
    {
        btn->click();
        pump(4);
        if (auto* popup = browser.entityList()->findChild<QFrame*>(QStringLiteral("rpeAddPopup")))
        {
            save(popup, dir + QStringLiteral("/add_entity_picker.png"));
        }
    }

    // ── 1b. Same picker but with NO groups configured → a flat list ────────────
    {
        flecs::world w2;
        w2.prefab("Apple Prefab");
        w2.prefab("Banana Prefab");
        w2.prefab("Cherry Prefab");
        w2.prefab("Date Prefab");
        rpe::EcsMirror m2;
        m2.attach(&w2);
        rpe::EntityComponentBrowser b2;
        b2.setMirror(&m2);
        b2.setEntityAddingEnabled(true); // NO setPrefabGroups → flat
        b2.resize(360, 520);
        b2.show();
        for (int i = 0; i < 30; ++i)
        {
            w2.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(6);
        }
        if (auto* btn = b2.entityList()->findChild<QToolButton*>())
        {
            btn->click();
            QCoreApplication::processEvents();
            if (auto* popup = b2.entityList()->findChild<QFrame*>(QStringLiteral("rpeAddPopup")))
                save(popup, dir + QStringLiteral("/add_entity_flat.png"));
        }
        m2.detach();
    }

    // ── 2. Entity right-click menu (mirrors _onContextMenu's content) ──────────
    {
        QMenu menu;
        menu.setStyleSheet(rpe::darkStyleSheet());
        menu.addAction(QIcon(QStringLiteral(":/rpe/icons/remove.png")), QStringLiteral("Delete entity"));
        menu.addSeparator();
        menu.addAction(dot(QColor(0x66, 0xBB, 0x6A)), QStringLiteral("Focus camera"));
        menu.addAction(dot(QColor(0xF0, 0xC0, 0x50)), QStringLiteral("Duplicate"));
        menu.adjustSize();
        menu.grab(); // realise it
        save(&menu, dir + QStringLiteral("/entity_context_menu.png"));
    }

    mirror.detach();
    return 0;
}

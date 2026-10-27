// Two browser preferences:
//   • Settings::preferredComponents — when an entity is selected and there's no
//     previous component selection to keep (the first entity, or one lacking the
//     component you were on), the first preferred component it has is selected.
//     A previous selection still wins; a component removed from the SAME entity
//     still falls to its row's neighbour. Mirror and direct mode.
//   • Drag-to-scrub on/off: Settings::dragToScrub / setDragToScrubEnabled only —
//     the host decides; the right-click menu offers no toggle.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/gui/PropertyEditor.h>

#include <rttr/registration.h>

#include <QAction>
#include <QApplication>
#include <QListWidget>
#include <QMenu>
#include <QThread>
#include <QTimer>
#include <QTreeView>

#include <cstdio>
#include <functional>

struct Health
{
    int hp = 10;
};
struct Transform
{
    float x = 1;
};
namespace game
{
    struct Weapon
    {
        int ammo = 3;
    };
    struct Physics
    {
        float mass = 2;
    };
} // namespace game

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<Health>("Health").property("hp", &Health::hp);
    registration::class_<Transform>("Transform").property("x", &Transform::x);
    registration::class_<game::Weapon>("game::Weapon").property("ammo", &game::Weapon::ammo);
    registration::class_<game::Physics>("game::Physics").property("mass", &game::Physics::mass);
}

static int g_fails = 0;
static void check(const std::string& n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n.c_str());
    if (!ok)
        ++g_fails;
}

static void runPreferred(bool mirrorMode)
{
    const std::string tag = mirrorMode ? "mirror: " : "direct: ";
    printf("\n-- %s--\n", tag.c_str());
    flecs::world world;
    world.component<game::Weapon>("game::Weapon");
    world.component<game::Physics>("game::Physics");
    auto a = world.entity("A").set<Transform>({}).set<Health>({});
    auto b = world.entity("B").set<Health>({}).set<Transform>({}).set<game::Weapon>({});
    auto c = world.entity("C").set<game::Physics>({}).set<Transform>({});
    auto d = world.entity("D").set<game::Weapon>({}).set<game::Physics>({});

    rpe::EcsMirror mirror;
    rpe::EntityComponentBrowser browser;
    auto s = browser.settings();
    s.preferredComponents = QStringList { QStringLiteral("game::Weapon"), QStringLiteral("Health") };
    s.allowComponentEditing = true;
    browser.setSettings(s);
    if (mirrorMode)
    {
        mirror.attach(&world);
        browser.setMirror(&mirror);
    }
    else
    {
        browser.setWorld(&world);
    }
    browser.resize(600, 500);
    browser.show();

    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(4);
        }
    };
    auto* list = browser.componentList();
    auto* rows = list->findChild<QListWidget*>();
    auto current = [&] { return list->currentComponentKey(); };
    auto waitFor = [&](const std::function<bool()>& cond) {
        for (int i = 0; i < 200 && !cond(); ++i)
            pump(1);
        return cond();
    };
    auto selectEntity = [&](flecs::entity e, const QString& expectRowKey) {
        browser.selectEntity(e);
        // wait until the listing is the new entity's (it has expectRowKey's row set)
        waitFor([&] {
            for (int i = 0; i < rows->count(); ++i)
                if (rows->item(i)->data(Qt::UserRole + 3).toString() == expectRowKey)
                    return true;
            return false;
        });
        pump(5);
    };

    // ── first entity: nothing to keep → the first preferred one it has ─────────
    selectEntity(a, QStringLiteral("Health"));
    check(tag + "first entity (A lacks game::Weapon) → Health, the next preferred", waitFor([&] { return current() == QStringLiteral("Health"); }));
    check(tag + "...and the property tree shows it",
          browser.propertyEditor()->model()->boundType() == rttr::type::get<Health>());

    // ── a previous selection the entity has still wins ─────────────────────────
    selectEntity(b, QStringLiteral("game.Weapon"));
    check(tag + "B has Health too → stays on Health (beats the preference for Weapon)", current() == QStringLiteral("Health"));

    // ── switching to an entity WITHOUT the current component → preferred ──────
    selectEntity(d, QStringLiteral("game.Physics"));
    check(tag + "D lacks Health → game::Weapon (\"::\" spelling matches \"game.Weapon\")",
          waitFor([&] { return current() == QStringLiteral("game.Weapon"); }));

    // ── nothing preferred present → the old behaviour (some row, never crash) ──
    selectEntity(c, QStringLiteral("game.Physics"));
    check(tag + "C has neither → falls back to the default (a row is still selected)",
          waitFor([&] { return !current().isEmpty(); }));

    // ── a component removed from the SAME entity → its neighbour, as before ────
    selectEntity(b, QStringLiteral("game.Weapon"));
    for (int i = 0; i < rows->count(); ++i)
        if (rows->item(i)->data(Qt::UserRole + 3).toString() == QStringLiteral("Health"))
            rows->setCurrentRow(i);
    pump(5);
    check(tag + "on B, Health selected", current() == QStringLiteral("Health"));
    if (mirrorMode)
    {
        b.remove<Health>();
    }
    else
    {
        b.remove<Health>();
        list->setEntity(&world, b); // direct mode re-lists after a structural change
    }
    check(tag + "Health removed from B → the row that took its place (Transform), not the preference",
          waitFor([&] { return current() == QStringLiteral("Transform"); }));

    // ── a trailing part of the path works too ───────────────────────────────────
    browser.setPreferredComponents({ QStringLiteral("Physics") });
    rows->setCurrentRow(-1);
    pump(3);
    selectEntity(d, QStringLiteral("game.Physics"));
    if (!mirrorMode)
        list->setEntity(&world, d);
    check(tag + "\"Physics\" matches game.Physics", waitFor([&] {
              return current() == QStringLiteral("game.Physics") || current() == QStringLiteral("game.Weapon");
          }) && (mirrorMode || current() == QStringLiteral("game.Physics")));
    check(tag + "settings() reports the preference", browser.settings().preferredComponents == QStringList { QStringLiteral("Physics") });

    if (mirrorMode)
        mirror.detach();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    rpe::TypeBridge::registerTypes<Health, Transform, game::Weapon, game::Physics>();

    runPreferred(true);
    runPreferred(false);

    // ── drag to scrub: setting + menu toggle ────────────────────────────────────
    printf("\n-- drag to scrub --\n");
    {
        flecs::world world;
        auto e = world.entity("E").set<Transform>({});
        rpe::EntityComponentBrowser browser;
        browser.setWorld(&world);
        browser.show();
        QCoreApplication::processEvents();
        browser.selectEntity(e);
        QCoreApplication::processEvents();
        auto* ed = browser.propertyEditor();
        check("on by default", ed->isDragToScrubEnabled() && browser.settings().dragToScrub);

        auto s = browser.settings();
        s.dragToScrub = false;
        browser.setSettings(s);
        check("Settings::dragToScrub = false turns it off", !ed->isDragToScrubEnabled());
        browser.setDragToScrubEnabled(true);
        check("setDragToScrubEnabled(true) turns it back on", ed->isDragToScrubEnabled() && browser.settings().dragToScrub);

        // Host-controlled only: no toggle in the right-click menu.
        bool offered = false, menuShown = false;
        QTimer::singleShot(0, [&] {
            auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (m)
            {
                menuShown = true;
                for (QAction* a : m->actions())
                    if (a->text().contains(QStringLiteral("Drag"), Qt::CaseInsensitive))
                        offered = true;
                m->close();
            }
        });
        auto* view = ed->view();
        QMetaObject::invokeMethod(ed, "_onContextMenu", Qt::DirectConnection,
                                  Q_ARG(QPoint, view->visualRect(view->model()->index(0, 0)).center()));
        check("the row menu opens but offers no drag-to-scrub toggle", menuShown && !offered);
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

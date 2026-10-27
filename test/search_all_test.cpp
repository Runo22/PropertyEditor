// "Search all components": the property filter applied to every component of the
// selected entity.
//   • scope OFF: the tree filters the selected component as before, and the
//     component list shows a match count per component (others dimmed);
//   • scope ON (the button beside the filter, Ctrl+Shift+F): every match in one
//     grouped tree — a header per component — with live values, editable in place;
//   • picking a row there follows its component in the list; turning the scope
//     off lands on that component with the row selected;
//   • matches by NAME and by VALUE; a row that stops matching leaves the view;
//   • the search runs on the sim thread in mirror mode (time-sliced) and on a
//     timer in direct mode — both are covered.
// Also: rpe::searchProperties on its own, and the Reset button only showing when
// something is frozen is in reset_live_test.
#include <rpe/core/PropertySearch.h>
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>
#include <rpe/ecs/PinnedPropertiesWidget.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QImage>
#include <QPalette>
#include <QLineEdit>
#include <QListWidget>
#include <QSortFilterProxyModel>
#include <QThread>
#include <QToolButton>
#include <QTreeView>

#include <algorithm>
#include <cstdio>
#include <functional>

struct Vec3
{
    float x = 0, y = 0, z = 0;
};
struct Transform
{
    Vec3 position { 12.5f, 0.f, -4.f };
    float yaw = 90.f;
};
struct Physics
{
    float mass = 80.f;
    float maxSpeed = 12.5f;
    float angularSpeed = 3.f;
};
struct Health
{
    int hp = 74;
    float regenPerSecond = 1.5f;
};
struct AIBrain
{
    float patrolSpeed = 4.f;
    std::string goal = "patrol";
};
struct Weapon
{
    int ammo = 24;
    float projectileSpeed = 80.f;
};

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<Vec3>("Vec3").property("x", &Vec3::x).property("y", &Vec3::y).property("z", &Vec3::z);
    registration::class_<Transform>("Transform").property("position", &Transform::position).property("yaw", &Transform::yaw);
    registration::class_<Physics>("Physics")
        .property("mass", &Physics::mass)
        .property("maxSpeed", &Physics::maxSpeed)
        .property("angularSpeed", &Physics::angularSpeed);
    registration::class_<Health>("Health").property("hp", &Health::hp).property("regenPerSecond", &Health::regenPerSecond);
    registration::class_<AIBrain>("AIBrain").property("patrolSpeed", &AIBrain::patrolSpeed).property("goal", &AIBrain::goal);
    registration::class_<Weapon>("Weapon").property("ammo", &Weapon::ammo).property("projectileSpeed", &Weapon::projectileSpeed);
}

static int g_fails = 0;
static void check(const std::string& n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n.c_str());
    if (!ok)
        ++g_fails;
}

// Visible (proxy) rows under the view's root, as "header/relpath" strings, plus
// the value shown for a path.
static QStringList visibleRows(QTreeView* view)
{
    QStringList out;
    auto* m = view->model();
    std::function<void(const QModelIndex&, const QString&)> walk = [&](const QModelIndex& parent, const QString& group) {
        for (int r = 0; r < m->rowCount(parent); ++r)
        {
            const QModelIndex i = m->index(r, 0, parent);
            const bool header = i.data(rpe::IsGroupRole).toBool();
            const QString g = header ? i.data(Qt::DisplayRole).toString() : group;
            if (!header)
            {
                QString key, rel;
                rpe::PropertyModel::splitGroupPath(i.data(rpe::PropertyPathRole).toString(), &key, &rel);
                if (m->rowCount(i) == 0)
                    out << (g.isEmpty() ? rel : g + QLatin1Char('/') + rel);
            }
            walk(i, g);
        }
    };
    walk({}, QString());
    return out;
}

static QModelIndex rowFor(QTreeView* view, const QString& group, const QString& rel)
{
    auto* m = view->model();
    QModelIndex found;
    std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int r = 0; r < m->rowCount(parent) && !found.isValid(); ++r)
        {
            const QModelIndex i = m->index(r, 0, parent);
            QString key, p;
            const bool grouped = rpe::PropertyModel::splitGroupPath(i.data(rpe::PropertyPathRole).toString(), &key, &p);
            if (!i.data(rpe::IsGroupRole).toBool() && p == rel && (!grouped || key == group))
                found = i;
            walk(i);
        }
    };
    walk({});
    return found;
}

static QString shown(QTreeView* view, const QString& group, const QString& rel)
{
    return rowFor(view, group, rel).siblingAtColumn(1).data().toString();
}

static void selectRow(QListWidget* list, const QString& text)
{
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == text)
            list->setCurrentRow(i);
}

static void runBrowser(bool mirrorMode)
{
    const std::string tag = mirrorMode ? "mirror: " : "direct: ";
    printf("\n-- %s--\n", tag.c_str());
    flecs::world world;
    auto player = world.entity("Player");
    player.set<Transform>({}).set<Physics>({}).set<Health>({}).set<AIBrain>({}).set<Weapon>({});
    auto other = world.entity("Other");
    other.set<Physics>({}).set<Health>({});

    rpe::EcsMirror mirror;
    rpe::EntityComponentBrowser browser;
    rpe::PinnedPropertiesWidget pins;
    if (mirrorMode)
    {
        mirror.attach(&world);
        browser.setMirror(&mirror);
        browser.setPinnedPropertiesWidget(&pins);
    }
    else
    {
        browser.setWorld(&world);
        browser.setEditPolicy(rpe::EditPolicy::WriteBack);
    }
    browser.resize(700, 600);
    browser.show();

    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(4);
        }
    };
    auto waitFor = [&](const std::function<bool()>& cond) {
        for (int i = 0; i < 300 && !cond(); ++i)
            pump(1);
        return cond();
    };

    auto* ed = browser.propertyEditor();
    auto* view = ed->view();
    auto* comps = browser.componentList()->findChild<QListWidget*>();
    auto* list = browser.componentList();
    auto* filter = ed->findChild<QLineEdit*>();
    QToolButton* scope = nullptr;
    for (auto* b : ed->findChildren<QToolButton*>())
        if (b->isCheckable())
            scope = b;

    check(tag + "the browser shows the scope button", scope && !scope->isHidden());

    browser.selectEntity(player);
    pump(10);
    waitFor([&] { return comps->count() == 5; });
    selectRow(comps, QStringLiteral("Health"));
    pump(5);

    // ── scope OFF: list counts, tree unchanged ─────────────────────────────────
    filter->setText(QStringLiteral("speed"));
    check(tag + "list counts arrive: Physics 2, AIBrain 1, Weapon 1", waitFor([&] {
              return list->matchCountsActive() && list->matchCount(QStringLiteral("Physics")) == 2
                  && list->matchCount(QStringLiteral("AIBrain")) == 1 && list->matchCount(QStringLiteral("Weapon")) == 1;
          }));
    check(tag + "...and none for Health / Transform",
          list->matchCount(QStringLiteral("Health")) == 0 && list->matchCount(QStringLiteral("Transform")) == 0);
    check(tag + "scope off: the tree still filters only the selected component (Health → nothing)",
          visibleRows(view).isEmpty() && !ed->model()->isGrouped());

    // ── scope ON: grouped matches with values ──────────────────────────────────
    scope->click();
    check(tag + "scope on → grouped view", ed->model()->isGrouped());
    check(tag + "every match, grouped by component", waitFor([&] {
              const QStringList rows = visibleRows(view);
              return rows.contains(QStringLiteral("Physics/maxSpeed")) && rows.contains(QStringLiteral("Physics/angularSpeed"))
                  && rows.contains(QStringLiteral("AIBrain/patrolSpeed")) && rows.contains(QStringLiteral("Weapon/projectileSpeed"))
                  && rows.size() == 4;
          }));
    check(tag + "with live values", waitFor([&] {
              return shown(view, QStringLiteral("Physics"), QStringLiteral("maxSpeed")).startsWith(QStringLiteral("12.5"))
                  && shown(view, QStringLiteral("Weapon"), QStringLiteral("projectileSpeed")).startsWith(QStringLiteral("80"));
          }));
    {
        QModelIndex physicsHeader;
        for (int r = 0; r < view->model()->rowCount(); ++r)
            if (view->model()->index(r, 0).data(Qt::DisplayRole).toString() == QStringLiteral("Physics"))
                physicsHeader = view->model()->index(r, 0);
        check(tag + "a header carries its match count", waitFor([&] { return physicsHeader.data(rpe::GroupCountRole).toInt() == 2; }));
        check(tag + "headers aren't editable", !(physicsHeader.siblingAtColumn(1).flags() & Qt::ItemIsEditable));
        check(tag + "headers carry no FontRole (a QFont() there would be the app font, not the view's)",
              !physicsHeader.data(Qt::FontRole).isValid());
    }

    // ── by value, and nested ───────────────────────────────────────────────────
    filter->setText(QStringLiteral("patrol"));
    check(tag + "a string VALUE matches too (goal = \"patrol\")", waitFor([&] {
              const QStringList rows = visibleRows(view);
              return rows.contains(QStringLiteral("AIBrain/goal")) && rows.contains(QStringLiteral("AIBrain/patrolSpeed"));
          }));
    filter->setText(QStringLiteral("80"));
    check(tag + "a number's VALUE matches (projectileSpeed = 80, mass = 80)", waitFor([&] {
              const QStringList rows = visibleRows(view);
              return rows.contains(QStringLiteral("Weapon/projectileSpeed")) && rows.contains(QStringLiteral("Physics/mass"));
          }));
    filter->setText(QStringLiteral("position"));
    check(tag + "a nested struct matches by name", waitFor([&] {
              return !visibleRows(view).filter(QStringLiteral("Transform/position")).isEmpty();
          }));

    // ── edit in place ──────────────────────────────────────────────────────────
    filter->setText(QStringLiteral("speed"));
    waitFor([&] { return shown(view, QStringLiteral("AIBrain"), QStringLiteral("patrolSpeed")).startsWith(QStringLiteral("4")); });
    {
        const QModelIndex cell = rowFor(view, QStringLiteral("AIBrain"), QStringLiteral("patrolSpeed")).siblingAtColumn(1);
        check(tag + "a grouped row is editable", bool(cell.flags() & Qt::ItemIsEditable));
        view->model()->setData(cell, QVariant::fromValue(rttr::variant(7.0f)), Qt::EditRole);
        check(tag + "...and the edit lands in ITS component", waitFor([&] { return player.get<AIBrain>().patrolSpeed == 7.f; }));
        check(tag + "...not in the selected one", player.get<Health>().regenPerSecond == 1.5f);
        check(tag + "the grouped view shows the new value",
              waitFor([&] { return shown(view, QStringLiteral("AIBrain"), QStringLiteral("patrolSpeed")).startsWith(QStringLiteral("7")); }));
    }

    // ── a row that stops matching leaves ───────────────────────────────────────
    filter->setText(QStringLiteral("74"));
    check(tag + "value match: hp = 74", waitFor([&] { return visibleRows(view).contains(QStringLiteral("Health/hp")); }));
    player.get_mut<Health>().hp = 50;
    player.modified<Health>();
    check(tag + "after hp changes to 50 the row drops out", waitFor([&] { return !visibleRows(view).contains(QStringLiteral("Health/hp")); }));

    // ── pick a row → the list follows; scope off → that component, row selected ─
    filter->setText(QStringLiteral("speed"));
    waitFor([&] { return rowFor(view, QStringLiteral("Weapon"), QStringLiteral("projectileSpeed")).isValid(); });
    view->setCurrentIndex(rowFor(view, QStringLiteral("Weapon"), QStringLiteral("projectileSpeed")));
    pump(2);
    check(tag + "picking a grouped row selects its component in the list",
          list->currentComponentKey() == QStringLiteral("Weapon") && ed->model()->isGrouped());
    if (mirrorMode)
    {
        emit ed->pinRequested(view->currentIndex().data(rpe::PropertyPathRole).toString());
        pump(3);
        check(tag + "pinning a grouped row pins its own component + path",
              pins.isPinned(static_cast<qulonglong>(player.id()), QStringLiteral("Weapon"), QStringLiteral("projectileSpeed")));
    }
    scope->click();
    check(tag + "scope off → back to one component: the picked one", !ed->model()->isGrouped()
              && ed->model()->boundType() == rttr::type::get<Weapon>());
    check(tag + "...with the filter kept and the picked row selected",
          filter->text() == QStringLiteral("speed")
              && view->currentIndex().data(rpe::PropertyPathRole).toString() == QStringLiteral("projectileSpeed"));

    // ── another entity keeps the grouped view, for its own components ──────────
    scope->click();
    browser.selectEntity(other);
    check(tag + "another entity: its matches only", waitFor([&] {
              const QStringList rows = visibleRows(view);
              return ed->model()->isGrouped() && rows.contains(QStringLiteral("Physics/maxSpeed"))
                  && !rows.contains(QStringLiteral("Weapon/projectileSpeed"));
          }));

    // ── empty filter ends it ───────────────────────────────────────────────────
    filter->clear();
    pump(3);
    check(tag + "clearing the filter: no grouped view, no counts", !ed->model()->isGrouped() && !list->matchCountsActive());
    if (mirrorMode)
        mirror.detach();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    rpe::TypeBridge::registerTypes<Transform, Physics, Health, AIBrain, Weapon>();

    // ── the search core ────────────────────────────────────────────────────────
    printf("\n-- searchProperties --\n");
    {
        Transform t;
        std::vector<rpe::PropertyHit> hits;
        rpe::searchProperties(rttr::instance(t), rttr::type::get<Transform>(), QStringLiteral("x"), hits);
        check("a nested leaf matches by name, with its full path",
              hits.size() == 1 && hits[0].path == QStringLiteral("position.x") && hits[0].value.to_float() == 12.5f);
        hits.clear();
        rpe::searchProperties(rttr::instance(t), rttr::type::get<Transform>(), QStringLiteral("POSITION"), hits);
        check("a struct matching by name is one hit, carrying the whole struct (case-insensitive)",
              hits.size() == 1 && hits[0].path == QStringLiteral("position") && hits[0].value.get_type() == rttr::type::get<Vec3>());
        hits.clear();
        rpe::searchProperties(rttr::instance(t), rttr::type::get<Transform>(), QStringLiteral("-4"), hits);
        check("a number matches by value", hits.size() == 1 && hits[0].path == QStringLiteral("position.z"));
        hits.clear();
        rpe::searchProperties(rttr::instance(t), rttr::type::get<Transform>(), QStringLiteral("   "), hits);
        check("a blank needle finds nothing", hits.empty());
    }
    {
        rpe::PropertyEditor standalone;
        QToolButton* scope = nullptr;
        for (auto* b : standalone.findChildren<QToolButton*>())
            if (b->isCheckable())
                scope = b;
        check("a standalone PropertyEditor has no scope button", scope && scope->isHidden() && !standalone.isSearchAllAvailable());
    }

    runBrowser(true);
    runBrowser(false);

    // ── colours that must not depend on focus or on when styling happened ──────
    printf("\n-- colours --\n");
    {
        // Windows-like: an INACTIVE highlight is grey. Typing in the filter leaves
        // the tree inactive, and the match used to be painted in that grey.
        QPalette pal = app.palette();
        pal.setColor(QPalette::Active, QPalette::Highlight, QColor(0x30, 0x6c, 0xe6));
        pal.setColor(QPalette::Inactive, QPalette::Highlight, QColor(0xc0, 0xc0, 0xc0));
        app.setPalette(pal);
        Physics ph;
        rpe::PropertyEditor ed;
        ed.editObject(ph);
        ed.resize(400, 200);
        ed.show();
        ed.findChild<QLineEdit*>()->setFocus();
        ed.setFilterText(QStringLiteral("speed"));
        QCoreApplication::processEvents();
        const QImage img = ed.view()->viewport()->grab().toImage();
        int blue = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < std::min(img.width(), 200); ++x)
            {
                const QColor c = img.pixelColor(x, y);
                if (c.blue() > c.red() + 60 && c.blue() > c.green() + 30)
                    ++blue;
            }
        check("the matched text is in the ACTIVE accent while the filter has focus (not the grey inactive one)", blue > 20);
        app.setPalette(QPalette());
    }
    {
        // Names keep the STYLE's text colour and font while filtering (a style-sheet
        // ::item colour, a pixel font size): the delegate only tints behind the
        // match, it never re-draws the text itself.
        app.setStyleSheet(QStringLiteral("QWidget { background: #2b2b2b; color: #dddddd; font-size: 15px; }"
                                         " QTreeView::item { color: #ffcc66; }"));
        Physics ph;
        rpe::PropertyEditor ed;
        ed.editObject(ph);
        ed.resize(400, 200);
        ed.show();
        auto countOrange = [&] {
            QCoreApplication::processEvents();
            const QImage img = ed.view()->viewport()->grab().toImage();
            int n = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < std::min(img.width(), 200); ++x)
                {
                    const QColor c = img.pixelColor(x, y);
                    if (c.red() > 200 && c.green() > 150 && c.green() < 230 && c.blue() < 140)
                        ++n;
                }
            return n;
        };
        const int before = countOrange();
        ed.setFilterText(QStringLiteral("speed"));
        const int during = countOrange();
        check("names keep the style sheet's ::item colour while filtering", before > 20 && during > 20);
        app.setStyleSheet(QString());
    }
    {
        // The scope glyph is drawn in the button's text colour once styling is
        // known — it used to start black under a dark style sheet.
        app.setStyleSheet(QStringLiteral("QWidget { color: #e0e0e0; background: #202020; }"));
        rpe::PropertyEditor ed;
        ed.setSearchAllAvailable(true);
        ed.show();
        QCoreApplication::processEvents();
        QToolButton* scope = nullptr;
        for (auto* b : ed.findChildren<QToolButton*>())
            if (b->isCheckable())
                scope = b;
        auto glyph = [&] {
            const QImage icon = scope->icon().pixmap(32, 32).toImage();
            QColor best(0, 0, 0, 0);
            for (int y = 0; y < icon.height(); ++y)
                for (int x = 0; x < icon.width(); ++x)
                    if (icon.pixelColor(x, y).alpha() > best.alpha())
                        best = icon.pixelColor(x, y);
            return best;
        };
        check("the scope glyph starts in the styled text colour (light), not black", scope && glyph().lightness() > 160);
        scope->click();
        QCoreApplication::processEvents();
        check("...and stays light when checked", glyph().lightness() > 160);
        app.setStyleSheet(QString());
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

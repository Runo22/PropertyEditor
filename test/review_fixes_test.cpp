// Regression tests for the PR #9 review findings. Each block reproduces what was
// reported (a repro first, then the fix's expected behaviour).
#include <rpe/core/EditorHints.h>
#include <rpe/core/RttrBridge.h>
#include <rpe/core/TypeRelease.h>
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>
#include <rpe/ecs/PinnedPropertiesWidget.h>
#include <rpe/gui/EditorWidgets.h>
#include <rpe/gui/PropertyDelegate.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QAction>
#include <QComboBox>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QThread>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

// ── types ────────────────────────────────────────────────────────────────────────
struct V3
{
    double x = 1, y = 2, z = 3;
};
struct Cam
{
    V3 position;
    double fov = 60;
    float speed = 1.0f;
};
struct Transform
{
    V3 position { 12.5, 0, -4 };
    float yaw = 90.f;
};
struct Physics
{
    float mass = 80.f;
    float maxSpeed = 12.5f;
};
struct Health
{
    int hp = 74;
};
struct Weapon
{
    int ammo = 24;
    float projectileSpeed = 80.f;
};
struct Tagged
{
};
struct Gauge // a read-only value + a writable one
{
    double level() const { return 42.5; }
    float speed = 1.25f;
    unsigned int count = 2;
};
struct Rect
{
    float x = 0, y = 0, w = 10, h = 5;
};
enum class Mode
{
    Idle,
    Run,
    Boost
};
struct Engine // never bridged: shown through editObject only
{
    Mode mode = Mode::Run;
    std::vector<V3> path { V3 {}, V3 {} };
};

#define DECL(N)                                                                                                       \
    struct Big##N                                                                                                     \
    {                                                                                                                 \
        std::vector<float> v = std::vector<float>(256, 1.25f);                                                        \
        std::vector<std::string> s = std::vector<std::string>(256, "hello1");                                         \
    };                                                                                                                \
    struct Small##N                                                                                                   \
    {                                                                                                                 \
        int a = N;                                                                                                    \
    };
DECL(0) DECL(1) DECL(2) DECL(3) DECL(4) DECL(5) DECL(6) DECL(7)
#define REG(N)                                                                                                        \
    registration::class_<Big##N>("Big" #N).property("v", &Big##N::v).property("s", &Big##N::s);                       \
    registration::class_<Small##N>("Small" #N).property("a", &Small##N::a);
#define ALL(P) P##0, P##1, P##2, P##3, P##4, P##5, P##6, P##7

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<V3>("V3").property("x", &V3::x).property("y", &V3::y).property("z", &V3::z);
    registration::class_<Cam>("Cam")
        .property("position", &Cam::position)
        .property("fov", &Cam::fov)(metadata(rpe::hint::Min, 10.0), metadata(rpe::hint::Max, 120.0),
                                    metadata(rpe::hint::Editor, rpe::editor::Slider))
        .property("speed", &Cam::speed);
    registration::class_<Transform>("Transform").property("position", &Transform::position).property("yaw", &Transform::yaw);
    registration::class_<Physics>("Physics").property("mass", &Physics::mass).property("maxSpeed", &Physics::maxSpeed);
    registration::class_<Health>("Health").property("hp", &Health::hp);
    registration::class_<Gauge>("Gauge")
        .property_readonly("level", &Gauge::level)
        .property("speed", &Gauge::speed)(metadata(rpe::hint::Decimals, 1))
        .property("count", &Gauge::count);
    registration::class_<Rect>("Rect").property("x", &Rect::x).property("y", &Rect::y).property("w", &Rect::w).property("h", &Rect::h);
    registration::enumeration<Mode>("Mode")(value("Idle", Mode::Idle), value("Run", Mode::Run), value("Boost", Mode::Boost));
    registration::class_<Engine>("Engine").property("mode", &Engine::mode).property("path", &Engine::path);
    registration::class_<Weapon>("Weapon").property("ammo", &Weapon::ammo).property("projectileSpeed", &Weapon::projectileSpeed);
    REG(0) REG(1) REG(2) REG(3) REG(4) REG(5) REG(6) REG(7)
}

static int g_fails = 0;
static void check(const std::string& n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n.c_str());
    if (!ok)
        ++g_fails;
}

static QModelIndex topRow(QTreeView* v, const QString& name, int col)
{
    auto* m = v->model();
    for (int r = 0; r < m->rowCount(); ++r)
        if (m->index(r, 0).data().toString() == name)
            return m->index(r, col);
    return {};
}

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
            if (!header && m->rowCount(i) == 0)
            {
                QString key, rel;
                rpe::PropertyModel::splitGroupPath(i.data(rpe::PropertyPathRole).toString(), &key, &rel);
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

static void sendKey(QWidget* to, int key, Qt::KeyboardModifiers mods)
{
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QCoreApplication::sendEvent(to, &press);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    rpe::TypeBridge::registerTypes<Transform, Physics, Health, Weapon>();

    // ── 1. Ctrl+Shift+F must not steal (or make ambiguous) the host's shortcut ─────
    printf("\n-- shortcut --\n");
    {
        QWidget host;
        int hostFired = 0;
        auto* act = new QAction(&host);
        act->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
        act->setShortcutContext(Qt::WindowShortcut);
        QObject::connect(act, &QAction::triggered, [&] { ++hostFired; });
        host.addAction(act);
        auto* lay = new QVBoxLayout(&host);
        Cam cam;
        auto* ed = new rpe::PropertyEditor(&host);
        ed->editObject(cam);
        lay->addWidget(ed);
        host.show();
        QApplication::setActiveWindow(&host);
        ed->view()->setFocus();
        QCoreApplication::processEvents();

        sendKey(ed->view(), Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
        check("a plain PropertyEditor (no search scope) leaves the host's Ctrl+Shift+F alone", hostFired == 1);

        ed->setSearchAllAvailable(true);
        ed->setSearchAllShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F));
        sendKey(ed->view(), Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
        check("with the scope on other keys, the host's Ctrl+Shift+F still fires", hostFired == 2 && !ed->isSearchAll());
        sendKey(ed->view(), Qt::Key_F, Qt::ControlModifier | Qt::AltModifier);
        check("...and the editor's own keys toggle the scope", ed->isSearchAll());
        ed->setSearchAllShortcut(QKeySequence());
        sendKey(ed->view(), Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
        check("an empty shortcut disables the editor's key entirely", hostFired == 3);
    }

    // ── 2. EcsMirror re-attached mid search round ────────────────────────────────
    printf("\n-- mirror re-attach during a search round --\n");
    {
        rpe::TypeBridge::registerTypes<ALL(Big), ALL(Small)>();
        flecs::world w1;
        auto e1 = w1.entity("E");
        (e1.set<Big0>({}), e1.set<Big1>({}), e1.set<Big2>({}), e1.set<Big3>({}), e1.set<Big4>({}), e1.set<Big5>({}),
         e1.set<Big6>({}), e1.set<Big7>({}));
        rpe::EcsMirror mirror;
        mirror.attach(&w1, rpe::EcsMirror::PumpMode::Manual);
        auto ch = mirror.channel();
        ch->setInterest(e1.id(), QString(), {});
        ch->setSearch(e1.id(), QStringLiteral("1"));
        w1.progress();
        mirror.pump(); // a round starts and is sliced: still running

        flecs::world w2;
        auto e2 = w2.entity("E");
        (e2.set<Small0>({}), e2.set<Small1>({}), e2.set<Small2>({}), e2.set<Small3>({}), e2.set<Small4>({}),
         e2.set<Small5>({}), e2.set<Small6>({}), e2.set<Small7>({}));
        mirror.attach(&w2, rpe::EcsMirror::PumpMode::Manual);
        ch->setInterest(e2.id(), QString(), {});
        ch->setSearch(e2.id(), QStringLiteral("1"));
        rpe::MirrorChannel::SearchResults r;
        bool got = false;
        for (int i = 0; i < 40 && !got; ++i)
        {
            w2.progress();
            mirror.pump();
            got = ch->pollSearchResults(r);
        }
        bool allSmall = got;
        for (const auto& h : r.hits)
            allSmall = allSmall && h.component.startsWith(QStringLiteral("Small"));
        check("re-attaching to another world mid-round doesn't crash, and the next round reads the new world", allSmall);
        mirror.detach();
    }

    // ── 3. Mirror: a row matching only by VALUE shows in the single-component view
    printf("\n-- value-only matches, mirror mode --\n");
    {
        flecs::world world;
        auto player = world.entity("Player").set<Transform>({}).set<Physics>({}).set<Health>({}).set<Weapon>({});
        rpe::EcsMirror mirror;
        mirror.attach(&world);
        rpe::EntityComponentBrowser browser;
        browser.setMirror(&mirror);
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
        browser.selectEntity(player);
        waitFor([&] { return comps->count() == 4; });
        ed->setSearchAll(true);
        ed->setFilterText(QStringLiteral("80"));
        check("grouped: Weapon/projectileSpeed matches by value",
              waitFor([&] { return shown(view, QStringLiteral("Weapon"), QStringLiteral("projectileSpeed")).startsWith(QStringLiteral("80")); }));
        view->setCurrentIndex(rowFor(view, QStringLiteral("Weapon"), QStringLiteral("projectileSpeed")));
        pump(3);
        ed->setSearchAll(false);
        check("scope off: the value match shows in Weapon's own tree, and is selected", waitFor([&] {
                  return view->currentIndex().data(rpe::PropertyPathRole).toString() == QStringLiteral("projectileSpeed")
                      && shown(view, QString(), QStringLiteral("projectileSpeed")).startsWith(QStringLiteral("80"));
              }));
        mirror.detach();
    }

    // ── 4. Composite editors commit when focus moves elsewhere in the window ──────
    printf("\n-- composite editors and focus --\n");
    {
        Cam cam;
        QWidget host;
        auto* lay = new QVBoxLayout(&host);
        auto* other = new QLineEdit(&host);
        auto* ed = new rpe::PropertyEditor(&host);
        lay->addWidget(other);
        lay->addWidget(ed);
        ed->editObject(cam);
        host.resize(500, 300);
        host.show();
        QApplication::setActiveWindow(&host);
        QCoreApplication::processEvents();

        const QModelIndex vi = topRow(ed->view(), QStringLiteral("position"), 1);
        ed->view()->setCurrentIndex(vi);
        ed->view()->edit(vi);
        QCoreApplication::processEvents();
        auto* ve = ed->view()->viewport()->findChild<rpe::VectorEditor*>();
        if (ve)
        {
            auto* sb = static_cast<QDoubleSpinBox*>(ve->spinBox(0));
            sb->setFocus();
            QCoreApplication::processEvents();
            sb->setValue(7.5);
            other->setFocus(Qt::MouseFocusReason);
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        check("vector editor: focus leaving commits the value", ve && cam.position.x == 7.5);
        check("...and closes the editor", !ed->view()->viewport()->findChild<rpe::VectorEditor*>());

        const QModelIndex fi = topRow(ed->view(), QStringLiteral("fov"), 1);
        ed->view()->setFocus();
        ed->view()->setCurrentIndex(fi);
        ed->view()->edit(fi);
        QCoreApplication::processEvents();
        auto* se = ed->view()->viewport()->findChild<rpe::SliderEditor*>();
        if (se)
        {
            auto* sb = static_cast<QDoubleSpinBox*>(se->spinBox());
            sb->setFocus();
            QCoreApplication::processEvents();
            sb->setValue(90);
            other->setFocus(Qt::MouseFocusReason);
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        check("slider editor: focus leaving commits the value and closes it",
              se && cam.fov == 90 && !ed->view()->viewport()->findChild<rpe::SliderEditor*>());
    }

    // ── 5. Type release details ────────────────────────────────────────────────────
    printf("\n-- type release --\n");
    {
        const std::vector<rttr::type> rel { rttr::type::get<V3>() };
        check("isReleased looks into containers: std::vector<V3>", rpe::isReleased(rttr::type::get<std::vector<V3>>(), rel));
        check("...and not at unrelated types", !rpe::isReleased(rttr::type::get<std::vector<int>>(), rel));

        // A type that was never bridged, shown through editObject: unregisterType
        // still releases it (a plugin object in a standalone editor).
        Engine eng;
        rpe::PropertyEditor ed;
        ed.editObject(eng);
        QString releasedName = QStringLiteral("<none>");
        QObject::connect(&ed, &rpe::PropertyEditor::boundTypeReleased, [&](const QString& n) { releasedName = n; });
        rpe::TypeBridge::unregisterType<Engine>();
        QCoreApplication::processEvents();
        check("an unbridged type shown in an editor is released too", releasedName == QStringLiteral("Engine")
                  && ed.model()->boundType() != rttr::type::get<Engine>());

        rpe::ownTypes<std::vector<float>, std::map<std::string, int>>();
        check("ownTypes<…>() registers template types in the calling module",
              rttr::type::get_by_name("std::vector<float>").is_valid() || rttr::type::get<std::vector<float>>().is_valid());
    }

    // ── 6. Enum edits travel as integers and come back as the enum ─────────────────
    printf("\n-- enum edits --\n");
    {
        const rttr::variant back = rpe::bridge::coerce(rttr::variant(int64_t(2)), rttr::type::get<Mode>());
        check("coerce(int → enum) gives the enum value", back.get_type() == rttr::type::get<Mode>() && back.get_value<Mode>() == Mode::Boost);
        check("coerce(int with no such value → enum) gives nothing", !rpe::bridge::coerce(rttr::variant(int64_t(9)), rttr::type::get<Mode>()).is_valid());

        Engine eng;
        rpe::PropertyEditor ed;
        ed.editObject(eng); // WriteBack
        ed.resize(400, 200);
        ed.show();
        QCoreApplication::processEvents();
        auto* view = ed.view();
        const QModelIndex mi = topRow(view, QStringLiteral("mode"), 1);
        view->setCurrentIndex(mi);
        view->edit(mi);
        QCoreApplication::processEvents();
        auto* combo = view->viewport()->findChild<QComboBox*>();
        if (combo)
        {
            combo->setCurrentText(QStringLiteral("Idle"));
            // Commit and close the way the view does (it owns the open editor).
            auto* dlg = view->itemDelegateForColumn(1);
            emit dlg->commitData(combo);
            emit dlg->closeEditor(combo, QAbstractItemDelegate::NoHint);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        check("an enum edit through the editor lands as the enum", combo && eng.mode == Mode::Idle);
        check("...and the row shows its NAME, not a number", topRow(view, QStringLiteral("mode"), 1).data().toString() == QStringLiteral("Idle"));

        // LocalEdit draft: shown as the enum name too.
        Engine eng2;
        rpe::PropertyEditor le;
        le.bindType(rttr::type::get<Engine>());
        le.refresh(rttr::instance(eng2));
        const QModelIndex mi2 = topRow(le.view(), QStringLiteral("mode"), 1);
        le.view()->model()->setData(mi2, QVariant::fromValue(rttr::variant(int64_t(2))), Qt::EditRole);
        check("a LocalEdit draft from an integer shows the enum name", topRow(le.view(), QStringLiteral("mode"), 1).data().toString() == QStringLiteral("Boost"));
    }

    // ── 7. Grouped search view (both modes) ────────────────────────────────────────
    for (bool mirrorMode : { true, false })
    {
        const std::string tag = mirrorMode ? "mirror: " : "direct: ";
        printf("\n-- grouped view, %s--\n", tag.c_str());
        flecs::world world;
        auto player = world.entity("Player").set<Transform>({}).set<Physics>({}).set<Health>({}).set<Weapon>({});
        auto other = world.entity("Other").set<Physics>({}).set<Health>({});
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
            browser.setComponentEditingEnabled(true);
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
        auto* list = browser.componentList();
        auto* comps = list->findChild<QListWidget*>();
        browser.selectEntity(player);
        waitFor([&] { return comps->count() == 4; });
        ed->setSearchAll(true);

        // Stale fields of a struct hit that stopped matching.
        ed->setFilterText(QStringLiteral("position"));
        waitFor([&] { return shown(view, QStringLiteral("Transform"), QStringLiteral("position.y")) == QStringLiteral("0"); });
        player.get_mut<Transform>().position.y = 7;
        player.modified<Transform>();
        pump(5);
        ed->setFilterText(QStringLiteral("0"));
        pump(5);
        check(tag + "a struct hit that stopped matching takes its fields along (no stale position.y = 0)",
              waitFor([&] { return visibleRows(view).contains(QStringLiteral("Transform/yaw")); })
                  && !visibleRows(view).contains(QStringLiteral("Transform/position.y")));

        // Header counts = the list's badges.
        ed->setFilterText(QStringLiteral("speed"));
        waitFor([&] { return list->matchCount(QStringLiteral("Weapon")) == 1; });
        QModelIndex weaponHeader;
        for (int r = 0; r < view->model()->rowCount(); ++r)
            if (view->model()->index(r, 0).data(rpe::GroupKeyRole).toString() == QStringLiteral("Weapon"))
                weaponHeader = view->model()->index(r, 0);
        check(tag + "a header's count is the list badge's count",
              waitFor([&] { return weaponHeader.data(rpe::GroupCountRole).toInt() == list->matchCount(QStringLiteral("Weapon")); }));

        // The picked row stays put when the entity's rows republish (a tag added).
        waitFor([&] { return rowFor(view, QStringLiteral("Weapon"), QStringLiteral("projectileSpeed")).isValid(); });
        view->setCurrentIndex(rowFor(view, QStringLiteral("Weapon"), QStringLiteral("projectileSpeed")));
        pump(3);
        player.add<Tagged>();
        pump(40);
        check(tag + "adding a tag doesn't yank the selection to the group header",
              view->currentIndex().data(rpe::PropertyPathRole).toString().endsWith(QStringLiteral("projectileSpeed")));

        if (mirrorMode)
        {
            // Pin tint survives an entity switch.
            const QString gp = rpe::PropertyModel::groupPath(QStringLiteral("Weapon"), QStringLiteral("projectileSpeed"));
            emit ed->pinRequested(gp);
            pump(5);
            browser.selectEntity(other);
            pump(40);
            browser.selectEntity(player);
            waitFor([&] { return rowFor(view, QStringLiteral("Weapon"), QStringLiteral("projectileSpeed")).isValid(); });
            pump(10);
            check(tag + "a pinned row is still tinted after switching away and back", ed->model()->isPinnedPath(gp));
        }
        else
        {
            // Direct mode follows components removed through the browser.
            int selectedSignals = 0;
            QObject::connect(&browser, &rpe::EntityComponentBrowser::componentSelected, [&] { ++selectedSignals; });
            for (int i = 0; i < comps->count(); ++i)
                if (comps->item(i)->text() == QStringLiteral("Physics"))
                    comps->setCurrentRow(i);
            pump(2);
            check(tag + "picking a component in the grouped view still emits componentSelected", selectedSignals > 0);
            emit list->removeComponentRequested(QStringLiteral("Weapon"));
            pump(10);
            check(tag + "a component removed through the browser leaves the grouped view",
                  !player.has<Weapon>() && !rowFor(view, QStringLiteral("Weapon"), QStringLiteral("projectileSpeed")).isValid());
            emit list->addComponentRequested(QStringLiteral("Weapon"));
            check(tag + "...and one added comes in", waitFor([&] { return rowFor(view, QStringLiteral("Weapon"), QStringLiteral("projectileSpeed")).isValid(); }));
        }

        // A type released and registered again: its group comes back by itself.
        rpe::TypeBridge::unregisterType<Weapon>();
        pump(20);
        rpe::TypeBridge::registerTypes<Weapon>();
        check(tag + "a released type's group returns once it's registered again",
              waitFor([&] { return rowFor(view, QStringLiteral("Weapon"), QStringLiteral("projectileSpeed")).isValid(); }));
        if (mirrorMode)
            mirror.detach();
    }

    // ── 8. Visual details ──────────────────────────────────────────────────────────
    printf("\n-- visuals --\n");
    {
        // Count the columns, along a row's middle line, painted in `c`.
        auto columnsOf = [](const QImage& img, int y, const QColor& c) {
            int n = 0;
            for (int x = 0; x < img.width(); ++x)
            {
                const QColor p = img.pixelColor(x, y);
                if (std::abs(p.red() - c.red()) < 40 && std::abs(p.green() - c.green()) < 40 && std::abs(p.blue() - c.blue()) < 40)
                    ++n;
            }
            return n;
        };

        // A selected read-only cell is painted ONCE: one selection border, not two.
        app.setStyleSheet(QStringLiteral("QTreeView::item:selected { background: rgba(0, 0, 255, 60); border: 1px solid #ff0000; }"));
        Gauge g;
        rpe::PropertyEditor ed;
        ed.editObject(g);
        ed.resize(420, 160);
        ed.show();
        const QModelIndex levelVal = topRow(ed.view(), QStringLiteral("level"), 1);
        ed.view()->setSelectionBehavior(QAbstractItemView::SelectItems);
        ed.view()->setCurrentIndex(levelVal);
        QCoreApplication::processEvents();
        {
            const QRect r = ed.view()->visualRect(levelVal);
            const QImage img = ed.view()->viewport()->grab(r).toImage();
            check("a selected read-only cell has a single selection border (2 red columns, not 4)",
                  columnsOf(img, img.height() / 2, QColor(255, 0, 0)) <= 2);
        }

        // The filter tint shows over a style sheet's ::item background.
        app.setStyleSheet(QStringLiteral("QTreeView::item { background: #202020; color: #dddddd; }"));
        ed.view()->setCurrentIndex(QModelIndex());
        ed.setFilterText(QStringLiteral("spee"));
        QCoreApplication::processEvents();
        {
            const QModelIndex speedName = topRow(ed.view(), QStringLiteral("speed"), 0);
            const QImage img = ed.view()->viewport()->grab(ed.view()->visualRect(speedName)).toImage();
            int tinted = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                {
                    const QColor p = img.pixelColor(x, y);
                    if (p.blue() > p.red() + 25)
                        ++tinted;
                }
            check("the match tint shows over a style sheet's ::item background", tinted > 20);
        }
        ed.setFilterText(QString());
        app.setStyleSheet(QString());

        // Local edit isn't offered on a read-only row.
        bool offered = true;
        QTimer::singleShot(0, [&] {
            if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
            {
                offered = false;
                for (QAction* a : m->actions())
                    if (a->text().startsWith(QStringLiteral("Local edit")))
                        offered = true;
                m->close();
            }
        });
        QMetaObject::invokeMethod(&ed, "_onContextMenu", Qt::DirectConnection, Q_ARG(QPoint, ed.view()->visualRect(levelVal).center()));
        check("\"Local edit\" isn't offered on a read-only row", !offered);

        // Toolbar: hidden Reset takes no room (the filter box shortens only when it
        // shows); toggling the scope doesn't change the filter box's width.
        ed.setSearchAllAvailable(true);
        QCoreApplication::processEvents();
        auto* filter = ed.findChild<QLineEdit*>();
        const int w0 = filter->width();
        ed.model()->beginLocalEdit(QStringLiteral("speed"));
        QCoreApplication::processEvents();
        const int w1 = filter->width();
        ed.model()->resetAll();
        QCoreApplication::processEvents();
        const int w2 = filter->width();
        ed.setSearchAll(true);
        QCoreApplication::processEvents();
        const int w3 = filter->width();
        ed.setSearchAll(false);
        check("hidden Reset leaves no gap: the filter box is shorter only while Reset shows", w1 < w0 && w2 == w0);
        check("toggling the scope doesn't change the filter box's width", w3 == w2);
        QToolButton* scope = nullptr;
        for (auto* b : ed.findChildren<QToolButton*>())
            if (b->isCheckable())
                scope = b;
        check("the scope button is no extra Tab stop", scope && scope->focusPolicy() == Qt::NoFocus);

        // Scrub: whole steps, the editor's decimals, the type's range.
        auto scrub = [&](const QString& name, int dx) {
            const QModelIndex n = topRow(ed.view(), name, 0);
            const QPoint at = ed.view()->visualRect(n).center();
            QWidget* vp = ed.view()->viewport();
            QMouseEvent press(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(vp, &press);
            const QPoint cross = at + QPoint(QApplication::startDragDistance() + 2, 0);
            QMouseEvent m1(QEvent::MouseMove, cross, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(vp, &m1);
            QMouseEvent m2(QEvent::MouseMove, cross + QPoint(dx, 0), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(vp, &m2);
            QMouseEvent rel(QEvent::MouseButtonRelease, cross + QPoint(dx, 0), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(vp, &rel);
        };
        int writes = 0;
        QObject::connect(&ed, &rpe::PropertyEditor::propertyEdited, [&] { ++writes; });
        scrub(QStringLiteral("speed"), 5); // 5 px → 1 step of 0.1
        check("scrubbing lands on whole steps at the editor's decimals (1.25 + 0.1 → 1.4, Decimals = 1)",
              std::abs(g.speed - 1.4f) < 1e-5f);
        check("...with one write per step, not one per pixel", writes == 1);
        scrub(QStringLiteral("count"), -40); // 10 steps down from 2
        check("an unsigned value stops at 0", g.count == 0);

        // Vector rows: axis chips only for axes.
        const QVector<rpe::VectorEditor::Field> rectFields { { QStringLiteral("x"), false }, { QStringLiteral("y"), false },
                                                             { QStringLiteral("w"), false }, { QStringLiteral("h"), false } };
        rpe::VectorEditor rectEd(rectFields);
        bool anyChip = false;
        for (auto* l : rectEd.findChildren<QLabel*>())
            anyChip = anyChip || !l->styleSheet().isEmpty();
        check("a Rect{x, y, w, h} gets plain name labels, not axis chips", !anyChip);
        const QVector<rpe::VectorEditor::Field> xyz { { QStringLiteral("x"), false }, { QStringLiteral("y"), false }, { QStringLiteral("z"), false } };
        rpe::VectorEditor xyzEd(xyz);
        int chips = 0;
        for (auto* l : xyzEd.findChildren<QLabel*>())
            chips += l->styleSheet().isEmpty() ? 0 : 1;
        check("x / y / z get their coloured axis chips", chips == 3);
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

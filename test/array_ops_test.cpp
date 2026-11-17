// Array operations through the whole browser — mirror and direct mode — with the
// simulation changing the arrays while you work on them:
//   • editing an element's field from the grid lands in that element;
//   • elements removed / inserted / the array cleared and refilled: the rows
//     follow, with the right values;
//   • an editor open on an element whose index disappears before it commits:
//     the edit is dropped (never written into another element or past the end),
//     and the rows catch up once the editor closes;
//   • a frozen (local edit) element under a shrink, then Reset;
//   • nested arrays and arrays of strings.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QThread>
#include <QTreeView>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

template <class T>
struct NamedValue
{
    std::string name;
    T value {};
};

struct Stats
{
    std::vector<NamedValue<float>> items { { "speed", 4.f }, { "armor", 7.f }, { "maxSpeed", 9.f } };
    std::vector<std::string> tags { "fast", "quiet" };
    std::vector<std::vector<int>> grid { { 1, 2 }, { 3 } };
};

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<NamedValue<float>>("NamedValue<float>")
        .property("name", &NamedValue<float>::name)
        .property("value", &NamedValue<float>::value);
    registration::class_<Stats>("Stats")
        .property("items", &Stats::items)
        .property("tags", &Stats::tags)
        .property("grid", &Stats::grid);
}

static int g_fails = 0;
static void check(const std::string& n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n.c_str());
    if (!ok)
        ++g_fails;
}

static void run(bool mirrorMode)
{
    const std::string tag = mirrorMode ? "mirror: " : "direct: ";
    printf("\n-- %s--\n", tag.c_str());
    flecs::world world;
    rpe::TypeBridge::registerTypes<Stats>();
    auto e = world.entity("E").set<Stats>({});

    rpe::EcsMirror mirror;
    rpe::EntityComponentBrowser browser;
    if (mirrorMode)
    {
        mirror.attach(&world);
        browser.setMirror(&mirror);
    }
    else
    {
        browser.setWorld(&world);
        browser.setEditPolicy(rpe::EditPolicy::WriteBack);
    }
    browser.resize(800, 700);
    browser.show();
    const auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(3);
        }
    };
    const auto waitFor = [&](const std::function<bool()>& cond) {
        for (int i = 0; i < 300 && !cond(); ++i)
            pump(1);
        return cond();
    };
    browser.selectEntity(e);
    auto* comps = browser.componentList()->findChild<QListWidget*>();
    waitFor([&] { return comps && comps->count() == 1; });
    comps->setCurrentRow(0);
    pump(10);

    auto* ed = browser.propertyEditor();
    auto* view = ed->view();
    auto* model = ed->model();
    const auto indexOf = [&](const QString& path, int column = 0) {
        QModelIndex found;
        std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
            for (int r = 0; r < view->model()->rowCount(parent) && !found.isValid(); ++r)
            {
                const QModelIndex i = view->model()->index(r, 0, parent);
                if (i.data(rpe::PropertyPathRole).toString() == path)
                    found = i.siblingAtColumn(column);
                walk(i);
            }
        };
        walk({});
        return found;
    };
    const auto shown = [&](const QString& path) { return indexOf(path, 1).data().toString(); };
    const auto rows = [&](const QString& path) { return view->model()->rowCount(indexOf(path)); };
    const auto expandAll = [&] {
        view->expandAll();
        pump(5);
    };

    expandAll();
    check(tag + "the array of structs lays out its elements", waitFor([&] {
              return rows(QStringLiteral("items")) == 3 && shown(QStringLiteral("items.[2].name")) == QStringLiteral("maxSpeed");
          }));

    // ── edit an element's field ─────────────────────────────────────────────────
    view->model()->setData(indexOf(QStringLiteral("items.[1].value"), 1), QVariant::fromValue(rttr::variant(70.0)), Qt::EditRole);
    check(tag + "editing an element's field writes that element",
          waitFor([&] { return e.get<Stats>().items[1].value == 70.f; }));
    check(tag + "...and no other", e.get<Stats>().items[0].value == 4.f && e.get<Stats>().items[2].value == 9.f);
    if (!mirrorMode)
        model->resetNode(QStringLiteral("items.[1].value")); // (a WriteBack edit isn't frozen; harmless)

    // ── the sim removes / inserts / clears ──────────────────────────────────────
    e.get_mut<Stats>().items.erase(e.get_mut<Stats>().items.begin()); // speed goes
    e.modified<Stats>();
    expandAll();
    check(tag + "an element erased at the front: rows shift with the right values", waitFor([&] {
              return rows(QStringLiteral("items")) == 2 && shown(QStringLiteral("items.[0].name")) == QStringLiteral("armor")
                  && shown(QStringLiteral("items.[1].name")) == QStringLiteral("maxSpeed");
          }));
    e.get_mut<Stats>().items.insert(e.get_mut<Stats>().items.begin() + 1, NamedValue<float> { "mid", 5.f });
    e.modified<Stats>();
    expandAll();
    check(tag + "an element inserted in the middle", waitFor([&] {
              return rows(QStringLiteral("items")) == 3 && shown(QStringLiteral("items.[1].name")) == QStringLiteral("mid")
                  && shown(QStringLiteral("items.[2].name")) == QStringLiteral("maxSpeed");
          }));
    e.get_mut<Stats>().items.clear();
    e.modified<Stats>();
    check(tag + "cleared: no element rows", waitFor([&] { return rows(QStringLiteral("items")) == 0; }));
    e.get_mut<Stats>().items = { { "a", 1.f }, { "b", 2.f } };
    e.modified<Stats>();
    expandAll();
    check(tag + "refilled", waitFor([&] {
              return rows(QStringLiteral("items")) == 2 && shown(QStringLiteral("items.[1].name")) == QStringLiteral("b");
          }));

    // ── an editor open on an element whose index goes away ──────────────────────
    const QModelIndex target = indexOf(QStringLiteral("items.[1].value"), 1);
    view->setCurrentIndex(target);
    view->edit(target);
    pump(3);
    auto* spin = view->viewport()->findChild<QDoubleSpinBox*>();
    check(tag + "an editor is open on items.[1].value", spin != nullptr);
    e.get_mut<Stats>().items.resize(1); // the sim drops [1] under the editor
    e.modified<Stats>();
    pump(10);
    check(tag + "the rows wait while the editor is open (no teardown under it)", rows(QStringLiteral("items")) == 2);
    if (spin)
    {
        spin->setValue(55.0);
        QAbstractItemDelegate* d = view->itemDelegateForColumn(1) ? view->itemDelegateForColumn(1) : view->itemDelegate();
        emit d->commitData(spin);
        emit d->closeEditor(spin, QAbstractItemDelegate::NoHint);
    }
    pump(10);
    check(tag + "the commit to the vanished index is dropped (nothing past the end, nothing elsewhere)",
          e.get<Stats>().items.size() == 1 && e.get<Stats>().items[0].value == 1.f && e.get<Stats>().items[0].name == "a");
    expandAll();
    check(tag + "...and the rows catch up once the editor closed", waitFor([&] { return rows(QStringLiteral("items")) == 1; }));

    // ── a frozen element under a shrink ─────────────────────────────────────────
    e.get_mut<Stats>().items = { { "a", 1.f }, { "b", 2.f }, { "c", 3.f } };
    e.modified<Stats>();
    expandAll();
    waitFor([&] { return rows(QStringLiteral("items")) == 3; });
    model->beginLocalEdit(QStringLiteral("items.[2].value"));
    e.get_mut<Stats>().items.resize(1);
    e.modified<Stats>();
    pump(15);
    check(tag + "a frozen element keeps its row while frozen", rows(QStringLiteral("items")) == 3);
    model->resetNode(QStringLiteral("items.[2].value"));
    expandAll();
    check(tag + "Reset lets the array shrink", waitFor([&] { return rows(QStringLiteral("items")) == 1; }));

    // ── the same, released by Reset All (two frozen elements in two arrays) ──────
    e.get_mut<Stats>().items = { { "a", 1.f }, { "b", 2.f }, { "c", 3.f } };
    e.get_mut<Stats>().tags = { "x", "y", "z" };
    e.modified<Stats>();
    expandAll();
    waitFor([&] { return rows(QStringLiteral("items")) == 3 && rows(QStringLiteral("tags")) == 3; });
    model->beginLocalEdit(QStringLiteral("items.[2].name"));
    model->beginLocalEdit(QStringLiteral("tags.[2]"));
    e.get_mut<Stats>().items.resize(2);
    e.get_mut<Stats>().tags = { "fast", "quiet" };
    e.modified<Stats>();
    pump(15);
    check(tag + "two frozen elements hold both arrays", rows(QStringLiteral("items")) == 3 && rows(QStringLiteral("tags")) == 3);
    model->resetAll();
    expandAll();
    check(tag + "Reset All lets both shrink", waitFor([&] {
              return rows(QStringLiteral("items")) == 2 && rows(QStringLiteral("tags")) == 2
                  && shown(QStringLiteral("tags.[1]")) == QStringLiteral("quiet");
          }));

    // ── arrays of strings, nested arrays ────────────────────────────────────────
    expandAll();
    view->model()->setData(indexOf(QStringLiteral("tags.[1]"), 1), QVariant::fromValue(rttr::variant(std::string("loud"))), Qt::EditRole);
    check(tag + "an element of an array of strings edits", waitFor([&] { return e.get<Stats>().tags[1] == "loud"; }));
    view->model()->setData(indexOf(QStringLiteral("grid.[1].[0]"), 1), QVariant::fromValue(rttr::variant(30)), Qt::EditRole);
    check(tag + "an element of a nested array edits", waitFor([&] { return e.get<Stats>().grid[1][0] == 30; }));
    e.get_mut<Stats>().grid[0].push_back(9);
    e.get_mut<Stats>().grid.push_back({ 7, 8 });
    e.modified<Stats>();
    expandAll();
    check(tag + "nested arrays grow at both levels", waitFor([&] {
              return rows(QStringLiteral("grid")) == 3 && rows(QStringLiteral("grid.[0]")) == 3
                  && shown(QStringLiteral("grid.[2].[1]")) == QStringLiteral("8");
          }));
    e.get_mut<Stats>().grid.erase(e.get_mut<Stats>().grid.begin());
    e.modified<Stats>();
    expandAll();
    check(tag + "...and shrink, inner rows following their new parents", waitFor([&] {
              return rows(QStringLiteral("grid")) == 2 && rows(QStringLiteral("grid.[0]")) == 1
                  && shown(QStringLiteral("grid.[0].[0]")) == QStringLiteral("30") && rows(QStringLiteral("grid.[1]")) == 2;
          }));

    if (mirrorMode)
        mirror.detach();
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    run(true);
    run(false);
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

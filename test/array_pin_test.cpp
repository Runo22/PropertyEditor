// Pins on array elements, map entries and whole arrays / structs, through the
// mirror, while the arrays change underneath them:
//   • an index past the end (the array shrank) or a key that's gone is reported
//     as MISSING once — the watch shows "—" with no editor — and the value comes
//     back when the path resolves again; only a removed component / destroyed
//     entity drops the pin;
//   • an index pin follows the slot, not the element (a reorder shows another
//     element) — by design for plain arrays;
//   • a whole array / struct / element can be pinned: a read-only summary that
//     follows the content;
//   • edits through element pins land in the right element; an edit to a path
//     that's gone is dropped;
//   • the grid offers "Pin to watch list" on array and struct rows too.
#include <rpe/core/TypeBridge.h>
#include <rpe/core/TypeRenderer.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/PinnedPropertiesWidget.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QListWidget>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>

#include <cstdio>
#include <functional>
#include <map>
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
    std::vector<float> nums { 1.f, 2.f, 3.f };
    std::vector<NamedValue<float>> items { { "speed", 4.f }, { "armor", 7.f }, { "maxSpeed", 9.f } };
    std::map<std::string, int> counts { { "a", 1 }, { "b", 2 } };
    std::vector<std::vector<int>> grid { { 1, 2 }, { 3 } };
    std::vector<int> big = std::vector<int>(100, 5);
};
struct Health
{
    int hp = 100;
};

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<NamedValue<float>>("NamedValue<float>")
        .property("name", &NamedValue<float>::name)
        .property("value", &NamedValue<float>::value);
    registration::class_<Stats>("Stats")
        .property("nums", &Stats::nums)
        .property("items", &Stats::items)
        .property("counts", &Stats::counts)
        .property("grid", &Stats::grid)
        .property("big", &Stats::big);
    registration::class_<Health>("Health").property("hp", &Health::hp);
}

static int g_fails = 0;
static void check(const std::string& n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n.c_str());
    if (!ok)
        ++g_fails;
}

using PinKey = rpe::MirrorChannel::PinKey;

// ── 1. The producer ─────────────────────────────────────────────────────────────
static void testProducer()
{
    printf("\n-- producer --\n");
    flecs::world world;
    rpe::TypeBridge::registerTypes<Stats, Health>();
    auto e = world.entity("E").set<Stats>({}).set<Health>({});
    const qulonglong eid = e.id();

    rpe::EcsMirror mirror;
    mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);
    mirror.setScanIntervalsMs(0, 0);
    auto ch = mirror.channel();
    mirror.setInterest(eid, "Stats", {});

    // path → shown text ("MISSING" for a missing report); dead paths.
    QMap<QString, QString> got;
    QStringList dead;
    int published = 0;
    const auto pump = [&] {
        world.progress(0.016f);
        mirror.pump();
        for (const auto& v : mirror.pollPinValues())
        {
            got.insert(v.key.path, v.missing ? QStringLiteral("MISSING") : rpe::TypeRenderer::summaryString(v.value));
            ++published;
        }
        for (const auto& d : ch->pollDeadPins())
            dead << d.path;
    };
    const auto key = [&](const char* comp, const char* path) {
        PinKey k;
        k.entity = eid;
        k.component = QString::fromLatin1(comp);
        k.path = QString::fromLatin1(path);
        return k;
    };
    const QVector<PinKey> pins { key("Stats", "nums.[1]"), key("Stats", "items.[2].value"), key("Stats", "counts.[b]"),
                                 key("Stats", "grid.[1].[0]"), key("Stats", "nums"), key("Stats", "items.[0]"),
                                 key("Stats", "big"), key("Health", "hp") };
    mirror.setPins(pins);
    pump();
    check("element, struct field, map entry and nested element pins all show",
          got.value("nums.[1]") == "2" && got.value("items.[2].value") == "9" && got.value("counts.[b]") == "2"
              && got.value("grid.[1].[0]") == "3");
    check("a whole array shows its elements", got.value("nums") == "[1, 2, 3]");
    check("a whole element shows its fields", got.value("items.[0]") == "{speed, 4}");
    check("a long array is cut, saying how many are left",
          got.value("big").startsWith("[5, 5") && got.value("big").contains(QStringLiteral("… +")));

    // Edits through element pins.
    mirror.queuePinEdit(key("Stats", "nums.[1]"), rttr::variant(20.f));
    mirror.queuePinEdit(key("Stats", "items.[2].value"), rttr::variant(90.f));
    mirror.queuePinEdit(key("Stats", "counts.[b]"), rttr::variant(22));
    mirror.queuePinEdit(key("Stats", "grid.[1].[0]"), rttr::variant(33));
    pump();
    const Stats& s = e.get<Stats>();
    check("element pin edits land in the right elements",
          s.nums[1] == 20.f && s.nums[0] == 1.f && s.items[2].value == 90.f && s.items[0].value == 4.f
              && s.counts.at("b") == 22 && s.counts.at("a") == 1 && s.grid[1][0] == 33);
    check("...and the whole-array pin follows the content", got.value("nums") == "[1, 20, 3]");
    check("...the whole-element pin too (another element changed: not this one)", got.value("items.[0]") == "{speed, 4}");

    // Reorder: an index pin follows the slot.
    std::swap(e.get_mut<Stats>().items[0], e.get_mut<Stats>().items[2]);
    pump();
    check("after a reorder an index pin shows the element now at that index",
          got.value("items.[2].value") == "4" && got.value("items.[0]") == "{maxSpeed, 90}");

    // Shrink below the pinned indices: MISSING, once.
    e.get_mut<Stats>().nums.resize(1);
    e.get_mut<Stats>().items.resize(1);
    e.get_mut<Stats>().grid.resize(1);
    e.get_mut<Stats>().counts.erase("b");
    published = 0;
    pump();
    check("an index past the end is reported missing",
          got.value("nums.[1]") == "MISSING" && got.value("items.[2].value") == "MISSING" && got.value("grid.[1].[0]") == "MISSING");
    check("a map key that's gone is reported missing", got.value("counts.[b]") == "MISSING");
    check("...the whole array shows what's left", got.value("nums") == "[1]");
    check("...nothing is dead: the component is still there", dead.isEmpty());
    published = 0;
    pump();
    pump();
    check("missing is reported once, not every pump", published == 0);

    // An edit to a path that's gone is dropped (no crash, nothing grows).
    mirror.queuePinEdit(key("Stats", "nums.[1]"), rttr::variant(5.f));
    mirror.queuePinEdit(key("Stats", "counts.[b]"), rttr::variant(5));
    pump();
    check("an edit to a missing path changes nothing",
          e.get<Stats>().nums.size() == 1 && e.get<Stats>().counts.count("b") == 0);

    // Grow back: the values return.
    e.get_mut<Stats>().nums = { 7.f, 8.f };
    e.get_mut<Stats>().items.push_back({ "a", 1.f });
    e.get_mut<Stats>().items.push_back({ "b", 2.f });
    e.get_mut<Stats>().grid.push_back({ 42 });
    e.get_mut<Stats>().counts["b"] = 5;
    pump();
    check("the values come back when the paths resolve again",
          got.value("nums.[1]") == "8" && got.value("items.[2].value") == "2" && got.value("grid.[1].[0]") == "42"
              && got.value("counts.[b]") == "5");

    // Emptied completely, then refilled.
    e.get_mut<Stats>().nums.clear();
    pump();
    check("an emptied array: element missing, whole array []", got.value("nums.[1]") == "MISSING" && got.value("nums") == "[]");
    e.get_mut<Stats>().nums = { 0.f, 1.5f };
    pump();
    check("...and refilled", got.value("nums.[1]") == "1.5");

    // Only a removed component / destroyed entity drops pins.
    e.remove<Health>();
    pump();
    check("removing the component reports its pin dead", dead.contains("hp"));
    check("...not the Stats pins", !dead.contains("nums.[1]") && !dead.contains("nums"));
    e.destruct();
    pump();
    check("destroying the entity reports the rest dead", dead.contains("nums.[1]") && dead.contains("big"));

    mirror.detach();
}

// ── 2. The watch list ───────────────────────────────────────────────────────────
static void testWidget()
{
    printf("\n-- watch list --\n");
    flecs::world world;
    rpe::TypeBridge::registerTypes<Stats, Health>();
    auto e = world.entity("E").set<Stats>({});
    const qulonglong eid = e.id();

    rpe::EcsMirror mirror;
    mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);
    mirror.setScanIntervalsMs(0, 0);
    rpe::PinnedPropertiesWidget w;
    w.setChannel(mirror.channel());
    w.show();
    auto* tree = w.findChild<QTreeWidget*>();
    const auto pump = [&] {
        world.progress(0.016f);
        mirror.pump();
        w.pollNow();
        QCoreApplication::processEvents();
    };
    const auto row = [&](const char* path) -> QTreeWidgetItem* {
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(1).endsWith(QString::fromLatin1(path)))
                return tree->topLevelItem(i);
        return nullptr;
    };
    const auto text = [&](const char* path) {
        QTreeWidgetItem* it = row(path);
        return it ? it->text(2) : QStringLiteral("<no row>");
    };
    // Does the value cell open an editor? (Asks the delegate directly.)
    const auto editable = [&](const char* path) {
        QTreeWidgetItem* it = row(path);
        if (!it)
            return false;
        const QModelIndex idx = tree->model()->index(tree->indexOfTopLevelItem(it), 2);
        QStyleOptionViewItem opt;
        QAbstractItemDelegate* d = tree->itemDelegateForColumn(2);
        QWidget* ed = d ? d->createEditor(tree->viewport(), opt, idx) : nullptr;
        if (!ed)
            return false;
        emit d->closeEditor(ed);
        delete ed;
        return true;
    };

    w.pin(eid, "E", "Stats", "nums.[1]");
    w.pin(eid, "E", "Stats", "counts.[b]");
    w.pin(eid, "E", "Stats", "nums");
    w.pin(eid, "E", "Stats", "items");
    w.pin(eid, "E", "Stats", "items.[0]");
    pump();
    check("element pins show their values", text("nums.[1]") == "2" && text("counts.[b]") == "2");
    check("...and edit", editable("nums.[1]") && editable("counts.[b]"));
    check("a whole array shows a summary", text("Stats.nums") == "[1, 2, 3]");
    check("an array of structs too", text("Stats.items") == "[{speed, 4}, {armor, 7}, {maxSpeed, 9}]");
    check("a whole element", text("items.[0]") == "{speed, 4}");
    check("whole values never open an editor (known up front)", !editable("Stats.nums") && !editable("Stats.items"));
    check("...nor when learnt from the value (an element's path isn't walked up front)", !editable("items.[0]"));
    check("...and say why", row("Stats.nums") && row("Stats.nums")->toolTip(2).contains(QStringLiteral("whole array"))
                                && row("items.[0]") && row("items.[0]")->toolTip(2).contains(QStringLiteral("whole struct")));

    e.get_mut<Stats>().nums.resize(1);
    e.get_mut<Stats>().counts.erase("b");
    pump();
    check("a missing element shows —", text("nums.[1]") == QStringLiteral("—") && text("counts.[b]") == QStringLiteral("—"));
    check("...with no editor", !editable("nums.[1]") && !editable("counts.[b]"));
    check("...and says why", row("nums.[1]") && row("nums.[1]")->toolTip(2).contains(QStringLiteral("Not there")));
    check("...the row stays", tree->topLevelItemCount() == 5);
    check("the whole array follows", text("Stats.nums") == "[1]");

    e.get_mut<Stats>().nums = { 4.f, 5.f, 6.f };
    e.get_mut<Stats>().counts["b"] = 9;
    pump();
    check("back: the value shows again", text("nums.[1]") == "5" && text("counts.[b]") == "9");
    check("...and edits again", editable("nums.[1]"));
    check("...its tooltip is back to normal", row("nums.[1]") && row("nums.[1]")->toolTip(2).isEmpty());
    check("the whole array follows", text("Stats.nums") == "[4, 5, 6]");

    // Rapid churn between pumps: whatever the last state is, it's what shows.
    for (int i = 0; i < 20; ++i)
    {
        e.get_mut<Stats>().nums.resize(static_cast<size_t>(i % 3));
        pump();
    }
    e.get_mut<Stats>().nums = { 1.f, 2.f };
    pump();
    check("after churn the pins settle on the final state", text("nums.[1]") == "2" && text("Stats.nums") == "[1, 2]");

    mirror.detach();
}

// ── 3. From the grid ────────────────────────────────────────────────────────────
static void testGridMenu()
{
    printf("\n-- grid menu --\n");
    flecs::world world;
    rpe::TypeBridge::registerTypes<Stats, Health>();
    auto e = world.entity("E").set<Stats>({});

    rpe::EcsMirror mirror;
    mirror.attach(&world);
    rpe::EntityComponentBrowser browser;
    rpe::PinnedPropertiesWidget pins;
    browser.setMirror(&mirror);
    browser.setPinnedPropertiesWidget(&pins);
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
    browser.selectEntity(e);
    pump(30);
    auto* comps = browser.componentList()->findChild<QListWidget*>();
    if (comps && comps->count())
        comps->setCurrentRow(0);
    pump(20);

    auto* ed = browser.propertyEditor();
    auto* view = ed->view();
    const auto indexOf = [&](const QString& path) {
        QModelIndex found;
        std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
            for (int r = 0; r < view->model()->rowCount(parent) && !found.isValid(); ++r)
            {
                const QModelIndex i = view->model()->index(r, 0, parent);
                if (i.data(rpe::PropertyPathRole).toString() == path)
                    found = i;
                walk(i);
            }
        };
        walk({});
        return found;
    };
    const auto menuFor = [&](const QString& path) {
        QStringList labels;
        const QModelIndex idx = indexOf(path);
        if (!idx.isValid())
            return labels;
        view->scrollTo(idx);
        const QPoint pos = view->visualRect(idx).center();
        QTimer::singleShot(0, [&labels] {
            if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
            {
                for (QAction* a : m->actions())
                    labels << a->text();
                for (QAction* a : m->actions())
                    if (a->text() == QStringLiteral("Pin to watch list"))
                        a->trigger();
                m->close();
            }
        });
        QMetaObject::invokeMethod(ed, "_onContextMenu", Qt::DirectConnection, Q_ARG(QPoint, pos));
        return labels;
    };

    check("the array row offers Pin", menuFor(QStringLiteral("nums")).contains(QStringLiteral("Pin to watch list")));
    pump(20);
    auto* pinTree = pins.findChild<QTreeWidget*>();
    const auto pinText = [&](const char* suffix) {
        for (int i = 0; pinTree && i < pinTree->topLevelItemCount(); ++i)
            if (pinTree->topLevelItem(i)->text(1).endsWith(QString::fromLatin1(suffix)))
                return pinTree->topLevelItem(i)->text(2);
        return QStringLiteral("<no row>");
    };
    check("...and pinning it watches the whole array", pinText("Stats.nums") == "[1, 2, 3]");

    view->expand(indexOf(QStringLiteral("items")));
    pump(20);
    check("an element (struct) row offers Pin", menuFor(QStringLiteral("items.[1]")).contains(QStringLiteral("Pin to watch list")));
    pump(20);
    check("...and its pin shows the element", pinText("items.[1]") == "{armor, 7}");

    e.get_mut<Stats>().items.erase(e.get_mut<Stats>().items.begin());
    pump(20);
    check("the element pin follows its index after an erase", pinText("items.[1]") == "{maxSpeed, 9}");
    e.get_mut<Stats>().items.resize(1);
    pump(20);
    check("...and shows — when the index is gone", pinText("items.[1]") == QStringLiteral("—"));

    mirror.detach();
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testProducer();
    testWidget();
    testGridMenu();

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

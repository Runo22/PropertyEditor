// Search looks INSIDE containers: the elements of an array of structs, a map's
// keys and its struct values, nested arrays — by field name and by value — with
// the limits holding. A matching container is one hit (its value, so the view
// lays its elements out) that counts its matching rows.
//
// End to end, in mirror and direct mode: the component list's badges, the
// grouped view's rows, editing an element in place, and the array growing and
// shrinking while the search is on.
#include <rpe/core/PropertySearch.h>
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QThread>
#include <QToolButton>
#include <QTreeView>

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

struct Inventory
{
    std::vector<NamedValue<float>> stats { { "speed", 4.f }, { "armor", 7.f }, { "maxSpeed", 9.f } };
    std::vector<float> weights { 1.5f, 2.f };
    std::map<std::string, NamedValue<int>> gear { { "head", { "helmet", 3 } }, { "feet", { "boots", 1 } } };
    std::map<std::string, float> resist { { "fire", 0.5f }, { "ice", 0.25f }, { "x7", 7.f } };
    std::vector<std::vector<NamedValue<float>>> nested { { { "deepSpeed", 1.f } } };
    std::vector<std::string> tags { "fast", "quiet" };
    std::vector<NamedValue<float>> empty;
};
struct Plain
{
    float speed = 2.f;
};

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<NamedValue<float>>("NamedValue<float>")
        .property("name", &NamedValue<float>::name)
        .property("value", &NamedValue<float>::value);
    registration::class_<NamedValue<int>>("NamedValue<int>")
        .property("name", &NamedValue<int>::name)
        .property("value", &NamedValue<int>::value);
    registration::class_<Inventory>("Inventory")
        .property("stats", &Inventory::stats)
        .property("weights", &Inventory::weights)
        .property("gear", &Inventory::gear)
        .property("resist", &Inventory::resist)
        .property("nested", &Inventory::nested)
        .property("tags", &Inventory::tags)
        .property("empty", &Inventory::empty);
    registration::class_<Plain>("Plain").property("speed", &Plain::speed);
}

static int g_fails = 0;
static void check(const std::string& n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n.c_str());
    if (!ok)
        ++g_fails;
}

// path → matches
static QMap<QString, int> search(const Inventory& inv, const char* needle, rpe::PropertySearchLimits limits = {})
{
    std::vector<rpe::PropertyHit> hits;
    rpe::searchProperties(rttr::instance(inv), rttr::type::get<Inventory>(), QString::fromUtf8(needle), hits, limits);
    QMap<QString, int> out;
    for (const auto& h : hits)
        out.insert(h.path, h.matches);
    return out;
}

static void testCore()
{
    printf("\n-- core --\n");
    const Inventory inv;
    using M = QMap<QString, int>;

    check("a field VALUE inside an array of structs: both elements count",
          search(inv, "speed") == (M { { "stats", 2 }, { "nested", 1 } }));
    check("...the hit carries the whole array (for the view to lay out)", [&] {
        std::vector<rpe::PropertyHit> hits;
        rpe::searchProperties(rttr::instance(inv), rttr::type::get<Inventory>(), QStringLiteral("armor"), hits);
        return hits.size() == 1 && hits[0].value.is_sequential_container()
            && hits[0].value.create_sequential_view().get_size() == 3;
    }());
    check("a number inside an element", search(inv, "9") == (M { { "stats", 1 } }));
    check("a field NAME inside elements counts every element",
          search(inv, "name") == (M { { "stats", 3 }, { "gear", 2 }, { "nested", 1 } }));
    check("a map's struct value is searched (helmet)", search(inv, "helmet") == (M { { "gear", 1 } }));
    check("a map's key is a row of its own (head)", search(inv, "head") == (M { { "gear", 1 } }));
    check("a map of numbers: by key", search(inv, "fire") == (M { { "resist", 1 } }));
    check("a map of numbers: by value", search(inv, "0.25") == (M { { "resist", 1 } }));
    check("a key and its value both matching are still ONE row", search(inv, "7").value("resist") == 1); // [x7] = 7
    check("an array of strings", search(inv, "quiet") == (M { { "tags", 1 } }));
    check("an array of numbers", search(inv, "1.5") == (M { { "weights", 1 } }));
    check("the container's own name still matches (once)", search(inv, "weights") == (M { { "weights", 1 } }));
    check("nothing for text that occurs nowhere", search(inv, "xyz").isEmpty());

    // Limits.
    Inventory big;
    big.stats.assign(300, { "x", 0.f });
    big.stats.back().name = "needle";
    rpe::PropertySearchLimits small;
    small.maxElements = 256;
    check("an element past maxElements isn't looked at", !search(big, "needle", small).contains(QStringLiteral("stats")));
    small.maxElements = 400;
    check("...raising the limit finds it", search(big, "needle", small).value("stats") == 1);
    rpe::PropertySearchLimits shallow;
    shallow.maxDepth = 1;
    const M sh = search(inv, "speed", shallow);
    check("maxDepth 1: an array's struct fields are searched, an array inside an array isn't",
          sh.value("stats") == 2 && !sh.contains(QStringLiteral("nested")));
    rpe::PropertySearchLimits few;
    few.maxRows = 3;
    check("maxRows stops a search early without crashing", search(big, "needle", few).isEmpty());
}

// ── the browser ─────────────────────────────────────────────────────────────────
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
                out << (g + QLatin1Char('/') + rel);
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
            rpe::PropertyModel::splitGroupPath(i.data(rpe::PropertyPathRole).toString(), &key, &p);
            if (!i.data(rpe::IsGroupRole).toBool() && p == rel && key == group)
                found = i;
            walk(i);
        }
    };
    walk({});
    return found;
}

static void runBrowser(bool mirrorMode)
{
    const std::string tag = mirrorMode ? "mirror: " : "direct: ";
    printf("\n-- %s--\n", tag.c_str());
    flecs::world world;
    rpe::TypeBridge::registerTypes<Inventory, Plain>();
    auto hero = world.entity("Hero");
    hero.set<Inventory>({}).set<Plain>({});

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
    auto* filter = ed->findChild<QLineEdit*>();
    QToolButton* scope = nullptr;
    for (auto* b : ed->findChildren<QToolButton*>())
        if (b->isCheckable())
            scope = b;

    browser.selectEntity(hero);
    waitFor([&] { return comps->count() == 2; });
    for (int i = 0; i < comps->count(); ++i)
        if (comps->item(i)->text() == QStringLiteral("Plain"))
            comps->setCurrentRow(i);
    pump(5);

    filter->setText(QStringLiteral("speed"));
    check(tag + "badges count the rows inside arrays: Inventory 3, Plain 1", waitFor([&] {
              return list->matchCountsActive() && list->matchCount(QStringLiteral("Inventory")) == 3
                  && list->matchCount(QStringLiteral("Plain")) == 1;
          }));

    scope->click();
    check(tag + "the grouped view shows the matching elements", waitFor([&] {
              const QStringList rows = visibleRows(view);
              return rows.contains(QStringLiteral("Inventory/stats.[0].name")) && rows.contains(QStringLiteral("Inventory/stats.[2].name"))
                  && rows.contains(QStringLiteral("Inventory/nested.[0].[0].name")) && rows.contains(QStringLiteral("Plain/speed"));
          }));
    const auto onScreen = [&](const QString& group, const QString& rel) {
        const QModelIndex i = rowFor(view, group, rel);
        if (!i.isValid())
            return false;
        for (QModelIndex p = i.parent(); p.isValid(); p = p.parent())
            if (!view->isExpanded(p))
                return false;
        return true;
    };
    check(tag + "...shown OPEN on screen: rows that arrived after the filter text are expanded too", waitFor([&] {
              return onScreen(QStringLiteral("Inventory"), QStringLiteral("stats.[0].name"))
                  && onScreen(QStringLiteral("Inventory"), QStringLiteral("nested.[0].[0].name"));
          }));
    check(tag + "a matching field brings the rest of its element along",
          waitFor([&] { return visibleRows(view).contains(QStringLiteral("Inventory/stats.[0].value")); }));
    check(tag + "...not the other elements'", !visibleRows(view).contains(QStringLiteral("Inventory/stats.[1].value")));
    check(tag + "...and only them", !visibleRows(view).contains(QStringLiteral("Inventory/stats.[1].name"))
                                        && visibleRows(view).filter(QStringLiteral("weights")).isEmpty());
    check(tag + "...with their values", rowFor(view, QStringLiteral("Inventory"), QStringLiteral("stats.[2].name"))
                                            .siblingAtColumn(1).data().toString() == QStringLiteral("maxSpeed"));
    {
        QModelIndex header;
        for (int r = 0; r < view->model()->rowCount(); ++r)
            if (view->model()->index(r, 0).data(Qt::DisplayRole).toString() == QStringLiteral("Inventory"))
                header = view->model()->index(r, 0);
        check(tag + "the header counts the same rows (3)", waitFor([&] { return header.data(rpe::GroupCountRole).toInt() == 3; }));
    }

    // Edit an element's field in place.
    filter->setText(QStringLiteral("armor"));
    check(tag + "another needle: the other element", waitFor([&] {
              return visibleRows(view).contains(QStringLiteral("Inventory/stats.[1].name"));
          }));
    const QModelIndex armor = rowFor(view, QStringLiteral("Inventory"), QStringLiteral("stats.[1].name")).siblingAtColumn(1);
    view->model()->setData(armor, QVariant::fromValue(rttr::variant(std::string("armorPlus"))), Qt::EditRole);
    check(tag + "editing an element's field in the grouped view writes that element",
          waitFor([&] { return hero.get<Inventory>().stats[1].name == "armorPlus"; }));
    check(tag + "...and nothing else", hero.get<Inventory>().stats[0].name == "speed" && hero.get<Inventory>().stats[2].name == "maxSpeed");

    // The array changes while the search is on.
    filter->setText(QStringLiteral("speed"));
    waitFor([&] { return visibleRows(view).contains(QStringLiteral("Inventory/stats.[2].name")); });
    hero.get_mut<Inventory>().stats.pop_back(); // maxSpeed goes
    hero.modified<Inventory>();
    check(tag + "an element removed from the array leaves the view", waitFor([&] {
              const QStringList rows = visibleRows(view);
              return !rows.contains(QStringLiteral("Inventory/stats.[2].name")) && rows.contains(QStringLiteral("Inventory/stats.[0].name"));
          }));
    check(tag + "...and the counts follow (2)", waitFor([&] { return list->matchCount(QStringLiteral("Inventory")) == 2; }));
    hero.get_mut<Inventory>().stats.push_back({ "turboSpeed", 1.f });
    hero.get_mut<Inventory>().stats.insert(hero.get_mut<Inventory>().stats.begin(), NamedValue<float> { "warpSpeed", 5.f });
    hero.modified<Inventory>();
    check(tag + "elements added (front and back) show up at their new indices", waitFor([&] {
              const QStringList rows = visibleRows(view);
              return rows.contains(QStringLiteral("Inventory/stats.[0].name")) && rows.contains(QStringLiteral("Inventory/stats.[1].name"))
                  && rows.contains(QStringLiteral("Inventory/stats.[3].name"));
          }));
    check(tag + "...with the right names", waitFor([&] {
              return rowFor(view, QStringLiteral("Inventory"), QStringLiteral("stats.[0].name")).siblingAtColumn(1).data().toString() == QStringLiteral("warpSpeed")
                  && rowFor(view, QStringLiteral("Inventory"), QStringLiteral("stats.[3].name")).siblingAtColumn(1).data().toString() == QStringLiteral("turboSpeed");
          }));
    hero.get_mut<Inventory>().stats.clear();
    hero.modified<Inventory>();
    check(tag + "an emptied array leaves no rows behind", waitFor([&] {
              return visibleRows(view).filter(QStringLiteral("Inventory/stats")).isEmpty();
          }));

    // An array matching by NAME shows its elements — plain values and structs.
    filter->setText(QStringLiteral("weights"));
    check(tag + "an array of numbers matching by name shows its elements", waitFor([&] {
              const QStringList rows = visibleRows(view);
              return rows.contains(QStringLiteral("Inventory/weights.[0]")) && rows.contains(QStringLiteral("Inventory/weights.[1]"));
          }));
    filter->setText(QStringLiteral("stats"));
    check(tag + "an array of structs matching by name shows every element, whole", waitFor([&] {
              const QStringList rows = visibleRows(view);
              return rows.contains(QStringLiteral("Inventory/stats.[0].name")) && rows.contains(QStringLiteral("Inventory/stats.[1].value"));
          }));

    // The single-component view: the array's elements come in after the filter
    // text too (mirror mode reads them lazily) and open the same way.
    scope->click(); // scope off: back to one component
    hero.get_mut<Inventory>().stats = { { "speed", 1.f }, { "x", 2.f } };
    hero.modified<Inventory>();
    for (int i = 0; i < comps->count(); ++i)
        if (comps->item(i)->text() == QStringLiteral("Inventory"))
            comps->setCurrentRow(i);
    filter->setText(QStringLiteral("weights"));
    check(tag + "single component: an array matching by name shows its elements", waitFor([&] {
              return rowFor(view, QString(), QStringLiteral("weights.[1]")).isValid();
          }));
    filter->setText(QStringLiteral("spee"));
    check(tag + "single component: a matching field's element shows whole (its value too)", waitFor([&] {
              return rowFor(view, QString(), QStringLiteral("stats.[0].value")).isValid()
                  && !rowFor(view, QString(), QStringLiteral("stats.[1].value")).isValid();
          }));
    check(tag + "single component: matching array elements are shown open", waitFor([&] {
              const QModelIndex i = rowFor(view, QString(), QStringLiteral("stats.[0].name"));
              if (!i.isValid())
                  return false;
              for (QModelIndex p = i.parent(); p.isValid(); p = p.parent())
                  if (!view->isExpanded(p))
                      return false;
              return true;
          }));
    filter->clear();
    pump(5);
    if (mirrorMode)
        mirror.detach();
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testCore();
    runBrowser(true);
    runBrowser(false);

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

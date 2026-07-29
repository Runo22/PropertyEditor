// Regression: a per-leaf mirror update to a field INSIDE an array/map element
// (e.g. "items.[0].v") must reach its node. Array/assoc element sub-fields were
// built but never registered in _nodeByPath, so setPropertyValue() for such a
// path silently dropped — the row only refreshed on a full-container resend
// (re-selecting the component), which read as "vector components don't update".
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QCoreApplication>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
    struct Item
    {
        int v = 0;
    };
    struct Bag
    {
        std::vector<Item> items;
        const std::vector<Item>& getItems() const { return items; } // getter → const vector&
    };
    struct MapBag
    {
        std::map<std::string, Item> items;
    };
} // namespace

RTTR_REGISTRATION
{
    rttr::registration::class_<Item>("Item").property("v", &Item::v);
    rttr::registration::class_<Bag>("Bag").property_readonly("items", &Bag::getItems);
    rttr::registration::class_<MapBag>("MapBag").property("items", &MapBag::items);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

// The DisplayRole value cell of the row at `path` (walks the model tree).
static QString valueAt(rpe::PropertyModel& m, const QString& path)
{
    QString found;
    std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int r = 0; r < m.rowCount(parent); ++r)
        {
            const QModelIndex idx = m.index(r, 0, parent);
            if (idx.data(rpe::PropertyPathRole).toString() == path)
                found = m.index(r, 1, parent).data(Qt::DisplayRole).toString();
            walk(idx);
        }
    };
    walk(QModelIndex());
    return found;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto flush = [] { QCoreApplication::processEvents(); };

    // ── vector<struct>: build elements, then a per-leaf update must land ───────
    {
        rpe::PropertyModel model;
        model.bindType(rttr::type::get<Bag>());

        Bag bag;
        bag.items = { { 10 }, { 20 } };
        model.setPropertyValue(QStringLiteral("items"), rttr::variant(bag.getItems()));
        flush();

        check("element sub-field registered as a leaf",
              model.allLeafPaths().contains(QStringLiteral("items.[0].v")));
        check("element value populated from the vector", valueAt(model, QStringLiteral("items.[0].v")) == QStringLiteral("10"));

        // The mirror's per-leaf update for an in-place element change.
        model.setPropertyValue(QStringLiteral("items.[0].v"), rttr::variant(777));
        flush();
        check("per-leaf update reaches the nested array element (was dropped)",
              valueAt(model, QStringLiteral("items.[0].v")) == QStringLiteral("777"));

        // A resize still works and leaves no stale path behind.
        bag.items = { { 1 } };
        model.setPropertyValue(QStringLiteral("items"), rttr::variant(bag.getItems()));
        flush();
        check("resize drops the removed element's sub-path",
              !model.allLeafPaths().contains(QStringLiteral("items.[1].v")));
        check("surviving element still updates per-leaf after a resize",
              (model.setPropertyValue(QStringLiteral("items.[0].v"), rttr::variant(9)), flush(),
               valueAt(model, QStringLiteral("items.[0].v")) == QStringLiteral("9")));
    }

    // ── map<key, struct>: same for associative element sub-fields ─────────────
    {
        rpe::PropertyModel model;
        model.bindType(rttr::type::get<MapBag>());
        MapBag mb;
        mb.items = { { "a", { 5 } }, { "b", { 6 } } };
        model.setPropertyValue(QStringLiteral("items"), rttr::variant(mb.items));
        flush();
        check("map element sub-field registered as a leaf",
              model.allLeafPaths().contains(QStringLiteral("items.[a].v")));
        model.setPropertyValue(QStringLiteral("items.[a].v"), rttr::variant(555));
        flush();
        check("per-leaf update reaches the nested map element",
              valueAt(model, QStringLiteral("items.[a].v")) == QStringLiteral("555"));
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

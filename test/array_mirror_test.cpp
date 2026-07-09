// Arrays must display in MIRROR mode. Mirror updates arrive through
// setPropertyValue() (not refresh()), so the model has to build an array's
// element rows from the injected sequential value — otherwise the array shows
// blank with no children. Exercised straight through PropertyModel.
#include <rpe/core/TypeBridge.h>
#include <rpe/core/TypeRenderer.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>

#include <cstdio>
#include <vector>

struct Vec2
{
    double x = 0, y = 0;
};
struct Comp
{
    std::vector<int> nums = { 1 };
    std::vector<Vec2> points;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Vec2>("Vec2").property("x", &Vec2::x).property("y", &Vec2::y);
    rttr::registration::class_<Comp>("Comp").property("nums", &Comp::nums).property("points", &Comp::points);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

// Find the top-level row whose path == name.
static QModelIndex topIndex(rpe::PropertyModel& m, const QString& name)
{
    for (int r = 0; r < m.rowCount({}); ++r)
    {
        const QModelIndex idx = m.index(r, 0, {});
        if (idx.data(rpe::PropertyPathRole).toString() == name)
            return idx;
    }
    return {};
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerTypes<Vec2, Comp>();

    rpe::PropertyModel model;
    model.bindType(rttr::type::get<Comp>());

    const QModelIndex numsIdx = topIndex(model, QStringLiteral("nums"));
    check("array node exists in the schema", numsIdx.isValid());
    check("array children are lazy (none before any value)", model.rowCount(numsIdx) == 0);

    // ── Inject the whole vector, the way the mirror feeds it ──────────────────
    {
        std::vector<int> v { 10, 5, 0 };
        model.setPropertyValue(QStringLiteral("nums"), rttr::variant(v));
        QCoreApplication::processEvents(); // run the queued flush

        const QModelIndex idx = topIndex(model, QStringLiteral("nums"));
        check("injected array builds element rows (3)", model.rowCount(idx) == 3);
        check("array parent shows its count [3]", idx.siblingAtColumn(1).data(Qt::DisplayRole).toString() == QStringLiteral("[3]"));
        const QString e0 = model.index(0, 1, idx).data(Qt::DisplayRole).toString();
        const QString e1 = model.index(1, 1, idx).data(Qt::DisplayRole).toString();
        check("element [0] displays its value (10)", e0 == QStringLiteral("10"));
        check("element [1] displays its value (5)", e1 == QStringLiteral("5"));
    }

    // ── Resize down, then up — rows must track the size ───────────────────────
    {
        std::vector<int> v { 7 };
        model.setPropertyValue(QStringLiteral("nums"), rttr::variant(v));
        QCoreApplication::processEvents();
        check("shrinking the vector rebuilds rows (1)", model.rowCount(topIndex(model, QStringLiteral("nums"))) == 1);

        std::vector<int> w { 1, 2, 3, 4 };
        model.setPropertyValue(QStringLiteral("nums"), rttr::variant(w));
        QCoreApplication::processEvents();
        check("growing the vector rebuilds rows (4)", model.rowCount(topIndex(model, QStringLiteral("nums"))) == 4);
    }

    // ── Vector of structs: element rows are themselves expandable ─────────────
    {
        std::vector<Vec2> pts { { 1.0, 2.0 }, { 3.0, 4.0 } };
        model.setPropertyValue(QStringLiteral("points"), rttr::variant(pts));
        QCoreApplication::processEvents();

        const QModelIndex pIdx = topIndex(model, QStringLiteral("points"));
        check("struct-vector builds element rows (2)", model.rowCount(pIdx) == 2);
        const QModelIndex elem0 = model.index(0, 0, pIdx);
        check("struct element expands into its fields (x,y)", model.rowCount(elem0) == 2);
        const QString x = model.index(0, 1, elem0).data(Qt::DisplayRole).toString();
        check("nested field points.[0].x displays (1)", x == QStringLiteral("1"));
    }

    // ── A resize is deferred while an element has an open editor (no teardown) ──
    // overrideNode() is exactly the pin createEditor applies; the destructive
    // rebuild must not free a pinned element node out from under a live editor.
    {
        std::vector<int> base { 1, 2, 3 };
        model.setPropertyValue(QStringLiteral("nums"), rttr::variant(base));
        QCoreApplication::processEvents();
        check("array has 3 rows before pin", model.rowCount(topIndex(model, QStringLiteral("nums"))) == 3);

        model.overrideNode(QStringLiteral("nums.[1]")); // an editor is open on element [1]
        std::vector<int> grown { 1, 2, 3, 4, 5 };
        model.setPropertyValue(QStringLiteral("nums"), rttr::variant(grown));
        QCoreApplication::processEvents();
        check("resize deferred while a child is pinned (still 3 rows)", model.rowCount(topIndex(model, QStringLiteral("nums"))) == 3);

        model.resetNode(QStringLiteral("nums.[1]")); // editor closed → pin released
        model.setPropertyValue(QStringLiteral("nums"), rttr::variant(grown));
        QCoreApplication::processEvents();
        check("resize applies once the pin is released (5 rows)", model.rowCount(topIndex(model, QStringLiteral("nums"))) == 5);
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

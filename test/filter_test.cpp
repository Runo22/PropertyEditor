// Filtering: the component list has a filter box, and the property grid's filter
// matches a row's VALUE (including a struct's "[a, b]" summary) as well as its name.
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/gui/PropertyEditor.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QTreeView>

#include <cstdio>
#include <functional>

namespace
{
    struct Vec2
    {
        int x = 1, y = 2;
    };
    struct Thing
    {
        double mass = 7.5;
        bool active = true;
        int hp = 100;
        Vec2 dir; // summary "[1, 2]"
    };
} // namespace

RTTR_REGISTRATION
{
    rttr::registration::class_<Vec2>("Vec2").property("x", &Vec2::x).property("y", &Vec2::y);
    rttr::registration::class_<Thing>("Thing")
        .property("mass", &Thing::mass)
        .property("active", &Thing::active)
        .property("hp", &Thing::hp)
        .property("dir", &Thing::dir);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

// Names of the rows the property view currently shows (walks the proxy tree).
static QStringList visibleRows(QTreeView* view)
{
    QStringList out;
    auto* m = view->model();
    std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int r = 0; r < m->rowCount(parent); ++r)
        {
            const QModelIndex idx = m->index(r, 0, parent);
            out << idx.data(Qt::DisplayRole).toString();
            walk(idx);
        }
    };
    walk(QModelIndex());
    return out;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // ── Component list filter ──────────────────────────────────────────────────
    {
        rpe::ComponentListWidget comp;
        comp.setComponentNames({ QStringLiteral("game::Transform"), QStringLiteral("physics::Collider"),
                                 QStringLiteral("Health") });
        auto* lw = comp.findChild<QListWidget*>();
        auto* filter = comp.findChild<QLineEdit*>();
        check("component list has a filter box", filter != nullptr);

        auto hiddenOf = [&](const QString& leaf) {
            for (int i = 0; i < lw->count(); ++i)
                if (lw->item(i)->text() == leaf)
                    return lw->item(i)->isHidden();
            return true;
        };

        filter->setText(QStringLiteral("col")); // matches "Collider"
        check("filter shows the matching component", !hiddenOf(QStringLiteral("Collider")));
        check("filter hides non-matching components",
              hiddenOf(QStringLiteral("Health")) && hiddenOf(QStringLiteral("Transform")));

        filter->clear();
        check("clearing the filter shows all again",
              !hiddenOf(QStringLiteral("Collider")) && !hiddenOf(QStringLiteral("Health")));

        // The filter re-applies over a rebuild (new rows).
        filter->setText(QStringLiteral("health"));
        comp.setComponentNames({ QStringLiteral("Health"), QStringLiteral("Armor") });
        check("filter re-applied after a rebuild", !hiddenOf(QStringLiteral("Health")) && hiddenOf(QStringLiteral("Armor")));
    }

    // ── Property grid: filter by VALUE, not just name ──────────────────────────
    {
        Thing thing;
        rpe::PropertyEditor editor;
        editor.editObject(thing);
        editor.resize(360, 300);
        editor.show();
        QApplication::processEvents();
        auto* view = editor.view();
        auto* filter = editor.findChild<QLineEdit*>();
        check("property editor has a filter box", filter != nullptr);

        // Filter by a leaf VALUE ("7.5" = mass) — name doesn't contain it.
        filter->setText(QStringLiteral("7.5"));
        QApplication::processEvents();
        const QStringList byValue = visibleRows(view);
        check("value filter shows the row whose value matches (mass)", byValue.contains(QStringLiteral("mass")));
        check("value filter hides rows whose value/name don't match",
              !byValue.contains(QStringLiteral("active")) && !byValue.contains(QStringLiteral("hp")));

        // Filter by a struct SUMMARY value "[1, 2]" — matches "dir" (and shows its
        // ancestor path). The summary is matchable even though filtering expands all.
        filter->setText(QStringLiteral("[1, 2]"));
        QApplication::processEvents();
        check("filter matches a struct's summary value", visibleRows(view).contains(QStringLiteral("dir")));

        // A bool value: "true" matches the 'active' row.
        filter->setText(QStringLiteral("true"));
        QApplication::processEvents();
        check("value filter matches a bool value (active = true)", visibleRows(view).contains(QStringLiteral("active")));

        // Name still works.
        filter->setText(QStringLiteral("hp"));
        QApplication::processEvents();
        check("name filter still works", visibleRows(view).contains(QStringLiteral("hp")));

        filter->clear();
        QApplication::processEvents();
        check("clearing restores all rows", visibleRows(view).contains(QStringLiteral("mass"))
                  && visibleRows(view).contains(QStringLiteral("active")));
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

// Entity/component lists: alphabetical sort, auto-select of the top row, and the
// selection signal. Exercised directly through the list widgets.
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EntityListWidget.h>

#include <QApplication>
#include <QListWidget>

#include <cstdio>

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static QStringList rowsOf(QWidget* w)
{
    QStringList out;
    if (auto* lw = w->findChild<QListWidget*>())
        for (int i = 0; i < lw->count(); ++i)
            out << lw->item(i)->text();
    return out;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // ── Entities: sorted alphabetically + top row auto-selected + signal ───────
    {
        rpe::EntityListWidget list;
        int selCount = 0;
        qulonglong lastSel = 0;
        QObject::connect(&list, &rpe::EntityListWidget::entityIdSelected, &list, [&](qulonglong id) {
            ++selCount;
            lastSel = id;
        });

        // Fed out of order (id order != alpha order).
        QVector<QPair<qulonglong, QString>> entries {
            { 30, QStringLiteral("Zebra") }, { 10, QStringLiteral("apple") }, { 20, QStringLiteral("Mango") }
        };
        list.setEntries(entries);

        const QStringList rows = rowsOf(&list);
        check("entities sorted alphabetically (case-insensitive)",
              rows == QStringList({ QStringLiteral("apple"), QStringLiteral("Mango"), QStringLiteral("Zebra") }));

        auto* lw = list.findChild<QListWidget*>();
        check("top entity is auto-selected", lw && lw->currentRow() == 0);
        check("entityIdSelected fired once for the top entity",
              selCount == 1 && lastSel == 10ull); // "apple"

        // An empty feed clears the selection.
        list.setEntries({});
        check("empty feed leaves no selection", lw && lw->currentRow() == -1);
    }

    // ── Components: sorted alphabetically by displayed leaf name ───────────────
    {
        rpe::ComponentListWidget comp;
        comp.setComponentNames({ QStringLiteral("game::Transform"), QStringLiteral("physics::Collider"), QStringLiteral("Health") });
        check("components sorted alphabetically by leaf",
              rowsOf(&comp) == QStringList({ QStringLiteral("Collider"), QStringLiteral("Health"), QStringLiteral("Transform") }));
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

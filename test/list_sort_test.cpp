// Entity/component lists: alphabetical sort, auto-select of the top row, and the
// selection signal. Exercised directly through the list widgets.
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EntityListWidget.h>

#include <QApplication>
#include <QListWidget>
#include <QListWidgetItem>

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

    // ── Programmatic selection: immediate + pending (entity not yet loaded) ────
    {
        rpe::EntityListWidget list;
        int selCount = 0;
        qulonglong lastSel = 0;
        QObject::connect(&list, &rpe::EntityListWidget::entityIdSelected, &list, [&](qulonglong id) {
            ++selCount;
            lastSel = id;
        });

        list.setEntries({ { 10, QStringLiteral("apple") }, { 20, QStringLiteral("Mango") } });
        auto* lw = list.findChild<QListWidget*>();

        // Select a present entity by id.
        check("selectById(present) returns true", list.selectById(20) == true);
        check("selectById selected the right row + emitted", lw->currentItem()->data(Qt::UserRole).toULongLong() == 20ull && lastSel == 20ull);

        // Select an id that isn't in the list yet → deferred.
        check("selectById(absent) returns false (pending)", list.selectById(30) == false);
        check("selection unchanged while the request is pending", lw->currentItem()->data(Qt::UserRole).toULongLong() == 20ull);

        // When the requested entity arrives, the pending request is applied.
        list.setEntries({ { 10, QStringLiteral("apple") }, { 20, QStringLiteral("Mango") }, { 30, QStringLiteral("Zebra") } });
        check("pending request applied once the entity appears",
              lw->currentItem()->data(Qt::UserRole).toULongLong() == 30ull && lastSel == 30ull);

        // DIFF update, not a rebuild: adding one entity must leave the existing
        // rows' QListWidgetItem OBJECTS untouched (a clear+rebuild would recreate
        // them — the very churn that stalled big lists once per scan cycle).
        QListWidgetItem* mangoBefore = nullptr;
        for (int i = 0; i < lw->count(); ++i)
            if (lw->item(i)->text() == QStringLiteral("Mango"))
                mangoBefore = lw->item(i);
        list.setEntries({ { 10, QStringLiteral("apple") }, { 15, QStringLiteral("Kiwi") },
                          { 20, QStringLiteral("Mango") }, { 30, QStringLiteral("Zebra") } });
        QListWidgetItem* mangoAfter = nullptr;
        for (int i = 0; i < lw->count(); ++i)
            if (lw->item(i)->text() == QStringLiteral("Mango"))
                mangoAfter = lw->item(i);
        check("diff update keeps existing row objects (no rebuild)",
              lw->count() == 4 && mangoAfter != nullptr && mangoAfter == mangoBefore);
        check("diff update preserved the selection", lw->currentItem()->data(Qt::UserRole).toULongLong() == 30ull);
    }

    // ── Components: sorted alphabetically by displayed leaf name ───────────────
    {
        rpe::ComponentListWidget comp;
        comp.setComponentNames({ QStringLiteral("game::Transform"), QStringLiteral("physics::Collider"), QStringLiteral("Health") });
        check("components sorted alphabetically by leaf",
              rowsOf(&comp) == QStringList({ QStringLiteral("Collider"), QStringLiteral("Health"), QStringLiteral("Transform") }));

        // Duplicate leaves must order deterministically by full name (std::sort is
        // not stable), so the list never reshuffles between refreshes.
        comp.setComponentNames({ QStringLiteral("render::Collider"), QStringLiteral("physics::Collider"), QStringLiteral("Health") });
        auto* lw = comp.findChild<QListWidget*>();
        QStringList paths;
        for (int i = 0; i < lw->count(); ++i)
            paths << lw->item(i)->data(Qt::UserRole).toString();
        check("duplicate leaves ordered by full name (physics < render)",
              paths == QStringList({ QStringLiteral("physics::Collider"), QStringLiteral("render::Collider"), QStringLiteral("Health") }));
    }

    // ── After deleting the SELECTED component, select its neighbour (not the top) ──
    {
        using Row = rpe::MirrorChannel::ComponentRow;
        using Kind = rpe::MirrorChannel::RowKind;
        const auto row = [](const char* n) { return Row { QString::fromUtf8(n), QString(), Kind::Data, 0 }; };

        rpe::ComponentListWidget comp;
        QString sel;
        QObject::connect(&comp, &rpe::ComponentListWidget::componentNameSelected, &comp,
                         [&](const QString& n) { sel = n; });

        // Rows sort alphabetically: Aa, Bb, Cc, Dd.
        comp.setComponentRows({ row("Aa"), row("Bb"), row("Cc"), row("Dd") });
        auto* lw = comp.findChild<QListWidget*>();

        // Select the 3rd (Cc), then "delete" it → the neighbour that shifts up (Dd)
        // is selected, NOT the first row (Aa).
        lw->setCurrentRow(2);
        check("Cc selected", sel == QStringLiteral("Cc"));
        comp.setComponentRows({ row("Aa"), row("Bb"), row("Dd") });
        check("deleting the selected row selects the next neighbour (Dd), not the top",
              comp.currentComponentName() == QStringLiteral("Dd") && sel == QStringLiteral("Dd"));

        // Delete the now-last selected (Dd) → falls back to the previous one (Bb).
        comp.setComponentRows({ row("Aa"), row("Bb") });
        check("deleting the last selected row selects the previous (Bb)",
              comp.currentComponentName() == QStringLiteral("Bb") && sel == QStringLiteral("Bb"));

        // Deleting a NON-selected row keeps the current selection put.
        comp.setComponentRows({ row("Aa"), row("Bb"), row("Ee") });
        check("Bb still selected after adding a row", comp.currentComponentName() == QStringLiteral("Bb"));
        comp.setComponentRows({ row("Bb"), row("Ee") }); // drop Aa (not selected)
        check("deleting a non-selected row keeps the selection", comp.currentComponentName() == QStringLiteral("Bb"));
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

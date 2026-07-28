// The entity-list "Add" (spawn) picker: prefabs are grouped by their group tag,
// group headers carry the host's optional icon, and picking a prefab emits
// spawnPrefabRequested with that prefab's id.
#include <rpe/ecs/EntityListWidget.h>

#include <QApplication>
#include <QFrame>
#include <QPixmap>
#include <QToolButton>
#include <QTreeWidget>

#include <cstdio>

using Prefab = rpe::MirrorChannel::PrefabEntry;

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static QTreeWidgetItem* groupNamed(QTreeWidget* t, const QString& name)
{
    for (int i = 0; i < t->topLevelItemCount(); ++i)
        if (t->topLevelItem(i)->text(0) == name)
            return t->topLevelItem(i);
    return nullptr;
}

static QTreeWidgetItem* leafNamed(QTreeWidget* t, const QString& name)
{
    for (int i = 0; i < t->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* g = t->topLevelItem(i);
        for (int j = 0; j < g->childCount(); ++j)
            if (g->child(j)->text(0) == name)
                return g->child(j);
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::EntityListWidget w;
    w.setEntityAddingEnabled(true);
    w.setAddablePrefabs(QVector<Prefab> {
        { 10, QStringLiteral("Goblin"), QStringLiteral("Enemy") },
        { 11, QStringLiteral("Orc"), QStringLiteral("Enemy") },
        { 12, QStringLiteral("Chest"), QStringLiteral("Prop") },
    });
    QPixmap px(8, 8);
    px.fill(Qt::red);
    QIcon enemyIcon(px);
    w.setPrefabGroupIcons({ { QStringLiteral("Enemy"), enemyIcon } });

    qulonglong spawned = 0;
    QObject::connect(&w, &rpe::EntityListWidget::spawnPrefabRequested, &w, [&](qulonglong id) { spawned = id; });

    // The Add button is visible when adding is enabled; clicking opens the picker.
    auto* btn = w.findChild<QToolButton*>();
    check("Add button present and shown (not hidden)", btn && !btn->isHidden());
    btn->click();

    auto* popup = w.findChild<QFrame*>(QStringLiteral("rpeAddPopup"));
    check("picker popup opened", popup != nullptr);
    auto* tree = popup ? popup->findChild<QTreeWidget*>() : nullptr;
    check("picker has a tree", tree != nullptr);

    if (tree)
    {
        check("prefabs grouped: Enemy + Prop headers",
              groupNamed(tree, QStringLiteral("Enemy")) && groupNamed(tree, QStringLiteral("Prop")));
        check("Enemy group has Goblin + Orc",
              leafNamed(tree, QStringLiteral("Goblin")) && leafNamed(tree, QStringLiteral("Orc")));
        check("Prop group has Chest", leafNamed(tree, QStringLiteral("Chest")) != nullptr);

        auto* enemy = groupNamed(tree, QStringLiteral("Enemy"));
        check("Enemy group header shows the host icon", enemy && !enemy->icon(0).isNull());
        auto* prop = groupNamed(tree, QStringLiteral("Prop"));
        check("Prop group (no icon supplied) has none", prop && prop->icon(0).isNull());

        // Pick "Orc" → spawnPrefabRequested(11).
        QTreeWidgetItem* orc = leafNamed(tree, QStringLiteral("Orc"));
        check("Orc leaf carries its prefab id", orc && orc->data(0, Qt::UserRole).toULongLong() == 11ull);
        QMetaObject::invokeMethod(tree, "itemActivated", Q_ARG(QTreeWidgetItem*, orc), Q_ARG(int, 0));
        check("picking a prefab emits spawnPrefabRequested with its id", spawned == 11ull);
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

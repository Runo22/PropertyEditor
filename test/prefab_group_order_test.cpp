// Reported: the prefab picker stopped grouping — the tags are in the world, but
// every PrefabEntry arrives with an EMPTY group.
//
// Cause: the group TAG NAMES only reach the producer through the mirror channel,
// so configuring them before setMirror() silently dropped them (the icons, being
// GUI-side, were kept — which is why only the grouping disappeared). Nothing
// re-sent them when the channel later appeared.
//
// Pins down: both call orders group, and both ways of naming a tag resolve — its
// full world path, and a short alias registered with world.use().
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
#include <QIcon>
#include <QPixmap>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>

#include <cstdio>

struct Position
{
    double x = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Position>("Position").property("x", &Position::x);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

// A world whose group tag is SCOPED and also carries a short world.use() alias —
// the configuration the report came from.
struct Fixture
{
    flecs::world world;
    flecs::entity tag;

    Fixture()
    {
        tag = world.entity("game::npc::Enemy");
        world.use(tag, "Enemy");
        world.entity("Goblin").add(flecs::Prefab).add(tag).set<Position>({ 1 });
        world.entity("Hero").set<Position>({ 2 });
    }
};

// Drive the real chain (browser → channel → producer → picker) and report the
// group header the picker ended up with, or "<flat>" when nothing grouped.
static QString groupHeaderFor(const char* tagName, bool groupsBeforeMirror)
{
    Fixture fx;
    rpe::EcsMirror mirror;
    mirror.attach(&fx.world);
    mirror.setScanIntervalsMs(0, 0);

    rpe::EntityComponentBrowser browser;
    browser.setEntityAddingEnabled(true);

    QPixmap px(8, 8);
    px.fill(Qt::red);
    const QVector<rpe::EntityComponentBrowser::PrefabGroup> groups {
        { QString::fromUtf8(tagName), QIcon(px) }
    };

    if (groupsBeforeMirror)
    {
        browser.setPrefabGroups(groups);
        browser.setMirror(&mirror);
    }
    else
    {
        browser.setMirror(&mirror);
        browser.setPrefabGroups(groups);
    }
    browser.show();
    for (int i = 0; i < 40; ++i)
    {
        fx.world.progress(0.016f);
        QCoreApplication::processEvents();
        QThread::msleep(4);
    }

    browser.entityList()->findChild<QToolButton*>()->click();
    QCoreApplication::processEvents();
    auto* popup = browser.entityList()->findChild<QFrame*>(QStringLiteral("rpeAddPopup"));
    auto* tree = popup ? popup->findChild<QTreeWidget*>() : nullptr;

    QString header = QStringLiteral("<no popup>");
    if (tree && tree->topLevelItemCount() > 0)
    {
        QTreeWidgetItem* top = tree->topLevelItem(0);
        // A grouped picker puts prefabs UNDER a header; a flat one lists them at
        // top level — which is exactly what an empty group produces.
        header = top->childCount() > 0 ? top->text(0) : QStringLiteral("<flat>");
        if (top->childCount() > 0)
        {
            check("  …the group header keeps the host's icon", !top->icon(0).isNull());
        }
    }
    if (popup)
    {
        popup->close();
    }
    QCoreApplication::processEvents();
    mirror.detach();
    return header;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    rpe::TypeBridge::registerType<Position>();

    // Precondition: flecs resolves both spellings to the same tag — so a failure
    // below is rpe's plumbing, not the lookup.
    {
        Fixture fx;
        check("the scoped tag resolves by its full path",
              fx.world.lookup("game::npc::Enemy") == fx.tag);
        check("…and by its world.use() alias", fx.world.lookup("Enemy") == fx.tag);
        // Documented limit, so nobody re-reports it as this bug: world.lookup()'s
        // separator is "::" — a dotted spelling is NOT found.
        check("a DOTTED spelling does not resolve (lookup separator is \"::\")",
              !fx.world.lookup("game.npc.Enemy").is_valid());
    }

    // The header reads as the tag's leaf either way (display shortening).
    const QString leaf = QStringLiteral("Enemy");

    check("full path, groups set AFTER setMirror",
          groupHeaderFor("game::npc::Enemy", false) == leaf);
    check("alias, groups set AFTER setMirror",
          groupHeaderFor("Enemy", false) == leaf);

    // The regression: configuring the browser BEFORE handing it the mirror.
    check("full path, groups set BEFORE setMirror",
          groupHeaderFor("game::npc::Enemy", true) == leaf);
    check("alias, groups set BEFORE setMirror",
          groupHeaderFor("Enemy", true) == leaf);

    // ── A tag the world doesn't know must SAY so, once ─────────────────────────
    // This is what made the bug expensive: every prefab just came back ungrouped,
    // which reads as "grouping is broken" rather than "that name is wrong".
    {
        static int warnings = 0;
        static QString lastMsg;
        QtMessageHandler prev = qInstallMessageHandler(
            [](QtMsgType t, const QMessageLogContext&, const QString& msg) {
                if (t == QtWarningMsg && msg.contains(QStringLiteral("prefab group tag")))
                {
                    ++warnings;
                    lastMsg = msg;
                }
            });

        // The dotted spelling — resolvable-looking, but lookup() never finds it.
        const QString header = groupHeaderFor("game.npc.Enemy", false);
        qInstallMessageHandler(prev);

        check("an unresolvable group tag leaves the picker flat (as before)",
              header == QStringLiteral("<flat>"));
        check("...but now warns about it", warnings >= 1);
        check("...exactly once, not once per scan", warnings == 1);
        check("...naming the tag it could not find",
              lastMsg.contains(QStringLiteral("game.npc.Enemy")));
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

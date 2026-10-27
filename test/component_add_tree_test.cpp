// The "Add component" picker as a namespace TREE: every scope segment is a level
// ("plugins.output.hud.Speed" → plugins ▸ output ▸ hud ▸ Speed), tags sit under a
// "Tags" root (last), and top-level namespaces listed in
// Settings::hiddenAddNamespaces — "settings" by default — are not offered at all.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>

#include <cstdio>
#include <functional>

using Catalog = rpe::MirrorChannel::CatalogEntry;

struct Health
{
    int hp = 0;
};
struct Graphics
{
    int quality = 0;
};
struct Audio
{
    int volume = 0;
};
struct Mana
{
    int mp = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
    rttr::registration::class_<Graphics>("Graphics").property("quality", &Graphics::quality);
    rttr::registration::class_<Audio>("Audio").property("volume", &Audio::volume);
    rttr::registration::class_<Mana>("Mana").property("mp", &Mana::mp);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static QTreeWidgetItem* child(QTreeWidgetItem* parent, const QString& text)
{
    if (!parent)
        return nullptr;
    for (int i = 0; i < parent->childCount(); ++i)
        if (parent->child(i)->text(0) == text)
            return parent->child(i);
    return nullptr;
}

static QTreeWidgetItem* top(QTreeWidget* t, const QString& text)
{
    return t ? child(t->invisibleRootItem(), text) : nullptr;
}

// Every option (payload-carrying row) in the tree, visible or not.
static QStringList allOptions(QTreeWidget* t)
{
    QStringList out;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* it) {
        for (int i = 0; i < it->childCount(); ++i)
        {
            QTreeWidgetItem* c = it->child(i);
            if (!c->data(0, Qt::UserRole).isNull())
                out << c->data(0, Qt::UserRole).toString();
            walk(c);
        }
    };
    if (t)
        walk(t->invisibleRootItem());
    return out;
}

static void testTreeShape()
{
    printf("\n-- namespace tree --\n");
    rpe::ComponentListWidget w;
    w.setComponentEditingEnabled(true);
    w.setAddableEntries(QVector<Catalog> {
        { QStringLiteral("game::Health"), false },
        { QStringLiteral("plugins.output.hud.Fuel"), false },
        { QStringLiteral("plugins.output.hud.Speed"), false },
        { QStringLiteral("plugins.output.ig.Radar"), false },
        { QStringLiteral("render\\.Stats"), false }, // flecs-escaped dot: ONE segment
        { QStringLiteral("Loose"), false },
        { QStringLiteral("Marker"), true },
        { QStringLiteral("plugins.output.hud.Visible"), true },
    });
    QString added;
    QObject::connect(&w, &rpe::ComponentListWidget::addComponentRequested, &w,
                     [&](const QString& n) { added = n; });

    w.findChild<QToolButton*>()->click();
    QCoreApplication::processEvents();
    auto* popup = w.findChild<QFrame*>(QStringLiteral("rpeAddPopup"));
    auto* tree = popup ? popup->findChild<QTreeWidget*>() : nullptr;
    auto* search = popup ? popup->findChild<QLineEdit*>() : nullptr;
    check("picker opened", tree && search);
    if (!tree)
        return;

    QTreeWidgetItem* hud = child(child(top(tree, QStringLiteral("plugins")), QStringLiteral("output")), QStringLiteral("hud"));
    QTreeWidgetItem* ig = child(child(top(tree, QStringLiteral("plugins")), QStringLiteral("output")), QStringLiteral("ig"));
    check("plugins ▸ output ▸ hud is a nested branch", hud != nullptr);
    check("plugins ▸ output ▸ ig is its sibling under the SAME output node", ig != nullptr);
    check("hud holds Speed and Fuel (by leaf)",
          child(hud, QStringLiteral("Speed")) && child(hud, QStringLiteral("Fuel")));
    check("ig holds Radar", child(ig, QStringLiteral("Radar")) != nullptr);
    check("a leaf carries its FULL path for the add request",
          child(hud, QStringLiteral("Speed"))
              && child(hud, QStringLiteral("Speed"))->data(0, Qt::UserRole).toString()
                  == QStringLiteral("plugins.output.hud.Speed"));
    check("\"::\" scopes nest too (game ▸ Health)", child(top(tree, QStringLiteral("game")), QStringLiteral("Health")));
    check("an escaped dot is part of the name, not a level",
          child(top(tree, QStringLiteral("(global)")), QStringLiteral("render.Stats")) != nullptr);
    check("unscoped data sits under (global)", child(top(tree, QStringLiteral("(global)")), QStringLiteral("Loose")));

    QTreeWidgetItem* tags = top(tree, QStringLiteral("Tags"));
    check("tags live under a root labelled \"Tags\"", tags != nullptr);
    check("the old \"(tags)\" label is gone", top(tree, QStringLiteral("(tags)")) == nullptr);
    check("Tags is the LAST root", tags && tree->indexOfTopLevelItem(tags) == tree->topLevelItemCount() - 1);
    check("a scoped tag nests beneath Tags the same way",
          child(child(child(child(tags, QStringLiteral("plugins")), QStringLiteral("output")), QStringLiteral("hud")),
                QStringLiteral("Visible")));
    check("an unscoped tag sits directly under Tags", child(tags, QStringLiteral("Marker")) != nullptr);
    check("group headers are not options (nothing but the 8 entries is pickable)", allOptions(tree).size() == 8);

    // Filter by a NAMESPACE narrows to that branch (data and tags alike).
    search->setText(QStringLiteral("hud"));
    QCoreApplication::processEvents();
    check("typing a namespace keeps its branch", !hud->isHidden() && !child(hud, QStringLiteral("Speed"))->isHidden());
    check("...and hides the others", ig->isHidden() && top(tree, QStringLiteral("game"))->isHidden());
    check("...tags in that namespace stay too", !tags->isHidden());

    // Keyboard still walks the nested options.
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QCoreApplication::sendEvent(search, &down);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(search, &enter);
    QCoreApplication::processEvents();
    check("arrow + Enter picks a nested option by full path", added == QStringLiteral("plugins.output.hud.Speed"));
}

// The browser drops top-level "settings" namespaces from the picker by default.
static void testHiddenNamespaces()
{
    printf("\n-- hidden namespaces --\n");
    flecs::world world;
    world.component<Health>("game::Health");
    world.component<Graphics>("settings::Graphics");
    world.component<Audio>("Settings::Audio"); // different case: hidden too
    world.component<Mana>("game::Mana");
    rpe::TypeBridge::registerType<Health>();
    rpe::TypeBridge::registerType<Graphics>();
    rpe::TypeBridge::registerType<Audio>();
    rpe::TypeBridge::registerType<Mana>();
    world.entity("Hero").set<Health>({ 1 });

    rpe::EcsMirror mirror;
    mirror.attach(&world);
    mirror.setScanIntervalsMs(0, 0);
    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    browser.setComponentEditingEnabled(true);
    browser.show();

    auto pump = [&] {
        for (int i = 0; i < 30; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(3);
        }
    };
    auto offered = [&] {
        browser.componentList()->findChild<QToolButton*>()->click();
        QCoreApplication::processEvents();
        auto* popup = browser.componentList()->findChild<QFrame*>(QStringLiteral("rpeAddPopup"));
        const QStringList out = allOptions(popup ? popup->findChild<QTreeWidget*>() : nullptr);
        if (popup)
            popup->close();
        // WA_DeleteOnClose defers the delete; flush it, or the NEXT findChild would
        // still find this (stale) popup instead of the freshly opened one.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return out;
    };
    auto has = [](const QStringList& l, const char* leaf) {
        for (const QString& s : l)
            if (s.endsWith(QString::fromLatin1(leaf)))
                return true;
        return false;
    };

    pump();
    auto* el = browser.entityList()->findChild<QListWidget*>();
    if (el && el->count())
        el->setCurrentRow(0);
    pump();

    check("default Settings hide the \"settings\" namespace",
          browser.settings().hiddenAddNamespaces == QStringList { QStringLiteral("settings") });
    QStringList o = offered();
    check("settings::Graphics is not offered", !has(o, "Graphics"));
    check("the match is case-insensitive (Settings::Audio hidden too)", !has(o, "Audio"));
    check("other namespaces are unaffected (game::Mana offered)", has(o, "Mana"));

    browser.setHiddenAddNamespaces({});
    pump();
    o = offered();
    check("an empty list offers settings components again", has(o, "Graphics") && has(o, "Audio"));

    auto s = browser.settings();
    s.hiddenAddNamespaces = QStringList { QStringLiteral("SETTINGS") };
    browser.setSettings(s);
    pump();
    o = offered();
    check("setSettings applies the list immediately", !has(o, "Graphics") && !has(o, "Audio"));

    mirror.detach();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    testTreeShape();
    testHiddenNamespaces();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

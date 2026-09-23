// Keyboard driving of the browser's search-then-pick surfaces.
//
//   • the "Add" pickers (spawnable prefabs, addable components): you type, the
//     best match is already highlighted, arrows/Tab walk the matches, Enter takes
//     the highlighted one — a bare Enter takes the topmost valid option;
//   • the panel filters (entities, components): arrows walk the visible rows
//     straight from the filter box, Enter hands focus to the list, Esc clears.
//
// Both pickers must behave identically — that parity is what this pins down.
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>

#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QToolButton>
#include <QTreeWidget>

#include <cstdio>

using Prefab = rpe::MirrorChannel::PrefabEntry;
using Catalog = rpe::MirrorChannel::CatalogEntry;

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static void key(QWidget* w, int k, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent ev(QEvent::KeyPress, k, mods);
    QCoreApplication::sendEvent(w, &ev);
    QCoreApplication::processEvents();
}

// The picker's search box and tree, from whichever popup the widget just opened.
struct Picker
{
    QFrame* popup = nullptr;
    QLineEdit* search = nullptr;
    QTreeWidget* tree = nullptr;

    QString current() const
    {
        QTreeWidgetItem* it = tree ? tree->currentItem() : nullptr;
        return it ? it->text(0) : QStringLiteral("<none>");
    }
};

static Picker openPicker(QWidget* owner)
{
    owner->findChild<QToolButton*>()->click();
    QCoreApplication::processEvents();
    Picker p;
    p.popup = owner->findChild<QFrame*>(QStringLiteral("rpeAddPopup"));
    p.search = p.popup ? p.popup->findChild<QLineEdit*>() : nullptr;
    p.tree = p.popup ? p.popup->findChild<QTreeWidget*>() : nullptr;
    return p;
}

// ── The prefab picker (entity list) ──────────────────────────────────────────
static void testPrefabPicker()
{
    printf("\n-- prefab picker --\n");
    rpe::EntityListWidget w;
    w.setEntityAddingEnabled(true);
    // Deliberately two groups, so the walk has to cross a group header.
    w.setAddablePrefabs(QVector<Prefab> {
        { 10, QStringLiteral("Goblin"), QStringLiteral("Enemy") },
        { 11, QStringLiteral("Orc"), QStringLiteral("Enemy") },
        { 12, QStringLiteral("Chest"), QStringLiteral("Prop") },
        { 13, QStringLiteral("Crate"), QStringLiteral("Prop") },
    });
    qulonglong spawned = 0;
    QObject::connect(&w, &rpe::EntityListWidget::spawnPrefabRequested, &w, [&](qulonglong id) { spawned = id; });

    {
        Picker p = openPicker(&w);
        check("picker opens with a search box and a tree", p.search && p.tree);
        check("filter box has focus", p.popup && p.popup->focusWidget() == p.search);
        check("the topmost option is highlighted before any typing",
              p.current() == QStringLiteral("Goblin"));

        key(p.search, Qt::Key_Down);
        check("Down walks to the next option", p.current() == QStringLiteral("Orc"));
        key(p.search, Qt::Key_Down);
        check("Down crosses a group header (Orc → Chest)", p.current() == QStringLiteral("Chest"));
        key(p.search, Qt::Key_Up);
        check("Up walks back", p.current() == QStringLiteral("Orc"));

        // Arrows clamp at the ends — they never wrap or fall off the list.
        for (int i = 0; i < 8; ++i)
            key(p.search, Qt::Key_Up);
        check("Up clamps at the first option", p.current() == QStringLiteral("Goblin"));
        for (int i = 0; i < 8; ++i)
            key(p.search, Qt::Key_Down);
        check("Down clamps at the last option", p.current() == QStringLiteral("Crate"));

        // Tab cycles instead (there is nowhere else in a popup for it to go).
        key(p.search, Qt::Key_Tab);
        check("Tab wraps past the last option", p.current() == QStringLiteral("Goblin"));
        key(p.search, Qt::Key_Backtab);
        check("Shift+Tab wraps back", p.current() == QStringLiteral("Crate"));

        check("focus never left the filter box", p.popup->focusWidget() == p.search);

        key(p.search, Qt::Key_Return);
        check("Enter spawns the highlighted prefab", spawned == 13ull);
        check("...and closes the picker", QApplication::activePopupWidget() == nullptr);
    }

    // Typing re-highlights the best match, and a bare Enter takes it.
    {
        spawned = 0;
        Picker p = openPicker(&w);
        p.search->setText(QStringLiteral("Cr")); // matches Crate only
        QCoreApplication::processEvents();
        check("typing highlights the best match", p.current() == QStringLiteral("Crate"));
        key(p.search, Qt::Key_Return);
        check("Enter right after typing spawns that match", spawned == 13ull);
    }

    // Down must skip rows the filter hid — a filtered-out option is not walkable.
    {
        spawned = 0;
        Picker p = openPicker(&w);
        p.search->setText(QStringLiteral("o")); // Goblin, Orc — no Prop entries
        QCoreApplication::processEvents();
        check("filtering highlights the topmost match", p.current() == QStringLiteral("Goblin"));
        key(p.search, Qt::Key_Down);
        key(p.search, Qt::Key_Down); // would be Chest if hidden rows were walkable
        check("the walk stays inside the matches", p.current() == QStringLiteral("Orc"));
        key(p.search, Qt::Key_Return);
        check("Enter spawns the highlighted match (Orc)", spawned == 11ull);
    }

    // A filter that matches nothing: Enter is a no-op, not a spawn of something else.
    {
        spawned = 0;
        Picker p = openPicker(&w);
        p.search->setText(QStringLiteral("zzzz"));
        QCoreApplication::processEvents();
        key(p.search, Qt::Key_Down);
        key(p.search, Qt::Key_Return);
        check("Enter with no match spawns nothing", spawned == 0ull);
        check("...and the picker stays open", QApplication::activePopupWidget() != nullptr);
        key(p.search, Qt::Key_Escape);
        check("Esc closes the picker without spawning",
              spawned == 0ull && QApplication::activePopupWidget() == nullptr);
    }

    // A FLAT picker (no groups configured) walks exactly the same way.
    {
        rpe::EntityListWidget flat;
        flat.setEntityAddingEnabled(true);
        flat.setAddablePrefabs(QVector<Prefab> {
            { 20, QStringLiteral("Apple"), QString() },
            { 21, QStringLiteral("Banana"), QString() },
        });
        qulonglong picked = 0;
        QObject::connect(&flat, &rpe::EntityListWidget::spawnPrefabRequested, &flat,
                         [&](qulonglong id) { picked = id; });
        Picker p = openPicker(&flat);
        check("flat picker pre-highlights the first option", p.current() == QStringLiteral("Apple"));
        key(p.search, Qt::Key_Down);
        key(p.search, Qt::Key_Return);
        check("flat picker: arrow + Enter spawns the second option", picked == 21ull);
    }

    // No prefabs at all: the placeholder row is not an option, so Enter does nothing.
    {
        rpe::EntityListWidget empty;
        empty.setEntityAddingEnabled(true);
        bool spawnedAny = false;
        QObject::connect(&empty, &rpe::EntityListWidget::spawnPrefabRequested, &empty,
                         [&](qulonglong) { spawnedAny = true; });
        Picker p = openPicker(&empty);
        check("the '(no prefabs available)' row is never highlighted", p.current() == QStringLiteral("<none>"));
        key(p.search, Qt::Key_Down);
        key(p.search, Qt::Key_Return);
        check("Enter on an empty picker does nothing", !spawnedAny);
        key(p.search, Qt::Key_Escape);
    }
}

// ── The component picker (component list) — same behaviour, same keys ────────
static void testComponentPicker()
{
    printf("\n-- component picker --\n");
    rpe::ComponentListWidget w;
    w.setComponentEditingEnabled(true);
    w.setAddableEntries(QVector<Catalog> {
        { QStringLiteral("game.Health"), false },
        { QStringLiteral("game.Position"), false },
        { QStringLiteral("physics.Velocity"), false },
    });
    QString added;
    QObject::connect(&w, &rpe::ComponentListWidget::addComponentRequested, &w,
                     [&](const QString& n) { added = n; });

    {
        Picker p = openPicker(&w);
        check("picker opens with a search box and a tree", p.search && p.tree);
        check("the topmost option is highlighted before any typing",
              p.current() == QStringLiteral("Health"));
        key(p.search, Qt::Key_Down);
        check("Down walks to the next option", p.current() == QStringLiteral("Position"));
        key(p.search, Qt::Key_Down);
        check("Down crosses a namespace header", p.current() == QStringLiteral("Velocity"));
        key(p.search, Qt::Key_Tab);
        check("Tab wraps past the last option", p.current() == QStringLiteral("Health"));
        key(p.search, Qt::Key_Return);
        check("Enter adds the highlighted component by its FULL path",
              added == QStringLiteral("game.Health"));
        check("...and closes the picker", QApplication::activePopupWidget() == nullptr);
    }

    {
        added.clear();
        Picker p = openPicker(&w);
        p.search->setText(QStringLiteral("Vel"));
        QCoreApplication::processEvents();
        check("typing highlights the best match", p.current() == QStringLiteral("Velocity"));
        key(p.search, Qt::Key_Return);
        check("Enter right after typing adds that match", added == QStringLiteral("physics.Velocity"));
    }

    {
        added.clear();
        Picker p = openPicker(&w);
        p.search->setText(QStringLiteral("zzzz"));
        QCoreApplication::processEvents();
        key(p.search, Qt::Key_Return);
        check("Enter with no match adds nothing", added.isEmpty());
        key(p.search, Qt::Key_Escape);
        check("Esc closes the picker", QApplication::activePopupWidget() == nullptr);
    }
}

// ── The panel filter boxes drive their own list ──────────────────────────────
static void testPanelFilters()
{
    printf("\n-- panel filters --\n");

    // Component panel: rows are hidden by the filter, so the walk must skip them.
    {
        rpe::ComponentListWidget w;
        w.show();
        QVector<rpe::MirrorChannel::ComponentRow> rows;
        for (const char* n : { "Alpha", "Beta", "Gamma" })
        {
            rpe::MirrorChannel::ComponentRow r;
            r.name = QString::fromLatin1(n);
            rows.append(r);
        }
        w.setComponentRows(rows);
        QCoreApplication::processEvents();

        auto* list = w.findChild<QListWidget*>();
        auto* filter = w.findChild<QLineEdit*>();
        check("panel has a filter box and a list", list && filter);

        list->setCurrentRow(-1);
        key(filter, Qt::Key_Down);
        check("Down from the filter box selects the first row",
              list->currentItem() && list->currentItem()->text() == QStringLiteral("Alpha"));
        key(filter, Qt::Key_Down);
        check("Down walks on", list->currentItem()->text() == QStringLiteral("Beta"));
        key(filter, Qt::Key_Up);
        check("Up walks back", list->currentItem()->text() == QStringLiteral("Alpha"));
        check("focus stayed in the filter box so typing continues", w.focusWidget() == filter);

        filter->setText(QStringLiteral("a")); // Alpha, Beta, Gamma all match
        QCoreApplication::processEvents();
        filter->setText(QStringLiteral("mm")); // only Gamma
        QCoreApplication::processEvents();
        key(filter, Qt::Key_Down);
        check("the walk stays inside the visible rows",
              list->currentItem() && list->currentItem()->text() == QStringLiteral("Gamma"));

        key(filter, Qt::Key_Escape);
        check("Esc clears the filter", filter->text().isEmpty());

        key(filter, Qt::Key_Return);
        check("Enter hands focus to the list", w.focusWidget() == list);

        // …and Tab out of the rows goes straight back to the filter, so the panel
        // is a loop: type → Enter/arrows → browse → Tab → refine.
        const QString selected = list->currentItem()->text();
        key(list, Qt::Key_Tab);
        check("Tab from the list returns focus to the filter box", w.focusWidget() == filter);
        check("...without disturbing the selection", list->currentItem()->text() == selected);

        key(filter, Qt::Key_Return);
        key(list, Qt::Key_Backtab);
        check("Shift+Tab from the list does the same", w.focusWidget() == filter);

        // The filter text survives the trip with the caret at its end — you come
        // back here to REFINE, so the next keystroke must not wipe what you typed.
        filter->setText(QStringLiteral("a"));
        QCoreApplication::processEvents();
        key(filter, Qt::Key_Return);
        key(list, Qt::Key_Tab);
        check("the filter text is kept, not selected-for-overwrite",
              filter->text() == QStringLiteral("a") && !filter->hasSelectedText()
                  && filter->cursorPosition() == 1);
        filter->clear();

        // The list is out of the TAB CHAIN, so Tab from the filter leaves the panel
        // instead of dropping back into the rows — the loop can always be escaped.
        filter->setFocus();
        check("the list is focusable by click, not by Tab", list->focusPolicy() == Qt::ClickFocus);
        key(filter, Qt::Key_Tab);
        check("Tab from the filter does not drop back into the rows", w.focusWidget() != list);
    }

    // Entity panel (mirror mode): the filter REBUILDS the list rather than hiding
    // rows, so the walk has to see the rebuilt set, not a stale one.
    {
        rpe::EntityListWidget w;
        w.show();
        w.setEntries(QVector<QPair<qulonglong, QString>> {
            { 1, QStringLiteral("Hero") },
            { 2, QStringLiteral("Villain") },
            { 3, QStringLiteral("Bystander") },
        });
        QCoreApplication::processEvents();

        auto* list = w.findChild<QListWidget*>();
        auto* filter = w.findChild<QLineEdit*>();
        check("entity list populated", list && list->count() == 3);

        list->setCurrentRow(-1);
        key(filter, Qt::Key_Down);
        check("Down from the filter box selects the first entity (alphabetical)",
              list->currentItem() && list->currentItem()->text() == QStringLiteral("Bystander"));
        key(filter, Qt::Key_Down);
        check("Down walks on", list->currentItem()->text() == QStringLiteral("Hero"));
        check("focus stayed in the filter box", w.focusWidget() == filter);

        filter->setText(QStringLiteral("Vill"));
        QCoreApplication::processEvents();
        check("filtering rebuilt the list down to the match", list->count() == 1);
        key(filter, Qt::Key_Down);
        check("Down selects the only match",
              list->currentItem() && list->currentItem()->text() == QStringLiteral("Villain"));

        key(filter, Qt::Key_Escape);
        QCoreApplication::processEvents();
        check("Esc clears the filter and the list comes back", list->count() == 3);

        key(filter, Qt::Key_Return);
        check("Enter hands focus to the list", w.focusWidget() == list);
        key(list, Qt::Key_Tab);
        check("Tab from the entity list returns focus to its filter box", w.focusWidget() == filter);
    }
}

// ── Tab still crosses the panels, so the per-panel loop is never a trap ──────
static void testBrowserTabChain()
{
    printf("\n-- browser tab chain --\n");
    rpe::EntityComponentBrowser browser;
    browser.resize(420, 640);
    browser.show();
    QCoreApplication::processEvents();

    auto* entityFilter = browser.entityList()->findChild<QLineEdit*>();
    auto* entityRows = browser.entityList()->findChild<QListWidget*>();
    auto* compFilter = browser.componentList()->findChild<QLineEdit*>();
    check("both panels expose a filter box", entityFilter && compFilter);

    entityFilter->setFocus();
    key(entityFilter, Qt::Key_Tab);
    check("Tab leaves the entity panel for the component panel",
          browser.focusWidget() == compFilter);
    check("...rather than dropping into the entity rows", browser.focusWidget() != entityRows);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testPrefabPicker();
    testComponentPicker();
    testPanelFilters();
    testBrowserTabChain();

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

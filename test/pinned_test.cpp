// Pinned watches: values flow for entities/components OUTSIDE the current
// selection, pin edits write back to the world, the widget rows update, and
// pinned paths tint the main property tree.
#include <rpe/core/TypeBridge.h>
#include <rpe/core/TypeRenderer.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/PinnedPropertiesWidget.h>
#include <rpe/gui/EditorWidgets.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QAbstractItemDelegate>
#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QStyleOptionViewItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstdio>
#include <filesystem>

struct Health
{
    int hp = 100;
};
struct Speed
{
    double v = 5.0;
};
struct Armor // bridged LATE in the test (plugin load-order scenario)
{
    int def = 1;
};

enum class Team
{
    Red,
    Blue,
    Green
};
struct Unit // exercises the type-specialized pin editors (bool / enum / int)
{
    bool alive = true;
    Team team = Team::Red;
    int level = 3;
};
struct Doc // a std::filesystem::path leaf → FilePathEditor (modal-picker editor)
{
    std::filesystem::path file = "a.txt";
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
    rttr::registration::class_<Speed>("Speed").property("v", &Speed::v);
    rttr::registration::class_<Armor>("Armor").property("def", &Armor::def);
    rttr::registration::enumeration<Team>("Team")(
        rttr::value("Red", Team::Red),
        rttr::value("Blue", Team::Blue),
        rttr::value("Green", Team::Green));
    rttr::registration::class_<Unit>("Unit")
        .property("alive", &Unit::alive)
        .property("team", &Unit::team)
        .property("level", &Unit::level);
    rttr::registration::class_<Doc>("Doc").property("file", &Doc::file);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerTypes<Health, Speed>();

    flecs::world world;
    auto a = world.entity("A").set<Health>({ 70 });
    auto b = world.entity("B").set<Speed>({ 3.5 });
    const auto aid = static_cast<qulonglong>(a.id());
    const auto bid = static_cast<qulonglong>(b.id());

    rpe::EcsMirror mirror;
    mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);

    // Interest is on A/Health — B is NOT selected anywhere.
    mirror.setInterest(aid, "Health", { "hp" });

    // ── 1. Pins flow for a non-selected entity ────────────────────────────────
    mirror.setPins({ { bid, QStringLiteral("Speed"), QStringLiteral("v") } });
    world.progress(0.016f);
    mirror.pump();
    {
        const auto pins = mirror.pollPinValues();
        check("pinned value for a NON-selected entity arrives", pins.size() == 1);
        check("pinned value is correct (v == 3.5)",
              !pins.empty() && pins[0].key.entity == bid && pins[0].value.to_double() == 3.5);
    }

    // Unchanged value → deduped, nothing republished.
    world.progress(0.016f);
    mirror.pump();
    check("unchanged pin publishes nothing (dedup)", mirror.pollPinValues().empty());

    // ── 2. Pin edit writes back to the world ──────────────────────────────────
    // An OnSet observer must fire for inspector edits (ecs_modified_id is called
    // after the write), so reactive systems / change detection see them.
    int onSetCount = 0;
    world.observer<Speed>().event(flecs::OnSet).each([&](flecs::entity, Speed&) { ++onSetCount; });

    mirror.queuePinEdit({ bid, QStringLiteral("Speed"), QStringLiteral("v") }, rttr::variant(9.25));
    world.progress(0.016f);
    mirror.pump();
    check("pin edit reached the world (v == 9.25)", b.get<Speed>().v == 9.25);
    check("pin edit fires OnSet observers", onSetCount >= 1);
    {
        const auto pins = mirror.pollPinValues();
        check("edited pin value echoes back", pins.size() == 1 && pins[0].value.to_double() == 9.25);
    }

    // ── 3. The widget: rows, live text, tint set, unpin ───────────────────────
    {
        rpe::PinnedPropertiesWidget w;
        int changed = 0;
        QObject::connect(&w, &rpe::PinnedPropertiesWidget::pinsChanged, &w, [&] { ++changed; });
        w.setChannel(mirror.channel());
        w.pin(bid, QStringLiteral("B"), QStringLiteral("Speed"), QStringLiteral("v"));
        w.pin(aid, QStringLiteral("A"), QStringLiteral("Health"), QStringLiteral("hp"));
        check("pin() adds rows + notifies", w.pins().size() == 2 && changed == 2);
        check("duplicate pin is ignored", (w.pin(bid, QStringLiteral("B"), QStringLiteral("Speed"), QStringLiteral("v")), w.pins().size() == 2));

        world.progress(0.016f);
        mirror.pump();
        w.pollNow();
        auto* tree = w.findChild<QTreeWidget*>();
        QString vText, hpText;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
        {
            const auto* it = tree->topLevelItem(i);
            if (it->text(1).contains(QStringLiteral("Speed")))
                vText = it->text(2);
            else
                hpText = it->text(2);
        }
        check("widget shows the live pinned values", vText == QStringLiteral("9.25") && hpText == QStringLiteral("70"));

        check("pinnedPaths() filters by entity+component",
              w.pinnedPaths(bid, QStringLiteral("Speed")) == QSet<QString>({ QStringLiteral("v") })
                  && w.pinnedPaths(aid, QStringLiteral("Health")) == QSet<QString>({ QStringLiteral("hp") })
                  && w.pinnedPaths(aid, QStringLiteral("Speed")).isEmpty());

        w.unpin(bid, QStringLiteral("Speed"), QStringLiteral("v"));
        check("unpin removes the row", w.pins().size() == 1 && changed == 3);
    }

    // ── 3b. A pin is editable IMMEDIATELY, from its declared type ───────────────
    // Before this fix a fresh pin showed a dead "…" cell that couldn't be edited
    // until the first mirror value arrived. The editor now comes from the leaf's
    // DECLARED type (resolved from the component + path), so int → spin box opens
    // straight away, seeded at the type default.
    {
        rpe::PinnedPropertiesWidget w;
        w.setChannel(mirror.channel());
        w.pin(aid, QStringLiteral("A"), QStringLiteral("Health"), QStringLiteral("hp"));
        auto* tree = w.findChild<QTreeWidget*>();
        QTreeWidgetItem* it = tree->topLevelItem(0);

        check("a fresh pin shows the placeholder until a value lands", it->text(2) == QStringLiteral("…"));
        // Double-click exactly as a user would: the view opens the editor via the
        // delegate and seeds it (from the live value if any, else the type default).
        tree->itemDoubleClicked(it, 2);
        auto* spin = tree->viewport()->findChild<QSpinBox*>();
        check("double-clicking a fresh pin opens a type editor (int → spin box)", spin != nullptr);
        check("the fresh-pin editor seeds at the type default (0)", spin && spin->value() == 0);
    }

    // The land-a-value-then-edit flow (fresh widget so live polling isn't paused).
    {
        rpe::PinnedPropertiesWidget w;
        w.setChannel(mirror.channel());
        w.pin(aid, QStringLiteral("A"), QStringLiteral("Health"), QStringLiteral("hp"));
        auto* tree = w.findChild<QTreeWidget*>();
        QTreeWidgetItem* it = tree->topLevelItem(0);
        auto* delegate = tree->itemDelegateForColumn(2);
        const QModelIndex vIdx = tree->model()->index(0, 2);
        const QStyleOptionViewItem opt;

        world.progress(0.016f);
        mirror.pump();
        w.pollNow();
        check("first mirrored value lands (70)", it->text(2) == QStringLiteral("70"));
        QWidget* editor = delegate->createEditor(tree->viewport(), opt, vIdx);
        delegate->setEditorData(editor, vIdx);
        auto* spin = qobject_cast<QSpinBox*>(editor);
        check("int pin leaf opens a QSpinBox", spin != nullptr);
        check("the spin box is seeded with the live value (70)", spin && spin->value() == 70);
        spin->setValue(55);
        delegate->setModelData(editor, tree->model(), vIdx);
        check("committed value echoes in the row (55)", it->text(2) == QStringLiteral("55"));
        world.progress(0.016f);
        mirror.pump();
        check("delegate edit reached the world (hp == 55)", a.get<Health>().hp == 55);
        delete editor;
    }

    // ── 3c. The pin editor is TYPE-SPECIALIZED like the property grid ──────────
    {
        rpe::TypeBridge::registerType<Unit>();
        auto d = world.entity("D").set<Unit>({ true, Team::Blue, 7 });
        const auto did = static_cast<qulonglong>(d.id());

        rpe::PinnedPropertiesWidget w;
        w.setChannel(mirror.channel());
        w.pin(did, QStringLiteral("D"), QStringLiteral("Unit"), QStringLiteral("alive"));
        w.pin(did, QStringLiteral("D"), QStringLiteral("Unit"), QStringLiteral("team"));
        w.pin(did, QStringLiteral("D"), QStringLiteral("Unit"), QStringLiteral("level"));
        world.progress(0.016f);
        mirror.pump();
        w.pollNow();

        auto* tree = w.findChild<QTreeWidget*>();
        auto* delegate = tree->itemDelegateForColumn(2);
        const QStyleOptionViewItem opt;
        auto editorFor = [&](const QString& leaf) -> QWidget* {
            for (int i = 0; i < tree->topLevelItemCount(); ++i)
            {
                if (tree->topLevelItem(i)->text(1).endsWith(leaf))
                {
                    return delegate->createEditor(tree->viewport(), opt, tree->model()->index(i, 2));
                }
            }
            return nullptr;
        };

        auto rowFor = [&](const QString& leaf) {
            for (int i = 0; i < tree->topLevelItemCount(); ++i)
                if (tree->topLevelItem(i)->text(1).endsWith(leaf))
                    return i;
            return -1;
        };

        QWidget* boolEd = editorFor(QStringLiteral("alive"));
        QWidget* enumEd = editorFor(QStringLiteral("team"));
        QWidget* intEd = editorFor(QStringLiteral("level"));
        check("bool pin leaf opens a QCheckBox", qobject_cast<QCheckBox*>(boolEd) != nullptr);
        check("enum pin leaf opens a QComboBox", qobject_cast<QComboBox*>(enumEd) != nullptr);
        check("int pin leaf opens a QSpinBox", qobject_cast<QSpinBox*>(intEd) != nullptr);

        // The combo is populated with the enum's names, seeded to the live value, and
        // a new selection reaches the world as the exact enum type.
        if (auto* combo = qobject_cast<QComboBox*>(enumEd))
        {
            const QModelIndex teamIdx = tree->model()->index(rowFor(QStringLiteral("team")), 2);
            delegate->setEditorData(combo, teamIdx);
            check("enum combo lists all names", combo->count() == 3);
            check("enum combo is seeded to the live value (Blue)", combo->currentText() == QStringLiteral("Blue"));

            combo->setCurrentText(QStringLiteral("Green"));
            delegate->setModelData(combo, tree->model(), teamIdx);
            world.progress(0.016f);
            mirror.pump();
            check("enum pin edit reached the world (team == Green)", d.get<Unit>().team == Team::Green);
        }

        // Commit a bool toggle through the check box.
        if (auto* box = qobject_cast<QCheckBox*>(boolEd))
        {
            const QModelIndex aliveIdx = tree->model()->index(rowFor(QStringLiteral("alive")), 2);
            delegate->setEditorData(box, aliveIdx);
            check("check box is seeded to the live value (checked)", box->isChecked());
            box->setChecked(false);
            delegate->setModelData(box, tree->model(), aliveIdx);
            world.progress(0.016f);
            mirror.pump();
            check("bool pin edit reached the world (alive == false)", d.get<Unit>().alive == false);
        }

        delete boolEd;
        delete enumEd;
        delete intEd;
    }

    // ── 3d. A live poll MUST NOT clobber an open editor (modal-picker safety) ───
    // A file/color picker steals focus from the tree while it's up, so the live
    // ~30 Hz refresh must skip the edited row by tracking it explicitly (a
    // focus-based guard would overwrite the cell here and destroy the editor,
    // discarding the value being picked). Exercised with an fs::path leaf.
    {
        rpe::TypeBridge::registerType<Doc>();
        auto e = world.entity("Doc1").set<Doc>({ "orig.txt" });
        const auto id = static_cast<qulonglong>(e.id());

        rpe::PinnedPropertiesWidget w;
        w.setChannel(mirror.channel());
        w.pin(id, QStringLiteral("Doc1"), QStringLiteral("Doc"), QStringLiteral("file"));
        world.progress(0.016f);
        mirror.pump();
        w.pollNow();
        auto* tree = w.findChild<QTreeWidget*>();
        QTreeWidgetItem* it = tree->topLevelItem(0);
        check("path pin shows its value", it->text(2) == QStringLiteral("orig.txt"));
        check("path leaf is a std::filesystem::path variant",
              rpe::TypeRenderer::isFilePath(rpe::TypeRenderer::rawType(
                  it->data(0, Qt::UserRole + 3).value<rttr::variant>().get_type())));

        // Open the editor exactly as a double-click would (this arms the edit guard).
        tree->itemDoubleClicked(it, 2);
        auto* fe = tree->viewport()->findChild<rpe::FilePathEditor*>();
        check("path pin opens a FilePathEditor", fe != nullptr);

        // Mimic the modal picker: focus leaves the tree AND a fresh live value lands.
        QWidget other;
        other.show();
        other.setFocus();
        fe->setPath(QStringLiteral("picked/from/dialog.dat"));
        e.set<Doc>({ "changed-by-sim.txt" });
        world.progress(0.016f);
        mirror.pump();
        w.pollNow();
        check("live poll does not overwrite the row being edited", it->text(2) == QStringLiteral("orig.txt"));
        check("the in-progress picked path survives the poll", fe->path() == QStringLiteral("picked/from/dialog.dat"));

        // Commit → the browsed path reaches the world.
        auto* delegate = tree->itemDelegateForColumn(2);
        delegate->setModelData(fe, tree->model(), tree->model()->index(0, 2));
        world.progress(0.016f);
        mirror.pump();
        check("browsed path commits to the world",
              e.get<Doc>().file == std::filesystem::path("picked/from/dialog.dat"));
    }

    // ── 4. Pinned rows tint the main property tree ─────────────────────────────
    {
        rpe::PropertyModel model;
        model.bindType(rttr::type::get<Health>());
        model.setPinnedPaths({ QStringLiteral("hp") });
        const QModelIndex idx = model.index(0, 0, {});
        const QBrush fg = model.data(idx, Qt::ForegroundRole).value<QBrush>();
        check("pinned path is tinted teal in the model", fg.color() == QColor(0x4D, 0xB6, 0xAC));
        model.setPinnedPaths({});
        check("clearing pins clears the tint", !model.data(idx, Qt::ForegroundRole).isValid());
    }

    // ── 4b. Tags / name-colliding plain entities never reach the component list ─
    // A marker entity named "Speed" (in a scope, so the leaf collides with the
    // bridged type) attached to the entity would pass the name-based resolve and
    // then ASSERT in debug flecs when the GUI selects it (get_mut on a dataless
    // id). It must be filtered out of the published component list.
    {
        auto scopeParent = world.entity("markers");
        auto marker = world.scope(scopeParent).entity("Speed"); // leaf "Speed", no data
        b.add(marker);

        mirror.setInterest(bid, "Speed", { QStringLiteral("v") });
        world.progress(0.016f);
        mirror.pump();
        QStringList comps;
        mirror.pollComponents(comps);
        check("dataless name-colliding id is NOT listed as a component",
              comps.contains(QStringLiteral("Speed")) && !comps.contains(QStringLiteral("markers.Speed")));

        // Selecting the real component still works (values flow, no assert).
        bool gotV = false;
        for (const auto& u : mirror.pollValues())
            gotV |= (u.path == QStringLiteral("v"));
        check("real component still mirrors alongside the marker", gotV);
        b.remove(marker);
    }

    // ── 5. A LATE bridge registration is picked up (registry generation) ───────
    // Plugin load order: the flecs component exists first, the RPE bridge arrives
    // later. The bridged-id cache must rebuild on the registry-generation bump —
    // the component-type count alone doesn't change at registration time.
    {
        auto c = world.entity("C").set<Armor>({ 3 }); // Armor NOT bridged yet
        mirror.setScanIntervalsMs(0, 0);              // scan every pump
        // Interest BEFORE the bridge exists: the selected-type and component-list
        // caches must not freeze the "unresolvable" state (they retry on the
        // registry-generation bump / invalid-type re-resolve).
        mirror.setInterest(static_cast<qulonglong>(c.id()), "Armor", { QStringLiteral("def") });
        world.progress(0.016f);
        mirror.pump();
        mirror.pollValues(); // drain (nothing meaningful can flow yet)
        QVector<rpe::EcsMirror::EntityEntry> ents;
        mirror.pollEntities(ents); // drain (C absent: only unbridged Armor on it)
        bool hasC = false;
        for (const auto& en : ents)
            hasC |= (en.id == static_cast<qulonglong>(c.id()));
        check("entity with only an unbridged component is not listed", !hasC);

        rpe::TypeBridge::registerType<Armor>(); // the "plugin" registers late
        world.progress(0.016f);
        mirror.pump();
        check("late bridge registration republishes the entity list", mirror.pollEntities(ents));
        hasC = false;
        for (const auto& en : ents)
            hasC |= (en.id == static_cast<qulonglong>(c.id()));
        check("late-bridged component's entity appears in the list", hasC);

        bool gotDef = false;
        for (const auto& u : mirror.pollValues())
            gotDef |= (u.path == QStringLiteral("def") && rpe::TypeRenderer::toDisplayString(u.value) == QStringLiteral("3"));
        check("values flow for the late-bridged selection (def == 3)", gotDef);
    }

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

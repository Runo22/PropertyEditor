// Pinned watches: values flow for entities/components OUTSIDE the current
// selection, pin edits write back to the world, the widget rows update, and
// pinned paths tint the main property tree.
#include <rpe/core/TypeBridge.h>
#include <rpe/core/TypeRenderer.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/PinnedPropertiesWidget.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QBrush>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstdio>

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

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
    rttr::registration::class_<Speed>("Speed").property("v", &Speed::v);
    rttr::registration::class_<Armor>("Armor").property("def", &Armor::def);
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

    // ── 3b. Widget edits: gated before the first value, applied after ──────────
    {
        rpe::PinnedPropertiesWidget w;
        w.setChannel(mirror.channel());
        w.pin(aid, QStringLiteral("A"), QStringLiteral("Health"), QStringLiteral("hp"));
        auto* tree = w.findChild<QTreeWidget*>();
        QTreeWidgetItem* it = tree->topLevelItem(0);

        // Edit BEFORE any mirrored value: parse has no type anchor → must be
        // ignored and the placeholder restored (not sent as a string and dropped).
        it->setText(2, QStringLiteral("55"));
        check("edit before the first value restores the placeholder", it->text(2) == QStringLiteral("…"));
        world.progress(0.016f);
        mirror.pump();
        check("no edit was queued (hp still 70)", a.get<Health>().hp == 70);

        // First value arrives → edits work and reach the world.
        w.pollNow();
        check("first mirrored value lands (70)", it->text(2) == QStringLiteral("70"));
        it->setText(2, QStringLiteral("55"));
        world.progress(0.016f);
        mirror.pump();
        check("widget edit reached the world (hp == 55)", a.get<Health>().hp == 55);
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

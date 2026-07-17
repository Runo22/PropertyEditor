// Tags & pairs in the component list: zero-size tags and relationship pairs are
// published as badge rows (never as selectable data), removable by flecs id,
// and zero-size tags are offered by the add catalog.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QListWidget>

#include <cstdio>

struct Health
{
    int hp = 100;
};
struct Burning // zero-size flecs TAG (empty struct)
{
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

using Row = rpe::MirrorChannel::ComponentRow;
using Kind = rpe::MirrorChannel::RowKind;

static const Row* findRow(const QVector<Row>& rows, const QString& name, Kind k)
{
    for (const auto& r : rows)
        if (r.name == name && r.kind == k)
            return &r;
    return nullptr;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerType<Health>();

    flecs::world world;
    world.component<Burning>(); // registers the zero-size tag type
    auto likes = world.entity("Likes");
    auto bob = world.entity("Bob");
    auto e = world.entity("E").set<Health>({ 70 });
    e.add<Burning>();
    e.add(likes, bob); // relationship pair (Likes, Bob)

    rpe::EcsMirror mirror;
    mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);
    mirror.setScanIntervalsMs(0, 0);
    mirror.setInterest(static_cast<qulonglong>(e.id()), "Health", { "hp" });
    world.progress(0.016f);
    mirror.pump();

    auto ch = mirror.channel();

    // ── 1. Rows: data + tag + pair, with removal ids ──────────────────────────
    QVector<Row> rows;
    check("component rows published", ch->pollComponentRows(rows));
    const Row* data = findRow(rows, QStringLiteral("Health"), Kind::Data);
    const Row* tag = findRow(rows, QStringLiteral("Burning"), Kind::Tag);
    const Row* pair = findRow(rows, QStringLiteral("Likes"), Kind::Pair);
    check("data row listed", data != nullptr);
    check("tag row listed with its id", tag && tag->rawId != 0);
    check("pair row listed with target + id", pair && pair->pairTarget == QStringLiteral("Bob") && pair->rawId != 0);

    // Legacy name poll must expose ONLY data rows (drives interest/selection).
    // (Rows were drained above; republish via resync to exercise the adapter.)
    ch->requestResync();
    world.progress(0.016f);
    mirror.pump();
    QStringList names;
    check("legacy pollComponents lists only data rows",
          ch->pollComponents(names) && names == QStringList { QStringLiteral("Health") });

    // ── 2. Remove the PAIR by id (unaddressable by name) ──────────────────────
    ch->queueStructuralById(rpe::MirrorChannel::StructuralKind::RemoveComponent,
                            static_cast<qulonglong>(e.id()), pair->rawId);
    world.progress(0.016f);
    mirror.pump();
    check("pair removed from the world", !e.has(likes, bob));

    // ── 3. Remove the TAG by id, re-add via the catalog name ──────────────────
    ch->queueStructuralById(rpe::MirrorChannel::StructuralKind::RemoveComponent,
                            static_cast<qulonglong>(e.id()), tag->rawId);
    world.progress(0.016f);
    mirror.pump();
    check("tag removed from the world", !e.has<Burning>());

    QVector<rpe::MirrorChannel::CatalogEntry> catalog;
    ch->pollCatalogEntries(catalog);
    bool tagInCatalog = false;
    for (const auto& c : catalog)
        tagInCatalog |= (c.path == QStringLiteral("Burning") && c.tag);
    check("zero-size tag is offered by the add catalog", tagInCatalog);

    ch->queueStructural(rpe::MirrorChannel::StructuralKind::AddComponent,
                        static_cast<qulonglong>(e.id()), QStringLiteral("Burning"));
    world.progress(0.016f);
    mirror.pump();
    check("tag re-added by name from the catalog", e.has<Burning>());

    // ── 4. Widget: badge rows are not selectable; data row drives selection ────
    {
        rpe::ComponentListWidget w;
        QString selected;
        QObject::connect(&w, &rpe::ComponentListWidget::componentNameSelected, &w,
                         [&](const QString& n) { selected = n; });
        QVector<Row> uiRows = {
            { QStringLiteral("Likes"), QStringLiteral("Bob"), Kind::Pair, 42 },
            { QStringLiteral("Burning"), QString(), Kind::Tag, 7 },
            { QStringLiteral("Health"), QString(), Kind::Data, 0 },
        };
        w.setComponentRows(uiRows);
        auto* lw = w.findChild<QListWidget*>();
        check("rows ordered data < tag < pair",
              lw->item(0)->text() == QStringLiteral("Health") && lw->item(1)->text() == QStringLiteral("Burning")
                  && lw->item(2)->text().startsWith(QStringLiteral("Likes")));
        check("data row auto-selected, emits its path", selected == QStringLiteral("Health"));
        check("tag row is not selectable", !(lw->item(1)->flags() & Qt::ItemIsSelectable));
        check("pair row displays Rel → Target", lw->item(2)->text().contains(QStringLiteral("Bob")));
    }

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

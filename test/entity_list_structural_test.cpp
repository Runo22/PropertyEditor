// A structural edit on a big world must not disturb the entity list.
//
// Two defects, found together while reviewing the add-component path:
//
//  1. The list is capped (5000 rows). The cap was applied in SNAPSHOT order, and the
//     snapshot is taken table by table — so adding a component to an entity moved
//     it to another table and could push it past the cap. The selected entity
//     silently dropped out; the GUI took that as a deletion and selected a neighbour.
//
//  2. A structural edit cleared the producer's last-published caches, and the resync
//     that accompanies it then re-published them — as EMPTY lists. When the entity
//     scan spans several pumps (any world big enough to be sliced), the GUI sat on an
//     empty entity list for those pumps and lost the selection.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QListWidget>
#include <QSet>
#include <QThread>

#include <cstdio>

struct Health
{
    int hp = 0;
};
struct Mana
{
    int mp = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
    rttr::registration::class_<Mana>("Mana").property("mp", &Mana::mp);
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
    rpe::TypeBridge::registerTypes<Health, Mana>();

    flecs::world w;
    w.component<Mana>();
    for (const char* n : { "Alpha", "Bravo", "Charlie", "Delta" })
        w.entity(n).set<Health>({ 1 });
    for (int i = 0; i < 6000; ++i) // past the 5000-row cap, and big enough to slice
        w.entity().set<Health>({ i });

    rpe::EcsMirror mirror;
    mirror.attach(&w); // DEFAULT intervals + budget: the scan spans several pumps
    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    browser.setComponentEditingEnabled(true);
    browser.show();

    auto* list = browser.entityList()->findChild<QListWidget*>();
    auto ids = [&] {
        QSet<qulonglong> s;
        for (int i = 0; i < list->count(); ++i)
            s.insert(list->item(i)->data(Qt::UserRole).toULongLong());
        return s;
    };
    auto current = [&] { return list->currentItem() ? list->currentItem()->text() : QStringLiteral("<none>"); };

    int emptyTicks = 0;
    bool moved = false;
    auto pump = [&](int n, bool watch) {
        for (int i = 0; i < n; ++i)
        {
            w.progress(0.033f);
            QCoreApplication::processEvents();
            QThread::msleep(5);
            if (watch)
            {
                emptyTicks += list->count() == 0 ? 1 : 0;
                moved = moved || current() != QStringLiteral("Charlie");
            }
        }
    };

    pump(60, false);
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == QStringLiteral("Charlie"))
            list->setCurrentRow(i);
    pump(30, false);
    check("the list is capped at 5000 rows", list->count() == 5000);
    check("Charlie is selected", current() == QStringLiteral("Charlie"));
    const QSet<qulonglong> before = ids();

    // Add a component to Charlie exactly as the Add picker does: queue + resync.
    const qulonglong charlie = list->currentItem()->data(Qt::UserRole).toULongLong();
    mirror.channel()->queueStructural(rpe::MirrorChannel::StructuralKind::AddComponent, charlie,
                                      QStringLiteral("Mana"));
    mirror.channel()->requestResync();
    pump(60, true);

    check("the entity list never went empty during the rescan", emptyTicks == 0);
    check("the selection stayed on Charlie throughout", !moved);
    check("the component was actually added", w.entity(static_cast<flecs::entity_t>(charlie)).has<Mana>());
    check("Charlie is still listed after moving to a new archetype", ids().contains(charlie));
    check("the capped list's MEMBERSHIP didn't reshuffle", ids() == before);

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

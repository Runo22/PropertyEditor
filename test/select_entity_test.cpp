// EntityComponentBrowser::selectEntity(flecs::entity) — direct call AND across a
// queued connection (Q_ARG(flecs::entity, e)), proving the metatype registration.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QListWidget>

#include <cstdio>

struct Tag
{
    int v = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Tag>("Tag").property("v", &Tag::v);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static qulonglong currentId(rpe::EntityComponentBrowser& b)
{
    auto* lw = b.entityList()->findChild<QListWidget*>();
    return (lw && lw->currentItem()) ? lw->currentItem()->data(Qt::UserRole).toULongLong() : 0;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerType<Tag>();

    flecs::world world;
    world.component<Tag>();
    flecs::entity alpha = world.entity("Alpha").set<Tag>({ 1 });
    flecs::entity beta = world.entity("Beta").set<Tag>({ 2 });
    flecs::entity gamma = world.entity("Gamma").set<Tag>({ 3 });

    rpe::EntityComponentBrowser browser;
    int selCount = 0;
    qulonglong lastSel = 0;
    QObject::connect(&browser, &rpe::EntityComponentBrowser::entityIdSelected, &browser, [&](qulonglong id) {
        ++selCount;
        lastSel = id;
    });

    browser.setWorld(&world); // direct mode; the list populates + auto-selects the top
    QCoreApplication::processEvents();
    check("list populated (top auto-selected == Alpha)", currentId(browser) == static_cast<qulonglong>(alpha.id()));

    // ── Direct call with a flecs::entity ──────────────────────────────────────
    check("selectEntity(entity) returns true", browser.selectEntity(beta) == true);
    check("selectEntity(entity) selected Beta + emitted",
          currentId(browser) == static_cast<qulonglong>(beta.id()) && lastSel == static_cast<qulonglong>(beta.id()));

    // ── Queued connection: Q_ARG(flecs::entity, e) ────────────────────────────
    const bool ok = QMetaObject::invokeMethod(&browser, "selectEntity", Qt::QueuedConnection, Q_ARG(flecs::entity, gamma));
    check("queued invoke of selectEntity(flecs::entity) accepted", ok);
    check("selection unchanged until the queued event is delivered", currentId(browser) == static_cast<qulonglong>(beta.id()));
    QCoreApplication::processEvents(); // deliver the queued call
    check("queued selectEntity(entity) selected Gamma", currentId(browser) == static_cast<qulonglong>(gamma.id()));

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

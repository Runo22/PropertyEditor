// Component discovery for the Add menu, across the orders plugins register in.
//
// Reported: "my friend can't see his component in the Add component menu".
// Reproduced three ways it could happen:
//   • a namespace that merely STARTS with "flecs" ("flecsx::…") was treated as
//     flecs' own scope and hidden — from the menu, the component list and the
//     entity scan alike;
//   • a component renamed after it was scanned, or swapped in while another was
//     unloaded in the same pump, never appeared: the catalog was rescanned only
//     when the component COUNT or the bridge registry changed;
//   • a component registered with RTTR but never TypeBridge::registerType'd is,
//     correctly, not offered — rpe::checkComponents() says why, on demand
//     (nothing is logged by itself: an automatic warning gave false positives).
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>
#include <rpe/ecs/HealthCheck.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QFrame>
#include <QListWidget>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>

#include <chrono>
#include <cstdio>
#include <functional>

struct Base
{
    int v = 0;
};
struct FlecsPrefixed
{
    int v = 0;
};
struct Renamed
{
    int v = 0;
};
struct Swapped
{
    int v = 0;
};
struct LateBridge
{
    int v = 0;
};
struct RttrOnly
{
    int v = 0;
};

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<Base>("Base").property("v", &Base::v);
    registration::class_<FlecsPrefixed>("flecsx::FlecsPrefixed").property("v", &FlecsPrefixed::v);
    registration::class_<Renamed>("plug::Renamed").property("v", &Renamed::v);
    registration::class_<Swapped>("plug::Swapped").property("v", &Swapped::v);
    registration::class_<LateBridge>("plug::LateBridge").property("v", &LateBridge::v);
    registration::class_<RttrOnly>("plug::RttrOnly").property("v", &RttrOnly::v);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static QStringList g_warnings;

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType t, const QMessageLogContext&, const QString& msg) {
        if (t == QtWarningMsg && msg.contains(QStringLiteral("no TypeBridge registration")))
            g_warnings << msg;
    });

    flecs::world w;
    rpe::TypeBridge::registerType<Base>();
    w.component<Base>("Base");
    w.entity("Hero").set<Base>({ 1 });

    rpe::EcsMirror mirror;
    mirror.attach(&w); // DEFAULT intervals, as in an app
    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    browser.setComponentEditingEnabled(true);
    browser.show();

    auto pumpMs = [&](int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end)
        {
            w.progress(0.033f);
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
    };
    auto* addBtn = browser.componentList()->findChild<QToolButton*>();
    // What the Add menu offers, opened the way a user does: pointer onto the
    // button (which asks the producer to refresh), a moment, then the click.
    auto offered = [&] {
        QEvent enter(QEvent::Enter);
        QCoreApplication::sendEvent(addBtn, &enter);
        pumpMs(150);
        addBtn->click();
        QCoreApplication::processEvents();
        auto* pop = browser.componentList()->findChild<QFrame*>(QStringLiteral("rpeAddPopup"));
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
        if (pop)
        {
            walk(pop->findChild<QTreeWidget*>()->invisibleRootItem());
            pop->close();
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return out;
    };
    auto has = [&](const QString& path) { return offered().contains(path); };

    pumpMs(400);
    auto* entities = browser.entityList()->findChild<QListWidget*>();
    if (entities->count())
        entities->setCurrentRow(0);
    pumpMs(200);

    // ── A namespace that merely starts with "flecs" is a USER namespace ────────
    rpe::TypeBridge::registerType<FlecsPrefixed>();
    w.component<FlecsPrefixed>("flecsx::FlecsPrefixed");
    pumpMs(100);
    check("\"flecsx::\" components are offered (not mistaken for flecs' own scope)",
          has(QStringLiteral("flecsx.FlecsPrefixed")));
    check("...while flecs' real built-ins still are not", !has(QStringLiteral("flecs.core.Identifier")));

    // ── Renamed after it was scanned (component count unchanged) ───────────────
    rpe::TypeBridge::registerType<Renamed>();
    auto rc = w.component<Renamed>("plug::TempName");
    pumpMs(300);
    rc.set_name("Renamed");
    pumpMs(100);
    check("a component renamed after the scan shows under its NEW name", has(QStringLiteral("plug.Renamed")));
    check("...and not under the old one", !has(QStringLiteral("plug.TempName")));

    // ── One unloaded and another loaded in the same pump (count unchanged) ─────
    rpe::TypeBridge::registerType<Swapped>();
    auto victim = w.entity("plug::Victim").set<flecs::Component>({ 4, 4 });
    pumpMs(300);
    victim.destruct();
    w.component<Swapped>("plug::Swapped");
    pumpMs(100);
    check("a component swapped in within one pump is offered", has(QStringLiteral("plug.Swapped")));

    // ── Registered with flecs first, bridged a moment later: fine, and quiet ───
    w.component<LateBridge>("plug::LateBridge");
    pumpMs(300);
    rpe::TypeBridge::registerType<LateBridge>();
    pumpMs(100);
    check("flecs-first-then-bridge is offered", has(QStringLiteral("plug.LateBridge")));

    // ── RTTR but no bridge: not offered — and the health check says why ────────
    w.component<RttrOnly>("plug::RttrOnly");
    pumpMs(100);
    check("an RTTR-only (unbridged) component is not offered", !has(QStringLiteral("plug.RttrOnly")));
    const rpe::HealthReport health = rpe::checkComponents(w);
    bool namesIt = false;
    for (const rpe::HealthIssue& i : health.byCheck(QStringLiteral("unbridged-rttr-type")))
        namesIt = namesIt || i.subject == QStringLiteral("plug.RttrOnly");
    check("...checkComponents() names it, with the fix",
          namesIt && health.about(QStringLiteral("plug.RttrOnly")).first().fix.contains(QStringLiteral("registerType")));
    check("a component bridged after its flecs registration has nothing to report",
          health.about(QStringLiteral("plug.LateBridge")).isEmpty());
    check("nothing is logged by itself", g_warnings.isEmpty());

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

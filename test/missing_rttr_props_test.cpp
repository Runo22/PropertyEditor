// Diagnostic for the "component is listed but shows NO editors" case (typically
// release-only).
//
// rttr::type::get<T>() is VALID for any complete type, even with no
// RTTR_REGISTRATION at all — it just carries no properties. So a component whose
// registration block was dropped (e.g. the linker stripped the static initializer
// of an unreferenced translation unit in a release build) still:
//   • gets a TypeBridge entry from the explicit registerType<T>() call,
//   • resolves by name, so it APPEARS in the component list,
//   • but binds an EMPTY schema → no editors.
// This test pins that mechanism so the symptom is identifiable, and contrasts it
// with a properly registered component.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>
#include <rpe/gui/PropertyEditor.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QListWidget>
#include <QThread>

#include <cstdio>

struct Reflected // properly registered below
{
    int good = 1;
};
struct Bare // deliberately NOT in any RTTR_REGISTRATION → no properties
{
    int hidden = 2;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Reflected>("Reflected").property("good", &Reflected::good);
    // NOTE: Bare is intentionally absent — it stands in for a registration block the
    // linker dropped in a release build.
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

    // Both get a bridge entry — exactly what an explicit registerType<T>() call does,
    // independently of whether the RTTR_REGISTRATION block ran.
    rpe::TypeBridge::registerTypes<Reflected, Bare>();

    // The unregistered type is still a VALID rttr::type — this is the crux.
    const rttr::type bareT = rttr::type::get<Bare>();
    check("an unregistered type is still rttr-valid", bareT.is_valid());
    check("...but exposes NO properties", bareT.get_properties().empty());
    check("a registered type does expose its properties",
          !rttr::type::get<Reflected>().get_properties().empty());

    flecs::world world;
    world.entity("WithBare").set<Bare>({ 7 });
    world.entity("WithReflected").set<Reflected>({ 5 });

    rpe::EcsMirror mirror;
    mirror.attach(&world);
    mirror.setScanIntervalsMs(0, 0);
    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    browser.resize(420, 640);
    browser.show();

    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(8);
        }
    };
    auto* el = browser.entityList()->findChild<QListWidget*>();
    auto* cl = browser.componentList()->findChild<QListWidget*>();
    auto selectEntity = [&](const QString& name) {
        for (int i = 0; i < el->count(); ++i)
            if (el->item(i)->text() == name)
                el->setCurrentRow(i);
    };

    pump(20);

    // The property-less component IS listed (it resolved) — the reported symptom.
    selectEntity(QStringLiteral("WithBare"));
    pump(20);
    check("a property-less component still appears in the component list", cl->count() >= 1);
    check("...but binds an EMPTY schema → no editors",
          browser.propertyEditor()->visibleLeafPaths(false).isEmpty());

    // The blank panel EXPLAINS itself rather than being silently empty.
    auto visibleHint = [&]() -> QString {
        for (auto* l : browser.propertyEditor()->findChildren<QLabel*>())
            if (l->isVisible())
                return l->text();
        return QString();
    };
    check("...and the panel says why (no reflected properties)",
          visibleHint().contains(QStringLiteral("reflects no properties")));

    // A properly registered component binds its schema and shows editors.
    selectEntity(QStringLiteral("WithReflected"));
    pump(20);
    check("a registered component shows its property rows",
          browser.propertyEditor()->visibleLeafPaths(false).contains(QStringLiteral("good")));
    check("...and no hint is shown for it", visibleHint().isEmpty());

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

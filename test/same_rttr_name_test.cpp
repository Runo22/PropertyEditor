// Reproduces: "the flecs path is correct, but resolveByName returns the FIRST
// registered type". That happens when two DIFFERENT C++ types are registered in RTTR
// under the SAME name (e.g. the namespace is stripped/replaced at registration), so
// the RTTR names carry no namespace and the correct full path has nothing to match
// against. Also proves the two ways out: an explicit alias, and the automatic
// C++-type-name alias registerType<T>() now records.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>
#include <rpe/gui/PropertyEditor.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QCoreApplication>
#include <QListWidget>
#include <QThread>

#include <cstdio>

namespace game
{
    struct Stats
    {
        int hp = 0;
    };
}
namespace ai
{
    struct Stats
    {
        double aggression = 0;
    };
}

RTTR_REGISTRATION
{
    // BOTH registered under the bare leaf "Stats" — the namespace is not in the RTTR
    // name. This is the configuration that breaks path-based resolution.
    rttr::registration::class_<game::Stats>("Stats").property("hp", &game::Stats::hp);
    rttr::registration::class_<ai::Stats>("Stats").property("aggression", &ai::Stats::aggression);
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

    rpe::TypeBridge::registerType<game::Stats>();
    rpe::TypeBridge::registerType<ai::Stats>();

    // Precondition: the RTTR names really are identical, so nothing in the name can
    // distinguish them.
    check("both types report the same RTTR name",
          rttr::type::get<game::Stats>().get_name().to_string()
              == rttr::type::get<ai::Stats>().get_name().to_string());

    // The correct, distinct flecs paths must still resolve to the right types — this
    // is what the automatic C++-type-name alias buys us.
    const rttr::type g = rpe::TypeBridge::resolveByName("game.Stats");
    const rttr::type a = rpe::TypeBridge::resolveByName("ai.Stats");
    check("\"game.Stats\" resolves to game::Stats", g == rttr::type::get<game::Stats>());
    check("\"ai.Stats\" resolves to ai::Stats", a == rttr::type::get<ai::Stats>());
    check("the two paths resolve to DIFFERENT types", g != a);

    // The "::" spelling of the same path must work identically.
    check("\"game::Stats\" resolves the same as \"game.Stats\"",
          rpe::TypeBridge::resolveByName("game::Stats") == rttr::type::get<game::Stats>());

    // An explicit alias remains available and authoritative.
    struct Other
    {
        int v = 0;
    };
    rpe::TypeBridge::registerType<Other>("weird.Legacy.Name");
    check("explicit alias resolves", rpe::TypeBridge::resolveByName("weird.Legacy.Name") == rttr::type::get<Other>());
    check("explicit alias is separator-insensitive",
          rpe::TypeBridge::resolveByName("weird::Legacy::Name") == rttr::type::get<Other>());

    // ── End to end: two entities whose same-RTTR-named components are DIFFERENT
    //    types must each bind their own schema when you switch between them. ──────
    {
        flecs::world world;
        world.entity("Hero").set<game::Stats>({ 77 });
        world.entity("Bot").set<ai::Stats>({ 9.5 });

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
        auto selectEntity = [&](const QString& name) {
            for (int i = 0; i < el->count(); ++i)
                if (el->item(i)->text() == name)
                    el->setCurrentRow(i);
        };
        auto leaves = [&] { return browser.propertyEditor()->visibleLeafPaths(false); };

        pump(25);
        selectEntity(QStringLiteral("Hero"));
        pump(25);
        check("Hero binds game::Stats (hp) despite the shared RTTR name",
              leaves().contains(QStringLiteral("hp")) && !leaves().contains(QStringLiteral("aggression")));

        selectEntity(QStringLiteral("Bot"));
        pump(25);
        check("Bot binds ai::Stats (aggression), NOT the first-registered type",
              leaves().contains(QStringLiteral("aggression")) && !leaves().contains(QStringLiteral("hp")));

        mirror.detach();
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

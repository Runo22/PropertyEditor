// A plugin is unloaded and loaded again while its component is selected and on
// screen — the cycle a host runs on every "start". RTTR frees a module's types
// when it unloads, and values read through its properties call into its code, so
// anything rpe still held of the plugin used to crash on the next touch (an
// is_wrapper / get_metadata on freed type data, a value destructor jumping into
// the unloaded DLL).
//
// TypeBridge::unregisterType is the release point: rpe drops everything of the
// type there, on the calling thread, while the plugin is still loaded — then the
// views rebind by name once the type is back.
//
// Two orders, both in mirror and direct mode:
//   • "blocked": the GUI runs NO events between unload and reload (the host's GUI
//     thread is blocked driving the reload);
//   • "idle":    the GUI keeps running while the plugin is gone, then it returns;
//   • "worker":  as "blocked", but the unload and the reload run on ANOTHER thread
//     while the GUI thread waits for it (a host that stops its sim thread and has
//     it reload the plugin) — the release must not need the GUI thread to run.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>
#include <rpe/ecs/PinnedPropertiesWidget.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QListWidget>
#include <QStyleOptionViewItem>
#include <QThread>
#include <QTreeView>
#include <QTreeWidget>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

// ── the plugin, loaded at run time ──────────────────────────────────────────────
struct Plugin
{
    using LoadFn = void (*)(flecs::world*, double);
    using UnloadFn = void (*)(flecs::world*);
    void* handle = nullptr;

    void load(flecs::world& w, double rpm)
    {
#if defined(_WIN32)
        handle = LoadLibraryA(RPE_RELOAD_PLUGIN_PATH);
        auto fn = reinterpret_cast<LoadFn>(GetProcAddress(static_cast<HMODULE>(handle), "reloadPluginLoad"));
#else
        handle = dlopen(RPE_RELOAD_PLUGIN_PATH, RTLD_NOW | RTLD_LOCAL);
        auto fn = reinterpret_cast<LoadFn>(dlsym(handle, "reloadPluginLoad"));
#endif
        fn(&w, rpm);
    }
    void unload(flecs::world& w)
    {
#if defined(_WIN32)
        reinterpret_cast<UnloadFn>(GetProcAddress(static_cast<HMODULE>(handle), "reloadPluginUnload"))(&w);
        FreeLibrary(static_cast<HMODULE>(handle));
#else
        reinterpret_cast<UnloadFn>(dlsym(handle, "reloadPluginUnload"))(&w);
        dlclose(handle);
#endif
        handle = nullptr;
    }
    static bool stillMapped()
    {
#if defined(_WIN32)
        return GetModuleHandleA(RPE_RELOAD_PLUGIN_PATH) != nullptr;
#else
        void* h = dlopen(RPE_RELOAD_PLUGIN_PATH, RTLD_NOW | RTLD_NOLOAD);
        if (h)
            dlclose(h);
        return h != nullptr;
#endif
    }
};

static QModelIndex findPath(QAbstractItemModel* m, const QString& p, const QModelIndex& parent = {})
{
    for (int r = 0; r < m->rowCount(parent); ++r)
    {
        const QModelIndex i = m->index(r, 0, parent);
        if (i.data(rpe::PropertyPathRole).toString() == p)
            return i;
        const QModelIndex sub = findPath(m, p, i);
        if (sub.isValid())
            return sub;
    }
    return {};
}

static QString shown(rpe::EntityComponentBrowser& b, const char* path)
{
    auto* m = b.propertyEditor()->view()->model();
    return findPath(m, QString::fromLatin1(path)).siblingAtColumn(1).data().toString();
}

static void runCycle(bool mirrorMode, bool guiBlocked, bool onWorker = false)
{
    const QString tag = QStringLiteral("%1/%2").arg(mirrorMode ? "mirror" : "direct",
                                                    onWorker ? "worker" : (guiBlocked ? "blocked" : "idle"));
    auto name = [&](const char* what) { return (tag + QStringLiteral(": ") + QLatin1String(what)).toStdString(); };
    printf("\n-- %s --\n", qPrintable(tag));

    flecs::world world;
    Plugin plugin;
    plugin.load(world, 900);

    rpe::EcsMirror mirror;
    rpe::EntityComponentBrowser browser;
    rpe::PinnedPropertiesWidget pins;
    if (mirrorMode)
    {
        mirror.attach(&world);
        browser.setMirror(&mirror);
        browser.setPinnedPropertiesWidget(&pins);
    }
    else
    {
        browser.setWorld(&world);
    }
    browser.setEditPolicy(rpe::EditPolicy::WriteBack);
    browser.resize(500, 600);
    browser.show();
    pins.show();

    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(4);
        }
    };
    auto waitFor = [&](const std::function<bool()>& cond) {
        for (int i = 0; i < 200 && !cond(); ++i)
            pump(1);
        return cond();
    };

    browser.selectEntity(world.lookup("Car"));
    pump(10);
    auto* cl = browser.componentList()->findChild<QListWidget*>();
    for (int i = 0; cl && i < cl->count(); ++i)
        if (cl->item(i)->text() == QStringLiteral("Engine"))
            cl->setCurrentRow(i);
    browser.propertyEditor()->view()->expandAll();
    check(name("the plugin's component is shown").c_str(),
          waitFor([&] { return shown(browser, "rpm").startsWith(QStringLiteral("900")); }));
    if (mirrorMode)
    {
        emit browser.propertyEditor()->pinRequested(QStringLiteral("rpm"));
        pump(10);
    }

    // An editor left open on a plugin value (its starting value is held).
    auto* view = browser.propertyEditor()->view();
    auto* dlg = view->itemDelegateForColumn(1);
    // Persistent, as a view holds it: a model reset invalidates it.
    const QPersistentModelIndex modeIdx(findPath(view->model(), QStringLiteral("mode")).siblingAtColumn(1));
    QWidget* openEditor = dlg->createEditor(view->viewport(), QStyleOptionViewItem(), modeIdx);
    if (openEditor)
        dlg->setEditorData(openEditor, modeIdx);

    // ── unload ───────────────────────────────────────────────────────────────────
    if (onWorker)
        std::thread([&] { plugin.unload(world); }).join(); // the GUI thread just waits
    else
        plugin.unload(world);
#if !defined(_WIN32)
    check(name("the plugin really left the process").c_str(), !Plugin::stillMapped());
#endif
    if (!guiBlocked)
    {
        pump(30); // the GUI runs with the plugin gone
        check(name("while it's gone, nothing of it is shown").c_str(), shown(browser, "rpm").isEmpty());
    }

    // The editor closes after the unload (focus moved to the host's button).
    // (A view commits only while the row exists; a reset closes the editor.)
    if (openEditor)
    {
        if (modeIdx.isValid())
            dlg->setModelData(openEditor, view->model(), modeIdx);
        dlg->destroyEditor(openEditor, modeIdx);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // ── reload ───────────────────────────────────────────────────────────────────
    if (onWorker)
        std::thread([&] { plugin.load(world, 1234); }).join();
    else
        plugin.load(world, 1234);
    check(name("after the reload the component shows again, with the new value").c_str(),
          waitFor([&] { return shown(browser, "rpm").startsWith(QStringLiteral("1234")); }));
    check(name("nested struct, enum, array and string rows are back").c_str(),
          waitFor([&] {
              return shown(browser, "inner.a").startsWith(QStringLiteral("1.5")) && shown(browser, "mode") == QStringLiteral("Run")
                  && !shown(browser, "gears.[2]").isEmpty() && shown(browser, "label") == QStringLiteral("v8");
          }));
    if (mirrorMode)
    {
        auto* tree = pins.findChild<QTreeWidget*>();
        check(name("the watch list shows the reloaded value").c_str(), waitFor([&] {
                  return tree && tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->text(2).startsWith(QStringLiteral("1234"));
              }));
    }

    // Edit the reloaded component through the grid: it lands in the new plugin.
    const QModelIndex rpmIdx = findPath(view->model(), QStringLiteral("rpm")).siblingAtColumn(1);
    view->model()->setData(rpmIdx, QVariant::fromValue(rttr::variant(4321.0)), Qt::EditRole);
    check(name("an edit after the reload reaches the reloaded component").c_str(),
          waitFor([&] { return shown(browser, "rpm").startsWith(QStringLiteral("4321")); }));

    // Second cycle, then tear down with the plugin unloaded (destruction order).
    plugin.unload(world);
    pump(5);
    plugin.load(world, 77);
    check(name("a second reload works too").c_str(),
          waitFor([&] { return shown(browser, "rpm").startsWith(QStringLiteral("77")); }));
    plugin.unload(world);
    if (mirrorMode)
        mirror.detach();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // An optional argument runs one cycle (0–4) — for debugging.
    const int only = argc > 1 ? atoi(argv[1]) : -1;
    const bool cycles[5][3] = {
        { true, true, false }, { true, false, false }, { false, true, false }, { false, false, false }, { true, true, true }
    };
    for (int i = 0; i < 5; ++i)
        if (only < 0 || only == i)
            runCycle(cycles[i][0], cycles[i][1], cycles[i][2]);

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

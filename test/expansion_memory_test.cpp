// Rows you collapse stay collapsed across entity switches — and aren't mirrored.
//
// Selecting a component called expandAll() every time, so a collapse lasted only
// until the next entity switch, and snapshotOpenFieldsOnly (mirror only what's
// open) never had anything to skip: everything was always open.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QThread>
#include <QTreeView>

#include <cstdio>

struct Vec3
{
    double x = 0, y = 0, z = 0;
};
// More than 4 leaves: a collapsed struct that small would still be watched, to
// render its "[a, b, c]" summary — this one isn't.
struct Tuning
{
    double a = 0, b = 0, c = 0, d = 0, e = 0;
};
struct Transform
{
    Vec3 pos;
    Tuning rot;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Vec3>("Vec3").property("x", &Vec3::x).property("y", &Vec3::y).property("z", &Vec3::z);
    rttr::registration::class_<Tuning>("Tuning")
        .property("a", &Tuning::a)
        .property("b", &Tuning::b)
        .property("c", &Tuning::c)
        .property("d", &Tuning::d)
        .property("e", &Tuning::e);
    rttr::registration::class_<Transform>("Transform").property("pos", &Transform::pos).property("rot", &Transform::rot);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

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

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    rpe::TypeBridge::registerType<Transform>();

    flecs::world w;
    w.entity("A").set<Transform>({ { 1, 2, 3 }, { 4, 5, 6, 7, 8 } });
    w.entity("B").set<Transform>({ { 7, 8, 9 }, { 1, 1, 1, 1, 1 } });

    rpe::EcsMirror mirror;
    mirror.attach(&w);
    mirror.setScanIntervalsMs(0, 0);
    rpe::EntityComponentBrowser browser;
    browser.setMirror(&mirror);
    browser.show();

    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            w.progress(0.033f);
            QCoreApplication::processEvents();
            QThread::msleep(3);
        }
    };
    auto* entities = browser.entityList()->findChild<QListWidget*>();
    auto select = [&](const char* name) {
        for (int i = 0; i < entities->count(); ++i)
            if (entities->item(i)->text() == QLatin1String(name))
                entities->setCurrentRow(i);
        pump(15);
    };
    auto* editor = browser.propertyEditor();
    auto* view = editor->view();
    auto expanded = [&](const char* path) {
        const QModelIndex i = findPath(view->model(), QString::fromLatin1(path));
        return i.isValid() && view->isExpanded(i);
    };
    auto watched = [&] { return editor->visibleLeafPaths(/*onlyExpanded=*/true); };

    pump(20);
    select("A");
    auto* comps = browser.componentList()->findChild<QListWidget*>();
    if (comps->count())
        comps->setCurrentRow(0);
    pump(15);

    check("first time: everything is open", expanded("pos") && expanded("rot"));
    check("...so rot's leaves are mirrored", watched().contains(QStringLiteral("rot.a")));

    // The user collapses rot (a per-row collapse, as a click does).
    view->collapse(findPath(view->model(), QStringLiteral("rot")));
    pump(5);
    check("collapsing rot stops mirroring its leaves", !watched().contains(QStringLiteral("rot.a")));

    select("B");
    check("switching entity keeps rot collapsed", !expanded("rot"));
    check("...and pos open", expanded("pos"));
    check("...and rot's leaves still NOT mirrored on the new entity", !watched().contains(QStringLiteral("rot.a")));
    check("...while pos's are", watched().contains(QStringLiteral("pos.x")));

    select("A");
    check("and back again: still collapsed", !expanded("rot"));

    // Re-opening it is remembered too.
    view->expand(findPath(view->model(), QStringLiteral("rot")));
    pump(5);
    select("B");
    check("an expand by hand is remembered as well", expanded("rot"));

    // Bulk operations don't count as the user's choice: a filter that collapses
    // everything, then clears, must not leave rows "remembered" as collapsed.
    editor->findChild<QLineEdit*>()->setText(QStringLiteral("x"));
    pump(3);
    editor->findChild<QLineEdit*>()->clear();
    pump(3);
    select("A");
    check("filtering doesn't record collapses", expanded("pos") && expanded("rot"));

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

// A value edit must land on the entity + component it was MADE on.
//
// Reported by review, reproduced: edit Hero's hp, then click Villain. The inline
// editor commits on focus-out — inside the very click that selects Villain — and
// the edit carried only a leaf PATH, so the sim applied it to whatever was
// selected when it pumped: Villain's hp became 99, Hero's stayed 1.
//
// Also pins the flip side: a value the sim read from the OLD target in a pump that
// raced the switch must not be shown on the new one (same leaf paths).
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QListWidget>
#include <QThread>

#include <cstdio>

struct Health
{
    int hp = 0;
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
    rpe::TypeBridge::registerType<Health>();

    // ── Browser: edit, then click another entity in the same GUI turn ──────────
    {
        flecs::world w;
        auto hero = w.entity("Hero").set<Health>({ 1 });
        auto villain = w.entity("Villain").set<Health>({ 2 });

        rpe::EcsMirror mirror;
        mirror.attach(&w);
        mirror.setScanIntervalsMs(0, 0);
        rpe::EntityComponentBrowser browser;
        browser.setMirror(&mirror);
        auto s = browser.settings();
        s.editPolicy = rpe::EditPolicy::WriteBack; // the reporter's config
        s.allowComponentEditing = true;
        browser.setSettings(s);
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
        };

        pump(20);
        select("Hero");
        pump(20);
        auto* comps = browser.componentList()->findChild<QListWidget*>();
        if (comps->count())
            comps->setCurrentRow(0);
        pump(20);

        auto* model = browser.propertyEditor()->model();
        const QModelIndex hp = findPath(model, QStringLiteral("hp")).siblingAtColumn(1);
        check("Hero's hp row is bound", hp.isValid());

        // Commit an edit and switch selection BEFORE the sim gets a pump in.
        model->setData(hp, QVariant::fromValue(rttr::variant(99)), Qt::EditRole);
        select("Villain");
        pump(10);

        check("the edit landed on Hero, where it was made", hero.get<Health>().hp == 99);
        check("Villain was NOT written", villain.get<Health>().hp == 2);

        // ── A stale value from the OLD target must not be shown on the new one ──
        // Simulate a pump that read Hero after the GUI had already moved to Villain.
        pump(10);
        std::vector<rpe::MirrorChannel::ValueUpdate> stale;
        stale.push_back({ QStringLiteral("hp"), rttr::variant(12345), static_cast<qulonglong>(hero.id()),
                          QStringLiteral("Health") });
        mirror.channel()->publishValues(std::move(stale));
        pump(3);
        const QString shown = findPath(model, QStringLiteral("hp")).siblingAtColumn(1).data(Qt::DisplayRole).toString();
        check("a value read from the previous entity is dropped", shown == QStringLiteral("2"));

        mirror.detach();
    }

    // ── Legacy EcsMirror::queueEdit(path, value): target fixed at QUEUE time ───
    {
        flecs::world w;
        auto a = w.entity("A").set<Health>({ 1 });
        auto b = w.entity("B").set<Health>({ 2 });
        rpe::EcsMirror mirror;
        mirror.attach(&w);
        mirror.setInterest(static_cast<qulonglong>(a.id()), QStringLiteral("Health"), { QStringLiteral("hp") });
        w.progress(0.016f); // build the listing for A
        mirror.queueEdit(QStringLiteral("hp"), rttr::variant(5));
        mirror.setInterest(static_cast<qulonglong>(b.id()), QStringLiteral("Health"), { QStringLiteral("hp") });
        w.progress(0.016f);
        check("legacy queueEdit writes the interest current when it was queued", a.get<Health>().hp == 5);
        check("...not the one current when the sim applies it", b.get<Health>().hp == 2);
        mirror.detach();
    }

    // ── An edit to a target that disappeared before the pump is dropped ────────
    {
        flecs::world w;
        auto gone = w.entity("Gone").set<Health>({ 1 });
        auto other = w.entity("Other").set<Health>({ 7 });
        const qulonglong goneId = static_cast<qulonglong>(gone.id());
        rpe::EcsMirror mirror;
        mirror.attach(&w);
        mirror.setInterest(static_cast<qulonglong>(other.id()), QStringLiteral("Health"), { QStringLiteral("hp") });
        mirror.channel()->queueEdit(goneId, QStringLiteral("Health"), QStringLiteral("hp"), rttr::variant(42));
        gone.destruct();
        w.progress(0.016f);
        check("an edit to a destroyed entity writes nothing anywhere", other.get<Health>().hp == 7);

        // Component removed rather than entity destroyed.
        auto stripped = w.entity("Stripped").set<Health>({ 3 });
        mirror.channel()->queueEdit(static_cast<qulonglong>(stripped.id()), QStringLiteral("Health"),
                                    QStringLiteral("hp"), rttr::variant(42));
        stripped.remove<Health>();
        w.progress(0.016f);
        check("an edit to a removed component is dropped (no crash, no re-add)", !stripped.has<Health>());
        check("...and still nothing leaked onto the selection", other.get<Health>().hp == 7);
        mirror.detach();
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

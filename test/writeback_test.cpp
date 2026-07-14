// Proves edits genuinely reach the flecs world through BOTH write-back paths:
//   1. Direct WriteBack — PropertyModel::_applyEdit writes via the instance
//      provider under the write guard (synchronously, on commit).
//   2. Mirror — the edit is queued to the channel and applied on the sim thread
//      during pump() (EcsMirror::_pumpImpl → bridge::setValueByPath on get_mut).
// Also checks the LocalEdit policy does NOT touch the world.
#include <rpe/core/RttrVariantWrapper.h>
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>

#include <cstdio>

struct Comp
{
    double x = 1.0;
    int n = 7;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Comp>("Comp").property("x", &Comp::x).property("n", &Comp::n);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

// The live component as a plain pointer (same call the browser/mirror use).
static Comp* live(flecs::world& w, flecs::entity e)
{
    return static_cast<Comp*>(e.get_mut(w.component<Comp>()));
}

// Value-column index of a top-level leaf by path.
static QModelIndex leaf(rpe::PropertyModel& m, const QString& path)
{
    for (int r = 0; r < m.rowCount({}); ++r)
        if (m.index(r, 0, {}).data(rpe::PropertyPathRole).toString() == path)
            return m.index(r, 1, {});
    return {};
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    rpe::TypeBridge::registerType<Comp>();

    // ── 1. Direct WriteBack: the model writes straight into the world ──────────
    {
        flecs::world w;
        w.component<Comp>();
        flecs::entity e = w.entity("E").set<Comp>({ 1.0, 7 });

        rpe::RttrVariantWrapper wrap;
        rpe::PropertyModel model;
        model.bindType(rttr::type::get<Comp>());
        model.setEditPolicy(rpe::EditPolicy::WriteBack);
        model.setInstanceProvider([&]() -> rttr::instance {
            wrap = rpe::RttrVariantWrapper::makeLinked(rttr::type::get<Comp>(), live(w, e));
            return wrap.instance();
        });
        { // seed the display
            auto seed = rpe::RttrVariantWrapper::makeLinked(rttr::type::get<Comp>(), live(w, e));
            model.refresh(seed.instance());
        }

        const bool ok = model.setData(leaf(model, QStringLiteral("x")), QVariant::fromValue(rttr::variant(2.5)), Qt::EditRole);
        check("WriteBack: setData accepted", ok);
        check("WriteBack: the flecs world was actually mutated (x == 2.5)", live(w, e)->x == 2.5);

        model.setData(leaf(model, QStringLiteral("n")), QVariant::fromValue(rttr::variant(42)), Qt::EditRole);
        check("WriteBack: a second field writes too (n == 42)", live(w, e)->n == 42);
    }

    // ── 2. LocalEdit policy must NOT write to the world ──────────────────────────
    {
        flecs::world w;
        w.component<Comp>();
        flecs::entity e = w.entity("E").set<Comp>({ 1.0, 7 });

        rpe::RttrVariantWrapper wrap;
        rpe::PropertyModel model;
        model.bindType(rttr::type::get<Comp>());
        model.setEditPolicy(rpe::EditPolicy::LocalEdit);
        model.setInstanceProvider([&]() -> rttr::instance {
            wrap = rpe::RttrVariantWrapper::makeLinked(rttr::type::get<Comp>(), live(w, e));
            return wrap.instance();
        });
        { auto seed = rpe::RttrVariantWrapper::makeLinked(rttr::type::get<Comp>(), live(w, e)); model.refresh(seed.instance()); }

        model.setData(leaf(model, QStringLiteral("x")), QVariant::fromValue(rttr::variant(9.0)), Qt::EditRole);
        check("LocalEdit: world is left untouched (x still 1.0)", live(w, e)->x == 1.0);
    }

    // ── 3. Mirror: queued edit is applied to the world on pump() ───────────────
    {
        flecs::world w;
        w.component<Comp>();
        flecs::entity e = w.entity("E").set<Comp>({ 1.0, 7 });
        const auto eid = static_cast<qulonglong>(e.id());

        rpe::EcsMirror mirror;
        mirror.attach(&w, rpe::EcsMirror::PumpMode::Manual);
        mirror.setInterest(eid, QStringLiteral("Comp"), { QStringLiteral("x") });
        mirror.queueEdit(QStringLiteral("x"), rttr::variant(5.0));

        for (int i = 0; i < 10; ++i)
        {
            w.progress(0.016f);
            mirror.pump();          // sim thread: applies the queued edit via get_mut
            mirror.pollValues();    // drain the GUI-bound updates
        }
        check("Mirror: queued edit reached the world (x == 5.0)", live(w, e)->x == 5.0);
        mirror.detach();
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

// Plugin-unload safety: when a plugin unregisters its bridged type (as it must in
// its unload path), the mirror must NOT crash and must stop touching the type —
// every value read is gated by TypeBridge::wrap (invalid once unregistered), and
// the selected-component listing + pin cache re-resolve on the registry generation
// bump. Re-registering (plugin reload) brings the type back.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <QApplication>

#include <cstdio>

struct Health // stands in for a plugin-registered component type
{
    int hp = 100;
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
using PinKey = rpe::MirrorChannel::PinKey;

// Poll the freshest component rows (caller forces a resync+pump first so they are
// republished) and report whether `name` is a selectable DATA row.
static bool hasDataRow(rpe::MirrorChannel* ch, const QString& name)
{
    QVector<Row> rows;
    ch->pollComponentRows(rows);
    for (const auto& r : rows)
        if (r.name == name && r.kind == Kind::Data)
            return true;
    return false;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::registerType<Health>(); // "plugin loaded"

    flecs::world world;
    auto e = world.entity("E").set<Health>({ 70 });

    rpe::EcsMirror mirror;
    mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);
    mirror.setScanIntervalsMs(0, 0);
    auto ch = mirror.channel();
    mirror.setInterest(static_cast<qulonglong>(e.id()), "Health", { "hp" });

    auto pump = [&] { world.progress(0.016f); mirror.pump(); };
    pump();

    // Baseline: Health is a selectable data component and its value flows.
    check("Health listed as a data component while registered", hasDataRow(ch.get(), QStringLiteral("Health")));
    {
        bool gotHp = false;
        for (const auto& u : ch->pollValues())
            gotHp |= (u.path == QStringLiteral("hp") && u.value.to_int64() == 70);
        check("Health value flows while registered", gotHp);
    }

    // Pin it too, so we cover the pin path.
    PinKey pk;
    pk.entity = static_cast<qulonglong>(e.id());
    pk.component = QStringLiteral("Health");
    pk.path = QStringLiteral("hp");
    mirror.setPins({ pk });
    pump();
    check("pinned Health value flows while registered", !mirror.pollPinValues().empty());

    // ── "Plugin unloads": unregister the bridge entry (RTTR keeps the type; the
    //    bridge wrap hook — the pointer into the now-gone module — is removed). The
    //    component may also leave the world, but the bridge gate alone must suffice. ──
    rpe::TypeBridge::unregisterType<Health>();
    ch->requestResync();
    pump();
    pump(); // a couple of pumps: generation bump rebuilds the listing + pin cache

    check("wrap returns invalid for the unregistered type (no dangling call)",
          !rpe::TypeBridge::wrap(rttr::type::get<Health>(), &e).is_valid());
    check("Health is no longer a selectable data component", !hasDataRow(ch.get(), QStringLiteral("Health")));
    {
        // Change the value on the sim side; nothing must be published for the gone
        // type (and, crucially, no crash reading it).
        e.set<Health>({ 5 });
        pump();
        bool anyHp = false;
        for (const auto& u : ch->pollValues())
            anyHp |= (u.path == QStringLiteral("hp"));
        check("no value published for the unregistered type", !anyHp);
        check("no pin value published for the unregistered type", mirror.pollPinValues().empty());
    }

    // ── "Plugin reloads": re-register → the type comes back, values resume. ──
    rpe::TypeBridge::registerType<Health>();
    ch->requestResync();
    e.set<Health>({ 42 });
    pump();
    pump();
    check("Health is a data component again after re-register", hasDataRow(ch.get(), QStringLiteral("Health")));
    {
        bool gotHp = false;
        for (const auto& u : ch->pollValues())
            gotHp |= (u.path == QStringLiteral("hp") && u.value.to_int64() == 42);
        check("selected value resumes after reload", gotHp);
    }
    {
        // Pin re-resolves after the generation bump and resumes.
        e.set<Health>({ 43 });
        pump();
        bool gotPin = false;
        for (const auto& p : mirror.pollPinValues())
            gotPin |= (p.value.to_int64() == 43);
        check("pinned value resumes after reload", gotPin);
    }

    mirror.detach();
    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

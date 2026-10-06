// On-demand health checks (rpe/ecs/HealthCheck.h) — each check triggered by the
// situation it exists for, plus the false positive the old automatic
// "has an RTTR type but no TypeBridge registration" qWarning produced.
#include <rpe/core/OptionalSupport.h>
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/HealthCheck.h>

#include <rttr/registration.h>

#include <QCoreApplication>

#include <cstdio>
#include <memory>
#include <optional>

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
        double aggr = 0;
    };
}
struct Mystery // a component whose flecs name ("render::Stats") binds the wrong type
{
    int v = 0;
};
namespace plug
{
    struct RttrOnly
    {
        int v = 0;
    };
}
struct Gizmo // RTTR knows it, unbridged, and a component with the same SHORT name exists
{
    int v = 0;
};
struct Transform // bridged, used by "Transform" — and "physics.Transform" is a DIFFERENT type
{
    double x = 0;
};
struct PhysicsTransform // unbridged; its flecs name "physics::Transform" shares Transform's leaf
{
    float x = 0;
};
struct Settings // RTTR-registered for some OTHER purpose, never meant to be a component
{
    int v = 0;
};
struct AudioSettings // an unbridged engine component that happens to be called "Settings"
{
    int volume = 0;
    int rate = 0;
};
struct Glow // bridged, reached only by its short name from "fx.Glow"
{
    int v = 0;
};
namespace a
{
    struct Dup
    {
        int v = 0;
    };
}
namespace b
{
    struct Dup
    {
        int w = 0;
    };
}
struct Unused
{
    int v = 0;
};
namespace plug
{
    struct UsedRttrOnly // RTTR-registered, unbridged, and CARRIED by an entity
    {
        int v = 0;
    };
}
// Value types: known to RTTR as FIELDS of a bridged component, and to flecs as
// components for its own reasons — the reported false positives.
struct Vec3
{
    double x = 0, y = 0, z = 0;
};
struct Body
{
    Vec3 pos;
    std::optional<int> tag;
};
namespace audio
{
    class Marker // move-only, no RTTR registration at all
    {
    public:
        Marker() = default;
        Marker(Marker&&) noexcept = default;
        Marker& operator=(Marker&&) noexcept = default;
        Marker(const Marker&) = delete;
        Marker& operator=(const Marker&) = delete;
        std::unique_ptr<int> res;
    };
}

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<game::Stats>("game::Stats").property("hp", &game::Stats::hp);
    registration::class_<ai::Stats>("ai::Stats").property("aggr", &ai::Stats::aggr);
    registration::class_<plug::RttrOnly>("plug::RttrOnly").property("v", &plug::RttrOnly::v);
    registration::class_<Gizmo>("Gizmo").property("v", &Gizmo::v);
    registration::class_<Transform>("Transform").property("x", &Transform::x);
    registration::class_<Glow>("Glow").property("v", &Glow::v);
    registration::class_<a::Dup>("Dup").property("v", &a::Dup::v);
    registration::class_<b::Dup>("Dup").property("w", &b::Dup::w);
    registration::class_<Unused>("Unused").property("v", &Unused::v);
    registration::class_<Settings>("Settings").property("v", &Settings::v);
    registration::class_<plug::UsedRttrOnly>("plug::UsedRttrOnly").property("v", &plug::UsedRttrOnly::v);
    registration::class_<Vec3>("Vec3").property("x", &Vec3::x).property("y", &Vec3::y).property("z", &Vec3::z);
    registration::class_<Body>("Body").property("pos", &Body::pos).property("tag", &Body::tag);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static bool has(const rpe::HealthReport& r, const char* check, const char* subject)
{
    for (const rpe::HealthIssue& i : r.byCheck(QString::fromLatin1(check)))
        if (i.subject == QString::fromLatin1(subject))
            return true;
    return false;
}

static int g_warnings = 0;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType t, const QMessageLogContext&, const QString& msg) {
        if (t == QtWarningMsg && msg.contains(QStringLiteral("TypeBridge")))
            ++g_warnings;
    });

    rpe::TypeBridge::registerTypes<game::Stats, ai::Stats, Transform, Glow, a::Dup, b::Dup, Unused>();
    rpe::TypeBridge::registerType<audio::Marker>(); // move-only, no RTTR registration
    rpe::TypeBridge::registerType<Body>();
    RPE_REGISTER_OPTIONAL(int);

    flecs::world w;
    w.component<game::Stats>("game::Stats");
    w.component<ai::Stats>("ai::Stats");
    w.component<Mystery>("render::Stats"); // unbridged → binds a same-leaf type
    w.component<plug::RttrOnly>("plug::RttrOnly");
    w.component<Gizmo>("plug::Gizmo"); // RTTR's "Gizmo" is unbridged
    w.component<Transform>("Transform");
    w.component<PhysicsTransform>("physics::Transform");
    w.component<Glow>("fx::Glow");
    w.component<audio::Marker>("audio::Marker");
    w.component<AudioSettings>("audio::Settings");
    w.entity("Speaker").set<AudioSettings>({}).set<Gizmo>({}); // short-name hints only apply to USED components
    w.component<plug::UsedRttrOnly>("plug::UsedRttrOnly");
    w.entity("Carrier").set<plug::UsedRttrOnly>({ 1 });
    w.component<Body>("Body");
    w.component<Vec3>("Vec3");            // flecs knows the value types too…
    w.component<std::optional<int>>();    // …as "std.optional<int>"
    w.entity("B").set<Body>({});
    w.component<a::Dup>("a::Dup");
    w.component<b::Dup>("b::Dup");

    // ── Registry (no world) ─────────────────────────────────────────────────────
    printf("\n-- registry --\n");
    const rpe::HealthReport reg = rpe::checkTypeRegistry();
    check("two bridged types under one RTTR name are reported", has(reg, "duplicate-rttr-name", "Dup"));
    check("a bridged type with no properties is noted (a marker)", !reg.byCheck(QStringLiteral("no-properties")).isEmpty());
    check("...as Info, not a warning", reg.byCheck(QStringLiteral("no-properties")).first().severity
                                         == rpe::HealthIssue::Severity::Info);

    // ── Components ──────────────────────────────────────────────────────────────
    printf("\n-- components --\n");
    const rpe::HealthReport comps = rpe::checkComponents(w);
    check("a component binding a type of another SIZE is an error", has(comps, "size-mismatch", "render.Stats"));
    check("...flagged as Error", comps.about(QStringLiteral("render.Stats")).first().severity
                                     == rpe::HealthIssue::Severity::Error);
    check("an ambiguous short-name binding is reported", has(comps, "ambiguous-name", "render.Stats"));
    check("one type bound by two components is reported", has(comps, "same-type-twice", "ai::Stats"));
    {
        flecs::world idle;
        idle.component<Gizmo>("plug::Gizmo"); // same short-name hint — but nothing uses it
        check("a short-name hint for a component NO entity uses stays silent",
              rpe::checkComponents(idle).about(QStringLiteral("plug.Gizmo")).isEmpty());
    }
    check("RTTR-registered, unbridged, CARRIED by an entity → Warning",
          has(comps, "unbridged-rttr-type", "plug.UsedRttrOnly")
              && comps.about(QStringLiteral("plug.UsedRttrOnly")).first().severity == rpe::HealthIssue::Severity::Warning);
    check("RTTR-registered, unbridged, on NO entity yet → only Info",
          has(comps, "unbridged-rttr-type", "plug.RttrOnly")
              && comps.about(QStringLiteral("plug.RttrOnly")).first().severity == rpe::HealthIssue::Severity::Info);
    check("a VALUE type of a bridged component (Vec3 in Body::pos) is not reported",
          comps.about(QStringLiteral("Vec3")).isEmpty());
    check("std::optional<int> (a value type, and std::) is not reported",
          comps.about(QStringLiteral("std.optional<int>")).isEmpty());
    check("an unbridged same-SHORT-name RTTR type → only a hint", has(comps, "unbridged-maybe", "plug.Gizmo"));
    // The old automatic qWarning's false positive: an unbridged engine component that
    // merely SHARES A SHORT NAME with some RTTR type ("audio.Settings" vs an unrelated
    // RTTR "Settings") was reported as a missing registration. Now it's only a hint.
    check("an unrelated same-short-name RTTR type is NOT a warning (the old false positive)",
          comps.about(QStringLiteral("audio.Settings")).size() == 1
              && comps.about(QStringLiteral("audio.Settings")).first().severity == rpe::HealthIssue::Severity::Info);
    // And the opposite case, which the old warning never saw: a component that DOES
    // bind — but to the wrong type, through its short name.
    check("a component bound to a same-leaf type of another size is caught (physics.Transform)",
          has(comps, "size-mismatch", "physics.Transform"));
    check("...without a contradicting \"works today\" note", !has(comps, "short-name-only", "physics.Transform"));
    check("a binding reached only through the short name is noted", has(comps, "short-name-only", "fx.Glow"));
    check("a bridged type no component uses is noted", has(comps, "unused-bridge", "Unused"));
    check("an exactly bound component has nothing to report", comps.about(QStringLiteral("game.Stats")).isEmpty());
    check("a move-only marker component binds cleanly", comps.about(QStringLiteral("audio.Marker")).isEmpty());

    // ── Required component ──────────────────────────────────────────────────────
    printf("\n-- required component --\n");
    check("full path: clean", rpe::checkRequiredComponent(w, QStringLiteral("game::Stats")).isClean());
    check("unknown: not found", !rpe::checkRequiredComponent(w, QStringLiteral("physics::Stats"))
                                     .byCheck(QStringLiteral("required-not-found"))
                                     .isEmpty());
    check("bare leaf matching three: ambiguous", !rpe::checkRequiredComponent(w, QStringLiteral("Stats"))
                                                      .byCheck(QStringLiteral("required-ambiguous"))
                                                      .isEmpty());
    check("unique leaf: works, noted as inexact", !rpe::checkRequiredComponent(w, QStringLiteral("Glow"))
                                                       .byCheck(QStringLiteral("required-inexact"))
                                                       .isEmpty());

    // ── Prefab group tags ───────────────────────────────────────────────────────
    printf("\n-- prefab groups --\n");
    {
        flecs::world pw;
        auto enemy = pw.entity("game::npc::Enemy");
        auto prop = pw.entity("game::Prop");
        pw.prefab("Goblin").add(enemy);
        pw.entity("Crate").add(prop); // on an INSTANCE, not a prefab
        pw.prefab("Barrel");
        const rpe::HealthReport pg = rpe::checkPrefabGroups(
            pw, { QStringLiteral("game::npc::Enemy"), QStringLiteral("game.npc.Enemy"), QStringLiteral("game::Prop") });
        check("a resolvable tag on a prefab is clean", pg.about(QStringLiteral("game::npc::Enemy")).isEmpty());
        check("a dotted tag name is not found, and says why",
              has(pg, "group-tag-not-found", "game.npc.Enemy")
                  && pg.about(QStringLiteral("game.npc.Enemy")).first().message.contains(QStringLiteral("dotted")));
        check("a tag only on instances is reported, naming that",
              has(pg, "group-tag-on-no-prefab", "game::Prop")
                  && pg.about(QStringLiteral("game::Prop")).first().message.contains(QStringLiteral("non-prefab")));
    }

    // ── flecs build, the aggregate, the text ────────────────────────────────────
    printf("\n-- aggregate --\n");
    check("this process runs the flecs it was compiled against",
          rpe::checkFlecsBuild().byCheck(QStringLiteral("flecs-version-mismatch")).isEmpty());
    const rpe::HealthReport all = rpe::checkHealth(w, { QStringLiteral("Stats"), { QStringLiteral("nope.tag") } });
    check("checkHealth gathers every check", all.hasErrors() && has(all, "required-ambiguous", "Stats")
                                                 && has(all, "group-tag-not-found", "nope.tag")
                                                 && has(all, "duplicate-rttr-name", "Dup"));
    const QString text = all.toText(rpe::HealthIssue::Severity::Warning);
    check("toText lists errors first, with a fix line",
          text.startsWith(QStringLiteral("[ERROR]")) && text.contains(QStringLiteral("fix: ")));
    check("toText(Warning) leaves the Info items out", !text.contains(QStringLiteral("[INFO]")));
    {
        flecs::world clean;
        clean.component<game::Stats>("game::Stats");
        check("a clean world reports no warnings",
              rpe::checkComponents(clean).toText(rpe::HealthIssue::Severity::Warning) == QStringLiteral("No issues."));
    }

    check("NOTHING was logged by itself — checks are on demand only", g_warnings == 0);

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

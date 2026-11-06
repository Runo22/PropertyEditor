// The required-component filter must select the component the host NAMED.
//
// Reproduced: with game::Stats and ai::Stats both registered, requiring
// "game::Stats" listed the entity carrying ai::Stats — the match was on the leaf
// alone, and whichever same-named component came last won. A dotted spelling
// ("game.Stats") matched nothing at all.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>

#include <rttr/registration.h>

#include <QCoreApplication>

#include <cstdio>

namespace game
{
    struct Stats
    {
        int hp = 0;
    };
    namespace npc
    {
        struct Enemy
        {
            int threat = 0;
        };
    }
}
namespace ai
{
    struct Stats
    {
        int aggr = 0;
    };
}
struct Unique
{
    int v = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<game::Stats>("game::Stats").property("hp", &game::Stats::hp);
    rttr::registration::class_<ai::Stats>("ai::Stats").property("aggr", &ai::Stats::aggr);
    rttr::registration::class_<game::npc::Enemy>("game::npc::Enemy").property("threat", &game::npc::Enemy::threat);
    rttr::registration::class_<Unique>("Unique").property("v", &Unique::v);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static int g_warnings = 0;
static QString g_lastWarning;

struct World
{
    flecs::world w;
    World()
    {
        w.component<game::Stats>("game::Stats");
        w.component<ai::Stats>("ai::Stats");
        w.component<game::npc::Enemy>("game::npc::Enemy");
        w.entity("Hero").set<game::Stats>({ 1 });
        w.entity("Bot").set<ai::Stats>({ 2 });
        w.entity("Orc").set<game::npc::Enemy>({ 3 });
    }
};

// Entities the mirror lists for `required`, as a sorted, comma-joined string.
static QString listedFor(const char* required, World& fx)
{
    g_warnings = 0;
    g_lastWarning.clear();
    rpe::EcsMirror m;
    m.attach(&fx.w);
    m.setScanIntervalsMs(0, 0);
    m.setRequiredComponent(QString::fromLatin1(required));
    for (int i = 0; i < 10; ++i)
        fx.w.progress(0.016f);
    QVector<rpe::MirrorChannel::EntityEntry> e;
    m.channel()->pollEntities(e);
    QStringList names;
    for (const auto& x : e)
        names << x.label;
    names.sort();
    m.detach();
    return names.join(QStringLiteral(","));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    rpe::TypeBridge::registerTypes<game::Stats, ai::Stats, game::npc::Enemy, Unique>();
    qInstallMessageHandler([](QtMsgType t, const QMessageLogContext&, const QString& msg) {
        if (t == QtWarningMsg && msg.contains(QStringLiteral("required component")))
        {
            ++g_warnings;
            g_lastWarning = msg;
        }
    });

    World fx;
    check("\"game::Stats\" selects game::Stats — not the same-named ai::Stats",
          listedFor("game::Stats", fx) == QStringLiteral("Hero"));
    check("\"ai::Stats\" selects ai::Stats", listedFor("ai::Stats", fx) == QStringLiteral("Bot"));
    check("a dotted path works the same (\"game.Stats\")", listedFor("game.Stats", fx) == QStringLiteral("Hero"));
    check("a leading root separator is tolerated (\"::game::Stats\")",
          listedFor("::game::Stats", fx) == QStringLiteral("Hero"));

    check("a scope-aligned suffix resolves (\"npc::Enemy\" → game.npc.Enemy)",
          listedFor("npc::Enemy", fx) == QStringLiteral("Orc"));
    check("a leaf matching exactly one component still works (\"Enemy\")",
          listedFor("Enemy", fx) == QStringLiteral("Orc"));

    // An AMBIGUOUS leaf: deterministic, and said out loud.
    const QString amb = listedFor("Stats", fx);
    check("an ambiguous leaf picks deterministically (shortest path: ai.Stats)", amb == QStringLiteral("Bot"));
    check("...and warns, naming the candidates",
          g_warnings == 1 && g_lastWarning.contains(QStringLiteral("game.Stats"))
              && g_lastWarning.contains(QStringLiteral("ai.Stats")));

    // A SCOPED name that doesn't exist must never fall back to another namespace.
    check("\"physics::Stats\" (no such scope) lists nothing — no leaf fallback",
          listedFor("physics::Stats", fx).isEmpty());
    check("...and warns that it wasn't found", g_warnings == 1 && g_lastWarning.contains(QStringLiteral("not found")));

    // Registered AFTER the filter was set (plugin load order): the query must follow.
    {
        flecs::world w;
        w.entity("Early").set<game::Stats>({ 1 });
        rpe::EcsMirror m;
        m.attach(&w);
        m.setScanIntervalsMs(0, 0);
        m.setRequiredComponent(QStringLiteral("Unique"));
        for (int i = 0; i < 5; ++i)
            w.progress(0.016f);
        QVector<rpe::MirrorChannel::EntityEntry> e;
        m.channel()->pollEntities(e);
        check("before the required component exists, nothing is listed", e.isEmpty());

        w.entity("Late").set<Unique>({ 7 }); // registers Unique now
        for (int i = 0; i < 5; ++i)
            w.progress(0.016f);
        e.clear();
        m.channel()->pollEntities(e);
        check("once it is registered, its carriers are listed",
              e.size() == 1 && e.first().label == QStringLiteral("Late"));
        m.detach();
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

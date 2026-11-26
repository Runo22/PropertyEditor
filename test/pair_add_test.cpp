// Adding PAIRS from the browser, end to end:
//   • which relationships the add catalog offers, and how (pair only / also by
//     itself), with which suggested targets — flecs traits (Relationship, Target,
//     OneOf, Exclusive), usage in the world, and Settings::pairRelations;
//   • flecs' trait rules checked before adding (a debug flecs aborts on them),
//     stale ids, typed target paths;
//   • the mirror applying add-pair edits on the sim thread;
//   • pairs on targets with the same leaf name kept apart (key by target path);
//   • the picker: a Relationships branch, the target step, live refresh, the
//     keyboard, in mirror and direct mode.
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/ComponentListWidget.h>
#include <rpe/ecs/ComponentScan.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/EntityListWidget.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <cstdio>

struct Health
{
    int hp = 100;
};
struct Damage // only ever used as (Damage, target)
{
    int amount = 10;
};
struct Position // used by itself AND as (Position, target)
{
    float x = 0.f;
    float y = 0.f;
};
struct Owner // named in pairRelations, also used by itself
{
    int id = 0;
};
struct Frozen // a plain tag
{
};
struct Likes // Relationship trait
{
};
struct Team // Exclusive
{
};
struct Marker // Target trait
{
};
struct Visits // used with many targets
{
};
struct LeftSide // a tag used as a TARGET: (Damage, Left), data on the relationship
{
};
enum class Mood // its constants become entities (flecs meta)
{
    Calm,
    Angry
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Health>("Health").property("hp", &Health::hp);
    rttr::registration::class_<Damage>("Damage").property("amount", &Damage::amount);
    rttr::registration::class_<Position>("Position").property("x", &Position::x).property("y", &Position::y);
    rttr::registration::class_<Owner>("Owner").property("id", &Owner::id);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

using Entry = rpe::MirrorChannel::CatalogEntry;
using Use = rpe::MirrorChannel::PairUse;

static const Entry* findEntry(const QVector<Entry>& cat, const char* path)
{
    for (const Entry& e : cat)
        if (e.path == QString::fromLatin1(path))
            return &e;
    return nullptr;
}

static QStringList targetPaths(const Entry* e)
{
    QStringList out;
    if (e)
        for (const auto& t : e->targets)
            out << t.path;
    return out;
}

// The scene every section starts from.
struct Scene
{
    flecs::world world;
    flecs::entity element, elFire, elIce, scopeA, scopeB, aFire, bFire, red, blue, worldEnt, follows, eats, apples;
    flecs::entity hero, other;
    flecs::query<Health> healthQuery; // a NAMED query: an entity too

    Scene()
    {
        world.component<Health>();
        world.component<Damage>();
        world.component<Position>();
        world.component<Owner>();
        world.component<Frozen>();
        world.component<Likes>().add(flecs::Relationship);
        world.component<Team>().add(flecs::Exclusive);
        world.component<Marker>().add(flecs::Target);
        world.component<Visits>();
        world.component<Mood>("Mood");
        world.component<std::string>(); // what reflecting a std::string member registers
        // Named children of an UNNAMED entity — "#<id>.allocators", like the fields
        // of an anonymous struct some flecs addons describe.
        const flecs::entity anon = world.entity();
        world.entity("allocators").child_of(anon);
        world.entity("collection_time").child_of(anon);
        healthQuery = world.query_builder<Health>("HealthQuery").build();
        world.system<Health>("HealthSystem").each([](Health&) {});

        element = world.entity("Element").add(flecs::OneOf);
        elFire = world.entity("Fire").child_of(element);
        elIce = world.entity("Ice").child_of(element);
        scopeA = world.entity("a");
        scopeB = world.entity("b");
        aFire = world.entity("Fire").child_of(scopeA);
        bFire = world.entity("Fire").child_of(scopeB);
        red = world.entity("Red");
        blue = world.entity("Blue");
        worldEnt = world.entity("World");
        follows = world.entity("Follows"); // a plain entity, named in pairRelations
        eats = world.entity("Eats");       // a plain entity used as a relation, NOT named
        apples = world.entity("Apples");

        hero = world.entity("Hero").set<Health>({ 70 });
        hero.set<Damage>(aFire, { 10 });
        hero.add<Team>(blue);
        hero.add(eats, apples);
        other = world.entity("Other").set<Position>({ 1.f, 2.f });
        other.set<Position>(worldEnt, { 3.f, 4.f });
        other.add<Team>(red);
        other.set<Owner>({ 7 });
        other.add(element, elFire);
        other.add<Likes>(world.component<std::vector<Position>>()); // a target with "<….…>" in its name
        world.component<LeftSide>("Left");
        other.set<Damage, LeftSide>({ 5 }); // (Damage, Left): the data is the relationship's
    }
};

static rpe::AddCatalogOptions declared()
{
    rpe::AddCatalogOptions o;
    o.pairRelations = QStringList { QStringLiteral("Follows"), QStringLiteral("Owner"), QStringLiteral("Element") };
    return o;
}

// ── 1. The catalog ──────────────────────────────────────────────────────────────
static void testCatalog()
{
    printf("\n-- catalog --\n");
    rpe::TypeBridge::registerTypes<Health, Damage, Position, Owner>();
    Scene s;
    const QVector<Entry> cat = rpe::buildAddCatalog(s.world, declared());

    const Entry* health = findEntry(cat, "Health");
    check("a plain data component is offered by itself only", health && health->pair == Use::None && !health->tag);
    const Entry* frozen = findEntry(cat, "Frozen");
    check("a plain tag is offered by itself only", frozen && frozen->tag && frozen->pair == Use::None);

    const Entry* damage = findEntry(cat, "Damage");
    check("a component used only in pairs is offered both ways (still addable by itself)", damage && damage->pair == Use::Also);
    check("...suggesting the targets it's used with (an entity, a tag), by full path",
          targetPaths(damage) == (QStringList { "a.Fire", "Left" }));
    check("...with ids carrying the generation", damage && damage->targets.size() == 2
                                                     && damage->targets[0].id == static_cast<qulonglong>(s.aFire.id()));
    check("...it carries data itself (its targets are labels)", damage && damage->carriesData);

    const Entry* position = findEntry(cat, "Position");
    check("a component used by itself AND in pairs is offered both ways", position && position->pair == Use::Also);
    check("...suggesting its pair target", targetPaths(position) == QStringList { "World" });

    const Entry* likes = findEntry(cat, "Likes");
    check("the Relationship trait makes it a relationship only", likes && likes->pair == Use::Only && likes->tag);
    check("...suggesting the target it's used with, a template name kept whole",
          targetPaths(likes) == QStringList { QStringLiteral("std.vector<Position>") } && !likes->targetsFixed);

    const Entry* team = findEntry(cat, "Team");
    check("an Exclusive relationship is flagged", team && team->exclusive);
    check("a TAG relationship is pair only (by itself it means nothing)", team && team->pair == Use::Only);
    check("...targets in use, sorted", targetPaths(team) == (QStringList { "Blue", "Red" }));

    check("an entity with the Target trait is never offered", !findEntry(cat, "Marker"));

    const Entry* element = findEntry(cat, "Element");
    check("a named plain entity with OneOf is a relationship", element && element->pair == Use::Only && element->tag);
    check("...whose targets are fixed to its children", element && element->targetsFixed
                                                            && targetPaths(element) == (QStringList { "Element.Fire", "Element.Ice" }));

    const Entry* follows = findEntry(cat, "Follows");
    check("a named plain entity nobody pairs yet is a relationship only", follows && follows->pair == Use::Only);
    const Entry* owner = findEntry(cat, "Owner");
    check("a named component also used by itself is offered both ways", owner && owner->pair == Use::Also);

    check("a plain entity used as a relation but not named stays out", !findEntry(cat, "Eats"));
    check("flecs' own relationships stay out", !findEntry(cat, "flecs.core.ChildOf") && !findEntry(cat, "ChildOf"));

    // Without the host's names: only what the traits and the world say.
    const QVector<Entry> bare = rpe::buildAddCatalog(s.world);
    check("unnamed, a plain unused entity isn't offered", !findEntry(bare, "Follows"));
    const Entry* ownerBare = findEntry(bare, "Owner");
    check("unnamed, a component nobody pairs is offered by itself only", ownerBare && ownerBare->pair == Use::None);
    check("a OneOf relationship needs naming (it isn't a component)", !findEntry(bare, "Element"));

    // A scoped name works like a component name ("::" or "."), and a missing one is ignored.
    rpe::AddCatalogOptions scoped;
    scoped.pairRelations = QStringList { QStringLiteral("::Follows"), QStringLiteral("no.such.Thing") };
    const QVector<Entry> sc = rpe::buildAddCatalog(s.world, scoped);
    check("a root-qualified name resolves; an unknown one is skipped", findEntry(sc, "Follows") != nullptr);

    // The suggestion limit.
    auto visitor = s.world.entity("Visitor");
    for (int i = 0; i < 70; ++i)
        visitor.add<Visits>(s.world.entity(("Place" + std::to_string(i)).c_str()));
    rpe::AddCatalogOptions few;
    few.maxTargets = 64;
    const QVector<Entry> cut = rpe::buildAddCatalog(s.world, few);
    const Entry* visits = findEntry(cut, "Visits");
    check("targets are cut at the limit, and the cut is flagged",
          visits && visits->targets.size() == 64 && visits->moreTargets);
    const Entry* teamNoCut = findEntry(cut, "Team");
    s.other.add<Team>(s.world.entity()); // an UNNAMED target in use
    const QVector<Entry> withUnnamed = rpe::buildAddCatalog(s.world);
    const Entry* teamUnnamed = findEntry(withUnnamed, "Team");
    check("an unnamed (#id) target in use isn't suggested", teamUnnamed && !targetPaths(teamUnnamed).join(' ').contains('#'));
    check("...an uncut list isn't flagged", teamNoCut && !teamNoCut->moreTargets);
}

// ── 2. flecs' rules, checked up front ───────────────────────────────────────────
static void testRules()
{
    printf("\n-- trait rules --\n");
    Scene s;
    auto& w = s.world;
    const auto rel = [&](auto t) { return w.component<decltype(t)>(); };

    check("a valid pair passes", rpe::pairRejection(w, rel(Damage {}), s.bFire).isEmpty());
    check("a Target-trait entity can't be the relationship",
          rpe::pairRejection(w, rel(Marker {}), s.red).contains(QStringLiteral("Target trait")));
    check("a Relationship-trait entity can't be the target",
          rpe::pairRejection(w, rel(Damage {}), rel(Likes {})).contains(QStringLiteral("Relationship trait")));
    check("a OneOf relationship refuses a target outside its scope",
          rpe::pairRejection(w, s.element, s.red).contains(QStringLiteral("OneOf")));
    check("...and takes its own children", rpe::pairRejection(w, s.element, s.elIce).isEmpty());
    check("flecs' own relationships are refused",
          !rpe::pairRejection(w, w.entity(flecs::ChildOf), s.red).isEmpty());
    check("an invalid entity is refused", !rpe::pairRejection(w, flecs::entity(), s.red).isEmpty());

    check("a Relationship can't be added by itself", !rpe::plainAddRejection(w, rel(Likes {})).isEmpty());
    check("a Target can't be added by itself", !rpe::plainAddRejection(w, rel(Marker {})).isEmpty());
    check("a plain component can", rpe::plainAddRejection(w, rel(Health {})).isEmpty());

    // Short names for showing: template-aware.
    check("scopedLeaf: a scoped name", rpe::scopedLeaf(QStringLiteral("game::Damage")) == "Damage"
                                           && rpe::scopedLeaf(QStringLiteral("game.Damage")) == "Damage");
    check("scopedLeaf: never cuts inside template arguments, and shortens them",
          rpe::scopedLeaf(QStringLiteral("std.vector<resim.core.ModelValue>")) == "vector<ModelValue>"
              && rpe::scopedLeaf(QStringLiteral("a::Map<b::K, c.d::V<e.F>>")) == "Map<K, V<F>>");
    check("scopedLeaf: plain and odd names as they are", rpe::scopedLeaf(QStringLiteral("Fire")) == "Fire"
                                                             && rpe::scopedLeaf(QStringLiteral("#123")) == "#123"
                                                             && rpe::scopedLeaf(QStringLiteral("a.B<c")) == "B<c"
                                                             && rpe::scopedLeaf(QStringLiteral("game.render\\.Stats")) == "render\\.Stats");

    // Paths the user types.
    check("lookup by dotted path", rpe::lookupEntityPath(w, QStringLiteral("a.Fire")) == s.aFire);
    check("lookup by C++ path", rpe::lookupEntityPath(w, QStringLiteral("b::Fire")) == s.bFire);
    check("lookup by root path", rpe::lookupEntityPath(w, QStringLiteral("::Element::Ice")) == s.elIce);
    check("lookup by #id", rpe::lookupEntityPath(w, QStringLiteral("#%1").arg(s.red.id())) == s.red);
    check("a leaf alone doesn't search scopes", !rpe::lookupEntityPath(w, QStringLiteral("Ice")).is_valid());
    check("junk resolves to nothing", !rpe::lookupEntityPath(w, QStringLiteral("#abc")).is_valid()
                                          && !rpe::lookupEntityPath(w, QString()).is_valid());

    // A stale id: the target died and its slot was recycled for another entity.
    flecs::entity gone = w.entity();
    const flecs::entity_t staleId = gone.id();
    gone.destruct();
    flecs::entity recycled = w.entity();
    check("(the slot really was recycled)", static_cast<uint32_t>(recycled.id()) == static_cast<uint32_t>(staleId));
    QString why;
    check("a stale target id is refused, not swapped for the recycled entity",
          rpe::addPairChecked(w, s.hero, rel(Damage {}).id(), staleId, QString(), &why) == 0 && !why.isEmpty());
    check("...and nothing was added to the recycled one", !s.hero.has(rel(Damage {}).id(), recycled.id()));
    check("#<stale id> is refused too", !rpe::lookupEntityPath(w, QStringLiteral("#%1").arg(staleId)).is_valid());

    why.clear();
    check("by typed path", rpe::addPairChecked(w, s.hero, rel(Likes {}).id(), 0, QStringLiteral("Red"), &why) != 0
                               && s.hero.has<Likes>(s.red));
    check("an unknown typed path is refused with a reason",
          rpe::addPairChecked(w, s.hero, rel(Likes {}).id(), 0, QStringLiteral("Nowhere"), &why) == 0
              && why.contains(QStringLiteral("Nowhere")));
    check("a dead entity is refused", rpe::addPairChecked(w, flecs::entity(), rel(Likes {}).id(), s.red.id()) == 0);
}

// ── 3. Through the mirror (sim thread) ──────────────────────────────────────────
static void testMirror()
{
    printf("\n-- mirror --\n");
    Scene s;
    rpe::TypeBridge::registerTypes<Health, Damage, Position, Owner>();
    auto& w = s.world;
    const qulonglong heroId = s.hero.id();
    const qulonglong damage = w.component<Damage>().id();

    rpe::EcsMirror mirror;
    mirror.attach(&w, rpe::EcsMirror::PumpMode::Manual);
    mirror.setScanIntervalsMs(0, 0);
    auto ch = mirror.channel();
    const auto pump = [&] {
        w.progress(0.016f);
        mirror.pump();
    };
    mirror.setInterest(heroId, "Health", { "hp" });
    pump();

    ch->setPairRelations({ QStringLiteral("Element") });
    pump();
    QVector<Entry> cat;
    ch->pollCatalogEntries(cat);
    check("the producer publishes relationships", findEntry(cat, "Element") && findEntry(cat, "Damage")
                                                       && findEntry(cat, "Damage")->pair == Use::Also);

    // Same leaf, different scopes: both pairs listed, keyed apart.
    ch->queueAddPair(heroId, damage, s.bFire.id());
    pump();
    check("(Damage, b.Fire) added by id", s.hero.has<Damage>(s.bFire) && s.hero.get<Damage>(s.bFire).amount == 10);
    QVector<rpe::MirrorChannel::ComponentRow> rows;
    ch->pollComponentRows(rows);
    QStringList pairKeys;
    for (const auto& r : rows)
        if (r.kind == rpe::MirrorChannel::RowKind::PairData)
            pairKeys << r.key();
    pairKeys.sort();
    check("two pairs on same-leaf targets get distinct keys",
          pairKeys == (QStringList { "Damage (a.Fire)", "Damage (b.Fire)" }));

    // Each edit lands on its own pair.
    ch->queueEdit(heroId, QStringLiteral("Damage (b.Fire)"), QStringLiteral("amount"), rttr::variant(33));
    pump();
    check("an edit to one reaches only that pair",
          s.hero.get<Damage>(s.bFire).amount == 33 && s.hero.get<Damage>(s.aFire).amount == 10);

    // Pins on both, by id.
    for (const auto& r : rows)
        if (r.kind == rpe::MirrorChannel::RowKind::PairData)
            (void)r;
    QVector<rpe::MirrorChannel::PinKey> pins;
    for (const auto& r : rows)
        if (r.kind == rpe::MirrorChannel::RowKind::PairData)
        {
            rpe::MirrorChannel::PinKey k;
            k.entity = heroId;
            k.component = r.key();
            k.path = QStringLiteral("amount");
            k.rawId = r.rawId;
            pins << k;
        }
    mirror.setPins(pins);
    pump();
    QHash<QString, int> pinned;
    for (const auto& v : mirror.pollPinValues())
        pinned.insert(v.key.component, v.value.to_int());
    check("pins on both pairs show their own values",
          pinned.value("Damage (a.Fire)") == 10 && pinned.value("Damage (b.Fire)") == 33);
    mirror.setPins({});

    // Exclusive: the new target replaces the old.
    ch->queueAddPair(heroId, w.component<Team>().id(), s.red.id());
    pump();
    check("an Exclusive relationship's new target replaces the old one",
          s.hero.has<Team>(s.red) && !s.hero.has<Team>(s.blue));

    // Rules, on the sim thread.
    ch->queueAddPair(heroId, w.component<Marker>().id(), s.red.id());
    ch->queueAddPair(heroId, damage, w.component<Likes>().id());
    ch->queueAddPair(heroId, s.element.id(), s.red.id());
    pump();
    check("pairs breaking flecs' rules are skipped (no abort)",
          !s.hero.has(w.component<Marker>().id(), s.red.id()) && !s.hero.has(damage, w.component<Likes>().id())
              && !s.hero.has(s.element, s.red));
    ch->queueAddPair(heroId, s.element.id(), s.elIce.id());
    pump();
    check("a OneOf child is accepted", s.hero.has(s.element, s.elIce));

    // Typed paths.
    ch->queueAddPair(heroId, w.component<Likes>().id(), 0, QStringLiteral("a::Fire"));
    ch->queueAddPair(heroId, w.component<Likes>().id(), 0, QStringLiteral("#%1").arg(s.apples.id()));
    ch->queueAddPair(heroId, w.component<Likes>().id(), 0, QStringLiteral("no.such.entity"));
    pump();
    check("a typed path is looked up on the sim thread", s.hero.has<Likes>(s.aFire) && s.hero.has<Likes>(s.apples));

    // Stale ids from the GUI.
    flecs::entity gone = w.entity("Gone");
    const flecs::entity_t staleId = gone.id();
    gone.destruct();
    flecs::entity recycled = w.entity();
    ch->queueAddPair(heroId, w.component<Likes>().id(), staleId);
    ch->queueAddPair(staleId, w.component<Likes>().id(), s.red.id()); // the ENTITY is stale
    pump();
    check("a stale target id is refused on the sim thread", !s.hero.has<Likes>(recycled));
    check("a stale entity id is ignored", !recycled.has<Likes>(s.red));

    // Plain adds of something flecs only takes in pairs.
    ch->queueStructural(rpe::MirrorChannel::StructuralKind::AddComponent, heroId, QStringLiteral("Likes"));
    ch->queueStructuralById(rpe::MirrorChannel::StructuralKind::AddComponent, heroId, w.component<Marker>().id());
    pump();
    check("a Relationship / Target is never added by itself",
          !s.hero.has(w.component<Likes>().id()) && !s.hero.has(w.component<Marker>().id()));
    ch->queueStructuralById(rpe::MirrorChannel::StructuralKind::AddComponent, heroId, w.component<Frozen>().id());
    pump();
    check("...while a tag by id still is", s.hero.has<Frozen>());

    mirror.detach();
}

// ── 4. The picker (both modes) ──────────────────────────────────────────────────
static QTreeWidgetItem* findItem(QTreeWidget* tree, const QString& text, QTreeWidgetItem* under = nullptr)
{
    if (!tree)
        return nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
    {
        if ((*it)->text(0) != text)
            continue;
        if (!under)
            return *it;
        for (QTreeWidgetItem* p = (*it)->parent(); p; p = p->parent())
            if (p == under)
                return *it;
    }
    return nullptr;
}

static QStringList childTexts(QTreeWidgetItem* item)
{
    QStringList out;
    if (item)
        for (int i = 0; i < item->childCount(); ++i)
            out << item->child(i)->text(0);
    return out;
}

static void key(QWidget* w, int k, const QString& text = QString())
{
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier, text);
    QCoreApplication::sendEvent(w, &press);
    QCoreApplication::processEvents();
}

static void testPicker(bool mirrorMode)
{
    printf("\n-- picker (%s) --\n", mirrorMode ? "mirror" : "direct");
    Scene s;
    rpe::TypeBridge::registerTypes<Health, Damage, Position, Owner>();
    auto& w = s.world;

    rpe::EcsMirror mirror;
    rpe::EntityComponentBrowser browser;
    auto settings = browser.settings();
    settings.allowComponentEditing = true;
    settings.pairRelations = QStringList { QStringLiteral("Element"), QStringLiteral("Follows") };
    browser.setSettings(settings);
    if (mirrorMode)
    {
        mirror.attach(&w);
        mirror.setScanIntervalsMs(0, 0);
        browser.setMirror(&mirror);
    }
    else
    {
        browser.setWorld(&w);
    }
    browser.resize(900, 600);
    browser.show();
    const auto pump = [&] {
        for (int i = 0; i < 20; ++i)
        {
            if (mirrorMode)
                w.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
    };
    pump();
    auto* el = browser.entityList()->findChild<QListWidget*>();
    for (int i = 0; el && i < el->count(); ++i)
        if (el->item(i)->text() == QStringLiteral("Hero"))
            el->setCurrentRow(i);
    pump();

    QTreeWidget* tree = nullptr;
    QLineEdit* search = nullptr;
    QPointer<QFrame> popup;
    const auto open = [&] {
        browser.componentList()->findChild<QToolButton*>()->click();
        pump(); // mirror mode: the rescan asked for on opening arrives — in place
        popup = browser.componentList()->findChild<QFrame*>(QStringLiteral("rpeAddPopup"));
        tree = popup ? popup->findChild<QTreeWidget*>() : nullptr;
        search = popup ? popup->findChild<QLineEdit*>() : nullptr;
    };
    const auto closePopup = [&] {
        if (popup)
            popup->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        popup = nullptr;
        tree = nullptr;
        search = nullptr;
    };

    open();
    check("the picker opens with a tree", tree && search);
    QTreeWidgetItem* rels = findItem(tree, QStringLiteral("Relationships"));
    check("relationships have their own branch", rels != nullptr);
    if (!rels)
    {
        closePopup();
        return;
    }
    QTreeWidgetItem* damage = findItem(tree, QStringLiteral("Damage"), rels);
    check("a component used in pairs sits under it", damage != nullptr);
    QTreeWidgetItem* plainDamage = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->text(0) == QStringLiteral("Damage") && *it != damage)
            plainDamage = *it;
    check("...and is still offered by itself, though the entity has a pair of it", plainDamage != nullptr);
    check("what can be added comes first, then Other target…, then the entity's own pair (marked)",
          childTexts(damage) == (QStringList { QStringLiteral("\u2192 Left"), QStringLiteral("Other target…"),
                                               QStringLiteral("\u2192 Fire  (already added)") }));
    check("...the entity's own pair is hidden while browsing", findItem(tree, QStringLiteral("\u2192 Fire  (already added)"), damage)
                                                               && findItem(tree, QStringLiteral("\u2192 Fire  (already added)"), damage)->isHidden());
    QTreeWidgetItem* position = findItem(tree, QStringLiteral("Position"), rels);
    check("an Also relationship is under Relationships with its target",
          position && childTexts(position).contains(QStringLiteral("→ World")));
    QTreeWidgetItem* plainPos = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->text(0) == QStringLiteral("Position") && *it != position)
            plainPos = *it;
    check("...and offered by itself as well", plainPos != nullptr);
    check("Health (on the entity) isn't offered", !findItem(tree, QStringLiteral("Health")));
    QTreeWidgetItem* team = findItem(tree, QStringLiteral("Team"), rels);
    check("an Exclusive relationship says what it replaces",
          team && childTexts(team).contains(QStringLiteral("→ Red  (replaces Blue)")));
    QTreeWidgetItem* element = findItem(tree, QStringLiteral("Element"), rels);
    check("a OneOf relationship lists its children, without Other target…",
          element && childTexts(element) == (QStringList { QStringLiteral("→ Fire"), QStringLiteral("→ Ice") }));
    check("relationships start collapsed", damage && !damage->isExpanded() && team && !team->isExpanded());
    check("Relationships come before Tags, last but one",
          tree && tree->topLevelItemCount() >= 2
              && tree->topLevelItem(tree->topLevelItemCount() - 2)->text(0) == QStringLiteral("Relationships")
              && tree->topLevelItem(tree->topLevelItemCount() - 1)->text(0) == QStringLiteral("Tags"));
    check("Enter right away never picks a hidden (collapsed) target",
          tree->currentItem() && tree->currentItem()->parent() != team && tree->currentItem()->parent() != element);

    // Filtering opens the matching relationship; Enter adds the pair.
    search->setText(QStringLiteral("red"));
    QCoreApplication::processEvents();
    check("a filter matching a target opens its relationship", team->isExpanded() && !team->isHidden());
    check("...and highlights that target", tree->currentItem() && tree->currentItem()->parent() == team);
    key(search, Qt::Key_Return);
    pump();
    check("Enter adds the pair (and the old target goes: Exclusive)",
          s.hero.has<Team>(s.red) && !s.hero.has<Team>(s.blue));
    check("the picker closed", !browser.componentList()->findChild<QFrame*>(QStringLiteral("rpeAddPopup"))
                                   || !browser.componentList()->findChild<QFrame*>(QStringLiteral("rpeAddPopup"))->isVisible());
    closePopup();

    // A click on a relationship opens it; a click on a target adds the pair.
    open();
    rels = findItem(tree, QStringLiteral("Relationships"));
    QTreeWidgetItem* el2 = findItem(tree, QStringLiteral("Element"), rels);
    emit tree->itemClicked(el2, 0);
    check("a click opens a relationship", el2 && el2->isExpanded());
    emit tree->itemClicked(findItem(tree, QStringLiteral("→ Ice"), el2), 0);
    pump();
    check("a click on a target adds the pair", s.hero.has(s.element, s.elIce));
    closePopup();

    // Other target…: any entity, filtered; Backspace goes back; a typed path.
    open();
    rels = findItem(tree, QStringLiteral("Relationships"));
    QTreeWidgetItem* likes = findItem(tree, QStringLiteral("Likes"), rels);
    QTreeWidgetItem* other = likes ? findItem(tree, QStringLiteral("Other target…"), likes) : nullptr;
    check("a relationship with nothing in use offers Other target…", other != nullptr);
    emit tree->itemClicked(other, 0);
    QCoreApplication::processEvents();
    check("Other target… opens the target step",
          tree->topLevelItemCount() > 0 && tree->topLevelItem(0)->text(0).startsWith(QStringLiteral("← Likes")));
    QTreeWidgetItem* ents = findItem(tree, QStringLiteral("Entities"));
    check("the target step's groups are open, filter empty",
          ents && ents->isExpanded() && (!findItem(tree, QStringLiteral("Tags")) || findItem(tree, QStringLiteral("Tags"))->isExpanded()));
    check("...listing the entities", ents && ents->childCount() > 3);
    check("...plain entities included (a target is often one), rpe's and flecs' own left out",
          childTexts(ents).contains(QStringLiteral("Red")) && childTexts(ents).contains(QStringLiteral("a.Fire"))
              && !childTexts(ents).contains(QStringLiteral("rpe.EcsMirror")) && !childTexts(ents).contains(QStringLiteral("rpe"))
              && !childTexts(ents).join(QLatin1Char(' ')).contains(QStringLiteral("flecs")));
    check("...and the relationship itself isn't a target", !childTexts(ents).contains(QStringLiteral("Likes")));
    // Likes is a TAG relationship: its pair's data would be its target's, so the
    // components that carry data (registered with rpe) come first, in their own group.
    QTreeWidgetItem* dataComps = findItem(tree, QStringLiteral("Components (carry the data)"));
    check("a tag relationship lists the components that carry data first",
          dataComps && tree->indexOfTopLevelItem(dataComps) < tree->indexOfTopLevelItem(ents)
              && childTexts(dataComps).contains(QStringLiteral("Health")) && childTexts(dataComps).contains(QStringLiteral("Position")));
    check("...only YOURS (registered with rpe): not std:: / unregistered types",
          !childTexts(dataComps).join(QLatin1Char(' ')).contains(QStringLiteral("vector")));
    check("...and not again among the entities", !childTexts(ents).contains(QStringLiteral("Health")));
    QTreeWidgetItem* tagsGroup = findItem(tree, QStringLiteral("Tags"));
    check("tags have their own group, between the data components and the entities",
          tagsGroup && tree->indexOfTopLevelItem(dataComps) < tree->indexOfTopLevelItem(tagsGroup)
              && tree->indexOfTopLevelItem(tagsGroup) < tree->indexOfTopLevelItem(ents)
              && childTexts(tagsGroup).contains(QStringLiteral("Frozen")) && childTexts(tagsGroup).contains(QStringLiteral("Left")));
    check("...a tag used as a relationship elsewhere is still offered (a world may be switching sides)",
          childTexts(tagsGroup).contains(QStringLiteral("Team")));
    check("...and the entities group holds plain entities only", !childTexts(ents).contains(QStringLiteral("Frozen")));
    check("...nothing under an unnamed parent (\"#140.allocators\")",
          childTexts(ents).filter(QStringLiteral("#")).isEmpty()
              && (!dataComps || childTexts(dataComps).filter(QStringLiteral("#")).isEmpty()));
    check("...not queries, systems, enum constants, std:: types",
          !childTexts(ents).contains(QStringLiteral("HealthQuery")) && !childTexts(ents).contains(QStringLiteral("HealthSystem"))
              && !childTexts(ents).contains(QStringLiteral("Mood.Calm"))
              && childTexts(ents).filter(QRegularExpression(QStringLiteral("^std(\\.|$)"))).isEmpty());
    check("...plain entities used as relationships are too", childTexts(ents).contains(QStringLiteral("Eats")));
    check("...the filter box was cleared, with a target placeholder",
          search->text().isEmpty() && search->placeholderText().contains(QStringLiteral("target")));
    key(search, Qt::Key_Backspace);
    check("Backspace in the empty filter goes back to the components",
          findItem(tree, QStringLiteral("Relationships")) != nullptr);
    rels = findItem(tree, QStringLiteral("Relationships"));
    likes = findItem(tree, QStringLiteral("Likes"), rels);
    emit tree->itemClicked(findItem(tree, QStringLiteral("Other target…"), likes), 0);
    search->setText(QStringLiteral("Apples"));
    QCoreApplication::processEvents();
    check("typing narrows the entities and highlights the match",
          tree->currentItem() && tree->currentItem()->text(0).contains(QStringLiteral("Apples")));
    key(search, Qt::Key_Return);
    pump();
    check("Enter adds (relationship, picked entity)", s.hero.has<Likes>(s.apples));
    closePopup();

    open();
    rels = findItem(tree, QStringLiteral("Relationships"));
    likes = findItem(tree, QStringLiteral("Likes"), rels);
    emit tree->itemClicked(findItem(tree, QStringLiteral("Other target…"), likes), 0);
    search->setText(QStringLiteral("b::Fire"));
    QCoreApplication::processEvents();
    check("a path no row matches offers itself as the target",
          tree->currentItem() && tree->currentItem()->text(0).contains(QStringLiteral("b::Fire")));
    key(search, Qt::Key_Return);
    pump();
    check("...and Enter adds it by path", s.hero.has<Likes>(s.bFire));
    closePopup();

    // A relationship WITH data (Damage): its target is a label — entities only.
    open();
    rels = findItem(tree, QStringLiteral("Relationships"));
    QTreeWidgetItem* dmgRel = findItem(tree, QStringLiteral("Damage"), rels);
    emit tree->itemClicked(findItem(tree, QStringLiteral("Other target\u2026"), dmgRel), 0);
    QCoreApplication::processEvents();
    check("the target step shows the entity's own pair, marked, LAST under In use",
          findItem(tree, QStringLiteral("In use"))
              && childTexts(findItem(tree, QStringLiteral("In use"))).last() == QStringLiteral("Fire  (already added)"));
    check("a relationship with data ((Damage, Left)): no data components — its target is a label",
          !findItem(tree, QStringLiteral("Components (carry the data)")));
    check("...tags first, then entities", findItem(tree, QStringLiteral("Tags")) && findItem(tree, QStringLiteral("Entities"))
                                             && tree->indexOfTopLevelItem(findItem(tree, QStringLiteral("Tags")))
                                                 < tree->indexOfTopLevelItem(findItem(tree, QStringLiteral("Entities"))));
    closePopup();

    // A relationship that gains a target while the picker is open: refreshed in place.
    open();
    rels = findItem(tree, QStringLiteral("Relationships"));
    QTreeWidgetItem* dmg = findItem(tree, QStringLiteral("Damage"), rels);
    emit tree->itemClicked(dmg, 0); // open it
    s.other.set<Damage>(s.red, { 1 });  // the sim pairs Damage with a new target
    if (mirrorMode)
    {
        mirror.channel()->requestCatalogRefresh();
        pump();
    }
    else
    {
        emit browser.componentList()->addPickerWanted(); // direct mode rescans on this
        QCoreApplication::processEvents();
    }
    rels = findItem(tree, QStringLiteral("Relationships"));
    dmg = findItem(tree, QStringLiteral("Damage"), rels);
    check("the open picker shows the new target in place", dmg && childTexts(dmg).contains(QStringLiteral("→ Red")));
    check("...keeping the relationship open", dmg && dmg->isExpanded());
    closePopup();

    // Tags by themselves (direct mode used to refuse unbridged ones).
    open();
    QTreeWidgetItem* frozen = findItem(tree, QStringLiteral("Frozen"));
    emit tree->itemClicked(frozen, 0);
    pump();
    check("a tag is added by itself", s.hero.has<Frozen>());
    closePopup();

    // A pair the entity already has: picking it selects its row, adds nothing.
    {
        auto* compList = browser.componentList()->findChild<QListWidget*>();
        const auto pairsOf = [&] {
            int n = 0;
            s.hero.each([&](flecs::id id) { n += id.is_pair() ? 1 : 0; });
            return n;
        };
        const int pairsBefore = pairsOf();
        open();
        search->setText(QStringLiteral("Fire")); // typed for: the entity's own pair shows
        QCoreApplication::processEvents();
        rels = findItem(tree, QStringLiteral("Relationships"));
        QTreeWidgetItem* dmgRel = findItem(tree, QStringLiteral("Damage"), rels);
        QTreeWidgetItem* own = findItem(tree, QStringLiteral("\u2192 Fire  (already added)"), dmgRel);
        check("typing for it shows the entity's own pair, marked", own && !own->isHidden());
        emit tree->itemClicked(own, 0);
        pump();
        check("clicking a pair the entity has selects its row",
              compList && compList->currentItem() && compList->currentItem()->text() == QStringLiteral("Damage \u2192 Fire"));
        check("...adds nothing", pairsOf() == pairsBefore);
        check("...and closes the picker", !popup || !popup->isVisible());
        closePopup();

        // By keyboard: filter to it, Enter.
        compList->setCurrentRow(-1);
        for (int i = 0; i < compList->count(); ++i)
            if (compList->item(i)->text() == QStringLiteral("Health"))
                compList->setCurrentRow(i);
        pump();
        open();
        search->setText(QStringLiteral("Damage a.Fire"));
        QCoreApplication::processEvents();
        check("filtering to it highlights the marked entry",
              tree->currentItem() && tree->currentItem()->text(0).contains(QStringLiteral("already added")));
        key(search, Qt::Key_Return);
        pump();
        check("Enter on it selects the pair's row", compList->currentItem() && compList->currentItem()->text() == QStringLiteral("Damage \u2192 Fire"));
        check("...still adds nothing", pairsOf() == pairsBefore);
        closePopup();
    }

    // A component the entity already has as a PAIR can still be added by itself.
    open();
    rels = findItem(tree, QStringLiteral("Relationships"));
    QTreeWidgetItem* plainDmg = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->text(0) == QStringLiteral("Damage") && (*it)->parent() != nullptr && !(*it)->data(0, Qt::UserRole).isNull())
            plainDmg = *it;
    emit tree->itemClicked(plainDmg, 0);
    pump();
    check("a component added by itself next to its pair", s.hero.has<Damage>() && s.hero.has<Damage>(s.aFire));
    closePopup();

    // A pair whose target's name has template arguments: never cut inside them.
    for (int i = 0; el && i < el->count(); ++i)
        if (el->item(i)->text() == QStringLiteral("Other"))
            el->setCurrentRow(i);
    pump();
    auto* compList = browser.componentList()->findChild<QListWidget*>();
    QStringList rowsShown;
    for (int i = 0; compList && i < compList->count(); ++i)
        rowsShown << compList->item(i)->text();
    check("a template target shows as vector<Position>, not \"Position>\"",
          rowsShown.contains(QStringLiteral("Likes \u2192 vector<Position>")));

    if (mirrorMode)
        mirror.detach();
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testCatalog();
    testRules();
    testMirror();
    testPicker(true);
    testPicker(false);

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

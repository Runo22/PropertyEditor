// Named elements (rpe/core/NamedElement.h): an array of name/value structs shown
// by NAME — one row per element, its value edited on that row — with search and
// pins finding an element by its name. Covers:
//   • marking: class metadata (const char* / std::string), TypeBridge per type,
//     per type name, per TEMPLATE (also for an instantiation registered later),
//     the runtime winning over metadata, turning it off, surviving an unregister;
//   • any name type: std::string, string_view, const char*, QString, enum, int,
//     a getter-only name, std::array;
//   • "{name}" paths (read / write / nested / missing / duplicates), the name
//     being read-only, display paths, search counts;
//   • the model: rows, values, editing, freezing, other fields below, struct
//     values, reorders, names given as text;
//   • the browser in mirror AND direct mode: grid, edit, grouped search, pins by
//     name through reorder / erase / re-add, names sent as text from the sim.
#include <rpe/core/EditorHints.h>
#include <rpe/core/NamedElement.h>
#include <rpe/core/PropertySearch.h>
#include <rpe/core/ReadOnly.h>
#include <rpe/core/RttrBridge.h>
#include <rpe/core/TypeBridge.h>
#include <rpe/core/TypeRenderer.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/EntityComponentBrowser.h>
#include <rpe/ecs/PinnedPropertiesWidget.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>

#include <array>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

enum class StatKind
{
    Speed,
    Armor,
    MaxSpeed
};

template <class N, class V>
struct NV
{
    N name {};
    V value {};
};

namespace game
{
    template <class N, class V>
    struct Stat // marked by its namespaced template name
    {
        N name {};
        V value {};
    };
} // namespace game

struct Vec2
{
    float x = 0, y = 0;
};

struct WithUnit // name, value AND another field
{
    std::string name;
    float value = 0;
    std::string unit;
};

struct Getter // the name only through a getter
{
    std::string n;
    float value = 0;
    const std::string& name() const { return n; }
};

struct ByMeta // marked by metadata given as std::string
{
    std::string key;
    int amount = 0;
};

struct Unmarked
{
    std::string name;
    float value = 0;
};

static std::string g_owned[3] = { "speed", "armor", "maxSpeed" };

struct Comp
{
    std::vector<NV<std::string, float>> stats { { "speed", 4.f }, { "armor", 7.f }, { "maxSpeed", 9.f } };
    std::vector<NV<std::string_view, float>> views { { g_owned[0], 4.f }, { g_owned[1], 7.f } };
    std::vector<NV<const char*, int>> cstrs { { "speed", 4 }, { "armor", 7 } };
    std::vector<NV<StatKind, float>> kinds { { StatKind::Speed, 4.f }, { StatKind::Armor, 7.f } };
    std::vector<NV<int, float>> ids { { 10, 4.f }, { 20, 7.f } };
    std::vector<NV<QString, double>> qstrs { { "speed", 4.0 }, { "armor", 7.0 } };
    std::vector<Getter> getters { { "speed", 4.f }, { "armor", 7.f } };
    std::array<NV<std::string, float>, 2> fixed { { { "speed", 4.f }, { "armor", 7.f } } };
    std::vector<NV<std::string, Vec2>> points { { "spawn", { 1.f, 2.f } }, { "goal", { 3.f, 4.f } } };
    std::vector<WithUnit> units { { "range", 30.f, "m" }, { "delay", 2.f, "s" } };
    std::vector<game::Stat<std::string, int>> gstats { { "level", 3 } };
    std::vector<ByMeta> metas { { "gold", 100 } };
    std::vector<Unmarked> plain { { "speed", 1.f } };
    std::vector<NV<std::string, float>> dups { { "x", 1.f }, { "x", 2.f }, { "a.b", 3.f } };
};

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::enumeration<StatKind>("StatKind")(value("Speed", StatKind::Speed), value("Armor", StatKind::Armor),
                                                    value("MaxSpeed", StatKind::MaxSpeed));
    registration::class_<Vec2>("Vec2").property("x", &Vec2::x).property("y", &Vec2::y);
    // Marked through the TEMPLATE (setNamedElementTemplate("NV") in main) — except
    // NV<std::string,float>, which also carries metadata (const char* values).
    registration::class_<NV<std::string, float>>("NV<std::string,float>")(
        metadata(rpe::hint::ElementLabel, "name"), metadata(rpe::hint::ElementValue, "value"))
        .property("name", &NV<std::string, float>::name)
        .property("value", &NV<std::string, float>::value);
    registration::class_<NV<std::string_view, float>>("NV<std::string_view,float>")
        .property("name", &NV<std::string_view, float>::name)
        .property("value", &NV<std::string_view, float>::value);
    registration::class_<NV<const char*, int>>("NV<const char*,int>")
        .property("name", &NV<const char*, int>::name)
        .property("value", &NV<const char*, int>::value);
    registration::class_<NV<StatKind, float>>("NV<StatKind,float>")
        .property("name", &NV<StatKind, float>::name)
        .property("value", &NV<StatKind, float>::value);
    registration::class_<NV<int, float>>("NV<int,float>")
        .property("name", &NV<int, float>::name)
        .property("value", &NV<int, float>::value);
    registration::class_<NV<QString, double>>("NV<QString,double>")
        .property("name", &NV<QString, double>::name)
        .property("value", &NV<QString, double>::value);
    registration::class_<NV<std::string, Vec2>>("NV<std::string,Vec2>")
        .property("name", &NV<std::string, Vec2>::name)
        .property("value", &NV<std::string, Vec2>::value);
    registration::class_<game::Stat<std::string, int>>("game::Stat<std::string,int>")
        .property("name", &game::Stat<std::string, int>::name)
        .property("value", &game::Stat<std::string, int>::value);
    registration::class_<WithUnit>("WithUnit")
        .property("name", &WithUnit::name)
        .property("value", &WithUnit::value)
        .property("unit", &WithUnit::unit);
    registration::class_<Getter>("Getter").property_readonly("name", &Getter::name).property("value", &Getter::value);
    registration::class_<ByMeta>("ByMeta")(metadata(rpe::hint::ElementLabel, std::string("key")),
                                           metadata(rpe::hint::ElementValue, std::string("amount")))
        .property("key", &ByMeta::key)
        .property("amount", &ByMeta::amount);
    registration::class_<Unmarked>("Unmarked").property("name", &Unmarked::name).property("value", &Unmarked::value);
    registration::class_<Comp>("Comp")
        .property("stats", &Comp::stats)
        .property("views", &Comp::views)
        .property("cstrs", &Comp::cstrs)
        .property("kinds", &Comp::kinds)
        .property("ids", &Comp::ids)
        .property("qstrs", &Comp::qstrs)
        .property("getters", &Comp::getters)
        .property("fixed", &Comp::fixed)
        .property("points", &Comp::points)
        .property("units", &Comp::units)
        .property("gstats", &Comp::gstats)
        .property("metas", &Comp::metas)
        .property("plain", &Comp::plain)
        .property("dups", &Comp::dups);
}

static int g_fails = 0;
static void check(const std::string& n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n.c_str());
    if (!ok)
        ++g_fails;
}

static QString fieldsOf(rttr::type t)
{
    const auto f = rpe::TypeBridge::namedElementFields(t);
    return QString::fromStdString(f.label + "/" + f.value);
}

// ── 1. marking ──────────────────────────────────────────────────────────────────
static void testMarking()
{
    printf("\n-- marking --\n");
    check("metadata (const char*): name/value", fieldsOf(rttr::type::get<NV<std::string, float>>()) == "name/value");
    check("metadata (std::string): key/amount", fieldsOf(rttr::type::get<ByMeta>()) == "key/amount");
    check("template mark covers an instantiation", fieldsOf(rttr::type::get<NV<StatKind, float>>()) == "name/value");
    check("...every one", fieldsOf(rttr::type::get<NV<const char*, int>>()) == "name/value"
                              && fieldsOf(rttr::type::get<NV<QString, double>>()) == "name/value");
    check("a namespaced template by its short name (\"Stat\" → game::Stat<…>)",
          fieldsOf(rttr::type::get<game::Stat<std::string, int>>()) == "name/value");
    check("per type: a type without metadata", fieldsOf(rttr::type::get<Getter>()) == "name/value");
    check("not marked → empty", fieldsOf(rttr::type::get<Unmarked>()) == "/");

    // An instantiation registered AFTER the template was marked.
    {
        using Late = NV<std::string, int>;
        rttr::registration::class_<Late>("NV<std::string,int>").property("name", &Late::name).property("value", &Late::value);
        check("an instantiation registered later is covered by the template", fieldsOf(rttr::type::get<Late>()) == "name/value");
    }

    // Runtime wins over metadata; empty turns it off; by name.
    const uint64_t gen = rpe::TypeBridge::namedElementGeneration();
    rpe::TypeBridge::setNamedElement<ByMeta>("key"); // no value field
    check("runtime per type wins over the metadata", fieldsOf(rttr::type::get<ByMeta>()) == "key/");
    check("...and bumps the generation", rpe::TypeBridge::namedElementGeneration() != gen);
    rpe::TypeBridge::setNamedElement<ByMeta>("");
    check("an empty label turns it OFF, metadata or not", fieldsOf(rttr::type::get<ByMeta>()) == "/");
    rpe::TypeBridge::setNamedElement(std::string_view("ByMeta"), "key", "amount");
    check("by type name", fieldsOf(rttr::type::get<ByMeta>()) == "key/amount");
    rpe::TypeBridge::setNamedElement<NV<int, float>>("");
    check("a per-type OFF wins over the template", fieldsOf(rttr::type::get<NV<int, float>>()) == "/");
    rpe::TypeBridge::setNamedElement<NV<int, float>>("name", "value");

    // An unregister drops ids, not names: a per-type mark survives a plugin reload.
    struct Bridged
    {
        int v = 0;
    };
    rttr::registration::class_<Bridged>("Bridged").property("v", &Bridged::v);
    rpe::TypeBridge::registerType<Bridged>();
    rpe::TypeBridge::unregisterType(rttr::type::get<Bridged>());
    check("a per-type mark survives an unregister (kept by name)", fieldsOf(rttr::type::get<Getter>()) == "name/value");

    check("an element type with no such label field isn't named",
          !rpe::namedElement(rttr::type::get<Vec2>()).isValid());
    rpe::TypeBridge::setNamedElement<Vec2>("nope", "x");
    check("...a label field that doesn't exist: not named", !rpe::namedElement(rttr::type::get<Vec2>()).isValid());
    rpe::TypeBridge::setNamedElement<Vec2>("");
}

// ── 2. any name type; paths; read-only; search ──────────────────────────────────
static QStringList labelsOf(const rttr::variant& container)
{
    QStringList out;
    const auto view = container.create_sequential_view();
    const rpe::NamedElement ne = rpe::namedElement(view.get_value_type());
    for (const auto& item : view)
        out << rpe::namedElementLabel(ne, item);
    return out;
}

static void testCore()
{
    printf("\n-- name types, paths, search --\n");
    Comp c;
    const rttr::type ct = rttr::type::get<Comp>();
    const auto get = [&](const char* p) { return ct.get_property(p).get_value(c); };
    check("std::string names", labelsOf(get("stats")) == (QStringList { "speed", "armor", "maxSpeed" }));
    check("string_view names", labelsOf(get("views")) == (QStringList { "speed", "armor" }));
    check("const char* names (shown as text now)", labelsOf(get("cstrs")) == (QStringList { "speed", "armor" }));
    check("enum names (the enumerator's name)", labelsOf(get("kinds")) == (QStringList { "Speed", "Armor" }));
    check("int names", labelsOf(get("ids")) == (QStringList { "10", "20" }));
    check("QString names", labelsOf(get("qstrs")) == (QStringList { "speed", "armor" }));
    check("getter-only names", labelsOf(get("getters")) == (QStringList { "speed", "armor" }));
    check("std::array of them", labelsOf(get("fixed")) == (QStringList { "speed", "armor" }));
    check("a const char* field displays as its text", rpe::TypeRenderer::toDisplayString(rttr::variant(static_cast<const char*>("hi"))) == "hi");
    check("...a null one as empty", rpe::TypeRenderer::toDisplayString(rttr::variant(static_cast<const char*>(nullptr))).isEmpty());

    rttr::instance inst(c);
    const auto read = [&](const char* p) { return rpe::TypeRenderer::toDisplayString(rpe::bridge::getValueByPath(inst, QString::fromLatin1(p))); };
    check("{name} reads", read("stats.{armor}.value") == "7" && read("kinds.{Armor}.value") == "7" && read("ids.{20}.value") == "7"
                              && read("cstrs.{armor}.value") == "7" && read("getters.{armor}.value") == "7" && read("fixed.{armor}.value") == "7");
    check("{name} into a struct value", read("points.{goal}.value.y") == "4");
    check("a missing name reads nothing", !rpe::bridge::getValueByPath(inst, QStringLiteral("stats.{nope}.value")).is_valid());
    check("duplicates: the first", read("dups.{x}.value") == "1");
    check("{name} on an unmarked array: nothing", !rpe::bridge::getValueByPath(inst, QStringLiteral("plain.{speed}.value")).is_valid());
    check("{name} writes", rpe::bridge::setValueByPath(inst, QStringLiteral("stats.{armor}.value"), rttr::variant(70.0)) && c.stats[1].value == 70.f);
    check("...through string_view / enum / const char* names too",
          rpe::bridge::setValueByPath(inst, QStringLiteral("views.{armor}.value"), rttr::variant(71.0))
              && rpe::bridge::setValueByPath(inst, QStringLiteral("kinds.{Armor}.value"), rttr::variant(72.0))
              && rpe::bridge::setValueByPath(inst, QStringLiteral("cstrs.{armor}.value"), rttr::variant(73))
              && c.views[1].value == 71.f && c.kinds[1].value == 72.f && c.cstrs[1].value == 73);
    check("...nested", rpe::bridge::setValueByPath(inst, QStringLiteral("points.{goal}.value.x"), rttr::variant(30.0)) && c.points[1].value.x == 30.f);
    check("a write to a missing name fails", !rpe::bridge::setValueByPath(inst, QStringLiteral("stats.{nope}.value"), rttr::variant(1.0)));
    check("findNamedElement", rpe::findNamedElement(get("stats"), "maxSpeed") == 2 && rpe::findNamedElement(get("stats"), "zzz") == -1);

    check("the name field is read-only", !rpe::readOnlyReason(ct, "stats.[1].name").isEmpty()
                                              && !rpe::readOnlyReason(ct, "stats.{armor}.name").isEmpty());
    check("the value field isn't", rpe::readOnlyReason(ct, "stats.{armor}.value").isEmpty() && rpe::readOnlyReason(ct, "stats.[1].value").isEmpty());
    check("an unmarked struct's name field isn't", rpe::readOnlyReason(ct, "plain.[0].name").isEmpty());
    check("displayPath drops the value field after a name", rpe::displayPath(ct, "stats.{armor}.value") == "stats.armor");
    check("...keeps a struct value's field", rpe::displayPath(ct, "points.{goal}.value.x") == "points.goal.x");
    check("...and other fields", rpe::displayPath(ct, "units.{range}.unit") == "units.range.unit");
    check("...index paths as they are", rpe::displayPath(ct, "stats.[1].value") == "stats.[1].value");
    check("isNamedLabelPath", rpe::isNamedLabelPath(ct, "stats.[1].name") && rpe::isNamedLabelPath(ct, "views.{armor}.name")
                                  && !rpe::isNamedLabelPath(ct, "stats.[1].value") && !rpe::isNamedLabelPath(ct, "plain.[0].name"));
    check("ownedText turns views into owned text", rpe::ownedText(rttr::variant(std::string_view("ab"))).get_type() == rttr::type::get<QString>()
                                                       && rpe::ownedText(rttr::variant(5)).get_type() == rttr::type::get<int>());
    QHash<QString, QStringList> names;
    rpe::collectNamedLabels(rttr::variant(c), QStringLiteral("c"), names);
    check("collectNamedLabels finds every named array inside a value",
          names.value("c.views") == (QStringList { "speed", "armor" }) && names.value("c.kinds") == (QStringList { "Speed", "Armor" })
              && !names.contains("c.plain"));

    // Search: one row per element (name OR value), other fields below.
    const auto counts = [&](const char* needle) {
        std::vector<rpe::PropertyHit> hits;
        rpe::searchProperties(rttr::instance(c), ct, QString::fromLatin1(needle), hits);
        QMap<QString, int> m;
        for (const auto& h : hits)
            m.insert(h.path, h.matches);
        return m;
    };
    Comp fresh;
    c = fresh;
    const auto armor = counts("armor");
    check("search by name: one row per element, any name type",
          armor.value("stats") == 1 && armor.value("views") == 1 && armor.value("cstrs") == 1 && armor.value("kinds") == 1
              && armor.value("qstrs") == 1 && armor.value("getters") == 1 && armor.value("fixed") == 1);
    check("search by value: the same row", counts("9").value("stats") == 1);
    check("a number name matches like text", counts("2").value("ids") == 1); // the element named 20
    check("the field NAMES 'name' / 'value' aren't rows any more", !counts("value").contains("stats") && !counts("name").contains("stats"));
    check("...an unmarked array still counts them", counts("name").value("plain") == 1);
    check("other fields are rows below", counts("m").value("units") == 1); // unit "m"
}

// ── 3. the model ────────────────────────────────────────────────────────────────
static QModelIndex child(const QAbstractItemModel& m, const QModelIndex& parent, const QString& name)
{
    for (int r = 0; r < m.rowCount(parent); ++r)
        if (m.index(r, 0, parent).data().toString() == name)
            return m.index(r, 0, parent);
    return {};
}

static void testModel()
{
    printf("\n-- model --\n");
    Comp c;
    rpe::PropertyModel model;
    model.bindType(rttr::type::get<Comp>());
    model.setEditPolicy(rpe::EditPolicy::WriteBack);
    model.setInstanceProvider([&] { return rttr::instance(c); });
    model.refresh(rttr::instance(c));
    QCoreApplication::processEvents();

    const QModelIndex stats = child(model, {}, "stats");
    check("one row per element, named", model.rowCount(stats) == 3 && model.index(1, 0, stats).data().toString() == "armor");
    const QModelIndex armor = model.index(1, 0, stats);
    check("...its value on the same row", armor.siblingAtColumn(1).data().toString() == "7");
    check("...no name / value child rows", model.rowCount(armor) == 0);
    check("...editable", armor.siblingAtColumn(1).flags() & Qt::ItemIsEditable);
    check("...its path is the value field's", armor.data(rpe::PropertyPathRole).toString() == "stats.[1].value");
    check("...its stable path is by name", armor.data(rpe::StablePathRole).toString() == "stats.{armor}.value");
    check("...the name field's path to watch", armor.data(rpe::RowLabelPathRole).toString() == "stats.[1].name");
    check("...the value's hints / type", armor.siblingAtColumn(1).data(rpe::RttrVariantRole).value<rttr::variant>().get_type() == rttr::type::get<float>());

    for (const char* f : { "views", "cstrs", "kinds", "ids", "qstrs", "getters", "fixed", "gstats", "metas" })
    {
        const QModelIndex a = child(model, {}, f);
        check(std::string("named rows for ") + f, model.rowCount(a) >= 1 && model.rowCount(model.index(0, 0, a)) == 0
                                                      && !model.index(0, 0, a).data().toString().startsWith("["));
    }
    const QModelIndex units = child(model, {}, "units");
    const QModelIndex range = model.index(0, 0, units);
    check("other fields stay as rows below", range.data().toString() == "range" && model.rowCount(range) == 1
                                                 && model.index(0, 0, range).data().toString() == "unit");
    const QModelIndex points = child(model, {}, "points");
    const QModelIndex goal = model.index(1, 0, points);
    check("a struct value: the row is named, the value expands below",
          goal.data().toString() == "goal" && model.rowCount(goal) == 1 && model.index(0, 0, goal).data().toString() == "value");
    const QModelIndex plain = child(model, {}, "plain");
    check("an unmarked array keeps [i] rows with name/value children",
          model.index(0, 0, plain).data().toString() == "[0]" && model.rowCount(model.index(0, 0, plain)) == 2);
    const QModelIndex dups = child(model, {}, "dups");
    check("duplicate names keep index stable paths", model.index(0, 0, dups).data(rpe::StablePathRole).toString() == "dups.[0].value");
    check("a name with '.' keeps an index stable path", model.index(2, 0, dups).data(rpe::StablePathRole).toString() == "dups.[2].value");

    // Edit on the row.
    model.setData(armor.siblingAtColumn(1), QVariant::fromValue(rttr::variant(70.0)), Qt::EditRole);
    check("editing the row writes the value field", c.stats[1].value == 70.f && c.stats[1].name == "armor");

    // Freeze / reset.
    model.beginLocalEdit(QStringLiteral("stats.[1].value"));
    check("freezing the value field marks the row", model.index(1, 0, stats).data(rpe::HasLocalEditRole).toBool());
    c.stats[1].value = 1.f;
    model.refresh(rttr::instance(c));
    check("...which keeps its value", model.index(1, 1, stats).data().toString() == "70");
    model.resetNode(QStringLiteral("stats.[1].value"));
    model.refresh(rttr::instance(c));
    check("...until reset", model.index(1, 1, stats).data().toString() == "1");

    // Reorder: names follow.
    std::swap(c.stats[0], c.stats[2]);
    model.refresh(rttr::instance(c));
    QCoreApplication::processEvents();
    check("a reorder: the names follow", model.index(0, 0, stats).data().toString() == "maxSpeed"
                                            && model.index(2, 0, stats).data().toString() == "speed"
                                            && model.index(0, 1, stats).data().toString() == "9");
    // Shrink / grow.
    c.stats.resize(1);
    model.refresh(rttr::instance(c));
    check("a shrink", model.rowCount(child(model, {}, "stats")) == 1);
    c.stats.push_back({ "turbo", 5.f });
    model.refresh(rttr::instance(c));
    check("a grow: the new element named", model.index(1, 0, child(model, {}, "stats")).data().toString() == "turbo");

    // Marked at runtime while the rows are up: they reshape on the next refresh.
    check("(an unmarked array has [i] rows)", model.index(0, 0, child(model, {}, "plain")).data().toString() == "[0]");
    rpe::TypeBridge::setNamedElement<Unmarked>("name", "value");
    model.refresh(rttr::instance(c));
    QCoreApplication::processEvents();
    check("marking a type at runtime reshapes its rows on the next refresh",
          model.index(0, 0, child(model, {}, "plain")).data().toString() == "speed"
              && model.rowCount(model.index(0, 0, child(model, {}, "plain"))) == 0);
    rpe::TypeBridge::setNamedElement<Unmarked>("");
    model.refresh(rttr::instance(c));
    QCoreApplication::processEvents();
    check("...and unmarking it back", model.index(0, 0, child(model, {}, "plain")).data().toString() == "[0]");

    // Names given as text win over the value's (the mirror's string_view case).
    rpe::PropertyModel given;
    given.bindType(rttr::type::get<Comp>());
    Comp g;
    given.setPropertyValue(QStringLiteral("views"), rttr::variant(g.views),
                           QHash<QString, QStringList> { { QStringLiteral("views"), { "fromSim0", "fromSim1" } } });
    QCoreApplication::processEvents();
    const QModelIndex views = child(given, {}, "views");
    check("names given as text are used (not read from the copy)", given.index(1, 0, views).data().toString() == "fromSim1");
    given.setPropertyValue(QStringLiteral("views.[1].name"), rttr::variant(QStringLiteral("renamed")));
    QCoreApplication::processEvents();
    check("a name updated alone renames the row", given.index(1, 0, views).data().toString() == "renamed");
    given.setPropertyValue(QStringLiteral("views.[1].value"), rttr::variant(8.f));
    QCoreApplication::processEvents();
    check("a value updated alone shows on the row", given.index(1, 1, views).data().toString() == "8");
}

// ── 4. the browser ──────────────────────────────────────────────────────────────
static QModelIndex rowFor(QTreeView* view, const QString& group, const QString& name)
{
    auto* m = view->model();
    QModelIndex found;
    std::function<void(const QModelIndex&, const QString&)> walk = [&](const QModelIndex& parent, const QString& g) {
        for (int r = 0; r < m->rowCount(parent) && !found.isValid(); ++r)
        {
            const QModelIndex i = m->index(r, 0, parent);
            const bool header = i.data(rpe::IsGroupRole).toBool();
            const QString gg = header ? i.data().toString() : g;
            if (!header && i.data().toString() == name && (group.isEmpty() || gg == group))
                found = i;
            walk(i, gg);
        }
    };
    walk({}, QString());
    return found;
}

static void testBrowser(bool mirrorMode)
{
    const std::string tag = mirrorMode ? "mirror: " : "direct: ";
    printf("\n-- browser (%s) --\n", mirrorMode ? "mirror" : "direct");
    flecs::world world;
    rpe::TypeBridge::registerTypes<Comp>();
    auto e = world.entity("E").set<Comp>({});

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
        browser.setEditPolicy(rpe::EditPolicy::WriteBack);
    }
    browser.resize(800, 800);
    browser.show();
    const auto pump = [&](int n) {
        for (int i = 0; i < n; ++i)
        {
            world.progress(0.016f);
            QCoreApplication::processEvents();
            QThread::msleep(3);
        }
    };
    const auto waitFor = [&](const std::function<bool()>& cond) {
        for (int i = 0; i < 300 && !cond(); ++i)
            pump(1);
        return cond();
    };
    browser.selectEntity(e);
    auto* comps = browser.componentList()->findChild<QListWidget*>();
    waitFor([&] { return comps->count() == 1; });
    comps->setCurrentRow(0);
    pump(10);
    auto* ed = browser.propertyEditor();
    auto* view = ed->view();
    view->expandAll();

    check(tag + "named rows with values", waitFor([&] {
              view->expandAll();
              const QModelIndex a = rowFor(view, QString(), "armor");
              return a.isValid() && a.siblingAtColumn(1).data().toString() == "7";
          }));
    check(tag + "string_view / enum / const char* names too", waitFor([&] {
              view->expandAll();
              return rowFor(view, QString(), "Armor").isValid() && rowFor(view, QString(), "maxSpeed").isValid();
          }));

    // Marked at runtime while it's on screen: no reselect needed.
    check(tag + "(the unmarked array shows [i] rows)", waitFor([&] {
              view->expandAll();
              return rowFor(view, QString(), "plain").isValid() && rowFor(view, QString(), "[0]").isValid();
          }));
    rpe::TypeBridge::setNamedElement<Unmarked>("name", "value");
    check(tag + "marking a type at runtime reshapes the open grid", waitFor([&] {
              view->expandAll();
              const QModelIndex plain = rowFor(view, QString(), "plain");
              return plain.isValid() && view->model()->rowCount(plain) == 1
                  && view->model()->index(0, 0, plain).data().toString() == "speed";
          }));
    rpe::TypeBridge::setNamedElement<Unmarked>("");
    check(tag + "...and back", waitFor([&] {
              view->expandAll();
              const QModelIndex plain = rowFor(view, QString(), "plain");
              return plain.isValid() && view->model()->index(0, 0, plain).data().toString() == "[0]";
          }));

    // Edit from the grid.
    view->model()->setData(rowFor(view, QString(), "maxSpeed").siblingAtColumn(1), QVariant::fromValue(rttr::variant(90.0)), Qt::EditRole);
    check(tag + "editing a named row writes that element", waitFor([&] { return e.get<Comp>().stats[2].value == 90.f; }));

    // Reorder in the sim: the rows' names follow.
    std::swap(e.get_mut<Comp>().stats[0], e.get_mut<Comp>().stats[2]);
    e.modified<Comp>();
    check(tag + "a reorder in the world: names and values follow", waitFor([&] {
              view->expandAll();
              const QModelIndex first = view->model()->index(0, 0, rowFor(view, QString(), "stats"));
              return first.data().toString() == "maxSpeed" && first.siblingAtColumn(1).data().toString() == "90";
          }));

    // The single-component filter: by name and by value.
    auto* filter0 = ed->findChild<QLineEdit*>();
    filter0->setText(QStringLiteral("armor"));
    check(tag + "the filter finds a named row by its name", waitFor([&] { return rowFor(view, QString(), "armor").isValid(); }));
    check(tag + "...and hides the others", !rowFor(view, QString(), "speed").isValid());
    filter0->setText(QStringLiteral("90"));
    check(tag + "...and by its value", waitFor([&] { return rowFor(view, QString(), "maxSpeed").isValid(); }));
    filter0->clear();
    pump(5);

    // A real editor on a named row (the delegate path: it holds the row while open).
    view->expandAll();
    {
        const QModelIndex speed = rowFor(view, QString(), "speed").siblingAtColumn(1);
        view->setCurrentIndex(speed);
        view->edit(speed);
        pump(3);
        auto* spin = view->viewport()->findChild<QDoubleSpinBox*>();
        check(tag + "a named row opens its value's editor", spin != nullptr);
        if (spin)
        {
            spin->setValue(44.0);
            QAbstractItemDelegate* d = view->itemDelegateForColumn(1) ? view->itemDelegateForColumn(1) : view->itemDelegate();
            emit d->commitData(spin);
            emit d->closeEditor(spin, QAbstractItemDelegate::NoHint);
        }
        check(tag + "...and its commit writes the element", waitFor([&] {
                  for (const auto& st : e.get<Comp>().stats)
                      if (st.name == "speed")
                          return st.value == 44.f;
                  return false;
              }));
    }

    // Search all components: "armor" shows the row with its value.
    auto* filter = ed->findChild<QLineEdit*>();
    QToolButton* scope = nullptr;
    for (auto* b : ed->findChildren<QToolButton*>())
        if (b->isCheckable())
            scope = b;
    filter->setText(QStringLiteral("armor"));
    scope->click();
    check(tag + "grouped search: the element's row, with its value", waitFor([&] {
              const QModelIndex a = rowFor(view, "Comp", "armor");
              return a.isValid() && a.siblingAtColumn(1).data().toString() == "7";
          }));
    filter->setText(QStringLiteral("90"));
    check(tag + "...found by its VALUE too", waitFor([&] { return rowFor(view, "Comp", "maxSpeed").isValid(); }));
    scope->click();
    filter->clear();
    pump(10);

    if (!mirrorMode)
        return; // pins and names-as-text are mirror features

    // Pins by name, from the grid's menu.
    view->expandAll();
    pump(5);
    const auto pinRow = [&](const QString& name) {
        const QModelIndex idx = rowFor(view, QString(), name);
        view->scrollTo(idx);
        const QPoint pos = view->visualRect(idx).center();
        QTimer::singleShot(0, [] {
            if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
            {
                for (QAction* a : m->actions())
                    if (a->text() == QStringLiteral("Pin to watch list"))
                        a->trigger();
                m->close();
            }
        });
        QMetaObject::invokeMethod(ed, "_onContextMenu", Qt::DirectConnection, Q_ARG(QPoint, pos));
    };
    pinRow("armor");
    pinRow("Armor");
    pump(20);
    auto* pinTree = pins.findChild<QTreeWidget*>();
    const auto pinText = [&](const char* prop) {
        for (int i = 0; pinTree && i < pinTree->topLevelItemCount(); ++i)
            if (pinTree->topLevelItem(i)->text(1) == QString::fromLatin1(prop))
                return pinTree->topLevelItem(i)->text(2);
        return QStringLiteral("<no row>");
    };
    QStringList keys;
    for (const auto& k : pins.pins())
        keys << k.path;
    check(tag + "a named row is pinned by name", keys.contains("stats.{armor}.value") && keys.contains("kinds.{Armor}.value"));
    check(tag + "...shown by name", pinText("Comp.stats.armor") == "7" && pinText("Comp.kinds.Armor") == "7");

    e.get_mut<Comp>().stats.insert(e.get_mut<Comp>().stats.begin(), NV<std::string, float> { "first", 0.f });
    e.get_mut<Comp>().stats[2].value = 75.f; // armor, now at [2]
    e.modified<Comp>();
    check(tag + "the pin follows its element after an insert in front", waitFor([&] { return pinText("Comp.stats.armor") == "75"; }));
    auto& st = e.get_mut<Comp>().stats;
    st.erase(std::remove_if(st.begin(), st.end(), [](const auto& s) { return s.name == "armor"; }), st.end());
    e.modified<Comp>();
    check(tag + "erased: the pin shows —", waitFor([&] { return pinText("Comp.stats.armor") == QStringLiteral("—"); }));
    e.get_mut<Comp>().stats.push_back({ "armor", 11.f });
    e.modified<Comp>();
    check(tag + "added back (elsewhere): the pin is back", waitFor([&] { return pinText("Comp.stats.armor") == "11"; }));
    check(tag + "the grid tints the pinned row", waitFor([&] {
              view->expandAll();
              return rowFor(view, QString(), "armor").data(Qt::ForegroundRole).isValid();
          }));

    // Names as text from the sim.
    {
        rpe::EcsMirror probe; // a second producer, pumped by hand, to see what it sends
        probe.attach(&world, rpe::EcsMirror::PumpMode::Manual);
        probe.setInterest(static_cast<qulonglong>(e.id()), "Comp", { "views", "views.[1].name", "cstrs.[0].name" });
        world.progress(0.016f);
        probe.pump();
        QHash<QString, rpe::MirrorChannel::ValueUpdate> got;
        for (auto& u : probe.channel()->pollValues())
            got.insert(u.path, u);
        check(tag + "an array sent whole comes with its names as text",
              got.value("views").names.value("views") == (QStringList { "speed", "armor" }));
        check(tag + "a string_view name sent alone goes as owned text",
              got.value("views.[1].name").value.get_type() == rttr::type::get<QString>()
                  && got.value("views.[1].name").value.get_value<QString>() == "armor");
        check(tag + "...a const char* one too", got.value("cstrs.[0].name").value.get_type() == rttr::type::get<QString>());
        // The name changes in the sim's own storage: the GUI shows the new text it
        // was SENT, never reading the sim's bytes itself.
        g_owned[1] = "armed"; // same length: a view keeps its own length
        e.modified<Comp>();
        world.progress(0.016f);
        probe.pump();
        bool renamed = false;
        for (auto& u : probe.channel()->pollValues())
            renamed |= u.path == "views.[1].name" && u.value.get_value<QString>() == "armed";
        check(tag + "a name changed in place is sent again, as text", renamed);
        probe.detach();
    }

    mirror.detach();
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::TypeBridge::setNamedElementTemplate("NV", "name", "value");
    rpe::TypeBridge::setNamedElementTemplate("Stat", "name", "value");
    rpe::TypeBridge::setNamedElement<Getter>("name", "value");
    rpe::TypeBridge::setNamedElement<WithUnit>("name", "value");

    testMarking();
    testCore();
    testModel();
    testBrowser(true);
    testBrowser(false);

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

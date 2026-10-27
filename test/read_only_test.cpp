// Read-only values, and move-only components.
//
//  • A getter-only property used to LOOK editable: in mirror mode the typed value
//    showed, then silently snapped back; in direct mode it stayed on screen as a
//    frozen "local edit" while the object never changed. RTTR knew it had no setter
//    — rpe never asked.
//  • A whole TYPE can now be locked (TypeBridge::setReadOnly, or RTTR class
//    metadata), wherever it appears — the component itself, or a struct inside one.
//  • Read-only rows are information, not disabled controls: enabled + selectable,
//    normal text, no editor, the reason in the tooltip. The watch list and the
//    mirror (sim thread) apply the same rule, so neither is a way around it.
//  • registerType<T>() accepts MOVE-ONLY T: nothing in rpe copies a component.
#include <rpe/core/EditorHints.h>
#include <rpe/core/ReadOnly.h>
#include <rpe/core/TypeBridge.h>
#include <rpe/ecs/EcsMirror.h>
#include <rpe/ecs/PinnedPropertiesWidget.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QStyleOptionViewItem>
#include <QTreeWidget>

#include <cstdio>
#include <memory>
#include <type_traits>

// A getter-only property next to a normal one.
struct Meter
{
    int level() const
    {
        return _level;
    }
    int _level = 7;
    float gain = 1.0f;
};

// A getter-only STRUCT property: editing "offset.x" would write into a copy.
struct Vec3
{
    double x = 0, y = 0, z = 0;
};
struct Rig
{
    Vec3 offset() const
    {
        return _offset;
    }
    Vec3 _offset { 1, 2, 3 };
    Vec3 pos { 4, 5, 6 };
};

// Per-property metadata (the pre-existing mechanism).
struct Hinted
{
    int fixed = 1;
    int free = 2;
};

// A type locked by RTTR CLASS metadata.
struct Sealed
{
    int a = 1;
};

// A type locked at runtime through TypeBridge, used INSIDE another component.
struct Tuning
{
    double gain = 1;
};
struct Synth
{
    Tuning tuning;
    int volume = 3;
};

// A move-only component (owns a resource through a move-only base).
namespace audio
{
    class Handle
    {
    public:
        Handle()
            : _res(std::make_unique<int>(1))
        {
        }
        Handle(Handle&&) noexcept = default;
        Handle& operator=(Handle&&) noexcept = default;
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

    private:
        std::unique_ptr<int> _res;
    };
    class Speaker : public Handle
    {
    public:
        float volume = 0.5f;
    };
}
static_assert(!std::is_copy_constructible_v<audio::Speaker>, "the point of the test");

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<Meter>("Meter").property_readonly("level", &Meter::level).property("gain", &Meter::gain);
    registration::class_<Vec3>("Vec3").property("x", &Vec3::x).property("y", &Vec3::y).property("z", &Vec3::z);
    registration::class_<Rig>("Rig").property_readonly("offset", &Rig::offset).property("pos", &Rig::pos);
    registration::class_<Hinted>("Hinted")
        .property("fixed", &Hinted::fixed)(metadata(rpe::hint::ReadOnly, true))
        .property("free", &Hinted::free);
    registration::class_<Sealed>("Sealed")(metadata(rpe::hint::ReadOnly, true)).property("a", &Sealed::a);
    registration::class_<Tuning>("Tuning").property("gain", &Tuning::gain);
    registration::class_<Synth>("Synth").property("tuning", &Synth::tuning).property("volume", &Synth::volume);
    registration::class_<audio::Speaker>("audio::Speaker").property("volume", &audio::Speaker::volume);
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

static bool ro(rttr::type t, const char* path)
{
    return !rpe::readOnlyReason(t, QString::fromLatin1(path)).isEmpty();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    rpe::TypeBridge::registerTypes<Meter, Rig, Hinted, Sealed, Synth>();
    rpe::TypeBridge::registerType<audio::Speaker>(); // must COMPILE for a move-only T

    // ── The rule itself ─────────────────────────────────────────────────────────
    printf("\n-- the rule --\n");
    check("a getter-only property is read-only", ro(rttr::type::get<Meter>(), "level"));
    check("...its writable sibling is not", !ro(rttr::type::get<Meter>(), "gain"));
    check("the reason says why",
          rpe::readOnlyReason(rttr::type::get<Meter>(), QStringLiteral("level")).contains(QStringLiteral("no setter")));
    check("BELOW a getter-only struct property is read-only too (offset.x)", ro(rttr::type::get<Rig>(), "offset.x"));
    check("...a writable struct property's fields are not (pos.x)", !ro(rttr::type::get<Rig>(), "pos.x"));
    check("hint::ReadOnly property metadata still works", ro(rttr::type::get<Hinted>(), "fixed")
                                                             && !ro(rttr::type::get<Hinted>(), "free"));
    check("RTTR CLASS metadata locks the whole type", ro(rttr::type::get<Sealed>(), "a"));

    check("before locking, Tuning inside Synth is editable", !ro(rttr::type::get<Synth>(), "tuning.gain"));
    rpe::TypeBridge::setReadOnly<Tuning>();
    check("TypeBridge::setReadOnly<Tuning> locks it wherever it appears", ro(rttr::type::get<Synth>(), "tuning.gain"));
    check("...but not the component's other fields", !ro(rttr::type::get<Synth>(), "volume"));
    rpe::TypeBridge::setReadOnly<Tuning>(false);
    check("unlocking releases it", !ro(rttr::type::get<Synth>(), "tuning.gain"));

    rpe::TypeBridge::setReadOnly("Synth"); // by NAME — as a host would, for a plugin's type
    check("locking a component type by name locks all of it", ro(rttr::type::get<Synth>(), "volume"));
    rpe::TypeBridge::setReadOnly("Synth", false);
    check("...and unlocking by name releases it", !ro(rttr::type::get<Synth>(), "volume"));

    // ── Move-only components ────────────────────────────────────────────────────
    printf("\n-- move-only --\n");
    check("a move-only type registers with the bridge",
          rpe::TypeBridge::isRegistered(rttr::type::get<audio::Speaker>()));
    {
        audio::Speaker s;
        check("clone() of a move-only type is (documented as) invalid",
              !rpe::TypeBridge::clone(rttr::type::get<audio::Speaker>(), &s).is_valid());
    }
    {
        flecs::world w;
        w.component<audio::Speaker>("audio::Speaker");
        auto e = w.entity("Radio").add<audio::Speaker>();
        rpe::EcsMirror m;
        m.attach(&w);
        m.setInterest(static_cast<qulonglong>(e.id()), QStringLiteral("audio.Speaker"), { QStringLiteral("volume") });
        w.progress(0.016f);
        m.queueEdit(QStringLiteral("volume"), rttr::variant(0.9f));
        w.progress(0.016f);
        check("a move-only component is edited in place through the mirror", e.get<audio::Speaker>().volume == 0.9f);
        m.detach();
    }

    // ── The property grid: shown normally, never an editor ─────────────────────
    printf("\n-- property grid --\n");
    {
        Meter meter;
        rpe::PropertyEditor ed;
        ed.editObject(meter); // DIRECT mode, WriteBack — where the old failure froze a fake edit
        auto* model = ed.model();
        const QModelIndex level = findPath(model, QStringLiteral("level")).siblingAtColumn(1);
        const QModelIndex gain = findPath(model, QStringLiteral("gain")).siblingAtColumn(1);
        const Qt::ItemFlags lf = model->flags(level);
        check("a read-only row is NOT editable", !(lf & Qt::ItemIsEditable));
        check("...but stays enabled and selectable (not a disabled look)",
              (lf & Qt::ItemIsEnabled) && (lf & Qt::ItemIsSelectable));
        check("...and its tooltip gives the reason",
              level.data(Qt::ToolTipRole).toString().contains(QStringLiteral("no setter")));
        check("a writable sibling stays editable", bool(model->flags(gain) & Qt::ItemIsEditable));

        check("setData on a read-only row is refused",
              !model->setData(level, QVariant::fromValue(rttr::variant(42)), Qt::EditRole));
        check("...nothing pretends it was edited: no frozen local edit",
              !findPath(model, QStringLiteral("level")).data(rpe::HasLocalEditRole).toBool());
        check("...the display still shows the real value", level.data().toString() == QStringLiteral("7"));
        check("...and the object is untouched", meter._level == 7);

        // The delegate must not build an editor for it either.
        auto* dlg = ed.view()->itemDelegateForColumn(1);
        auto* proxy = ed.view()->model();
        QWidget* editor = nullptr;
        const QModelIndex plevel = findPath(proxy, QStringLiteral("level")).siblingAtColumn(1);
        if (proxy->flags(plevel) & Qt::ItemIsEditable)
            editor = dlg->createEditor(ed.view()->viewport(), QStyleOptionViewItem(), plevel);
        check("no editor is ever created for a read-only row", editor == nullptr);
        delete editor;
    }
    {
        // A runtime lock reaches an ALREADY-bound tree (cache follows the generation).
        Synth synth;
        rpe::PropertyEditor ed;
        ed.editObject(synth);
        auto* model = ed.model();
        const QModelIndex g = findPath(model, QStringLiteral("tuning.gain")).siblingAtColumn(1);
        check("before the lock, tuning.gain is editable", bool(model->flags(g) & Qt::ItemIsEditable));
        rpe::TypeBridge::setReadOnly<Tuning>();
        check("locking the type takes effect on the live tree", !(model->flags(g) & Qt::ItemIsEditable));
        check("...the component's own field is unaffected",
              bool(model->flags(findPath(model, QStringLiteral("volume")).siblingAtColumn(1)) & Qt::ItemIsEditable));
        rpe::TypeBridge::setReadOnly<Tuning>(false);
    }

    // ── The mirror refuses read-only writes (API callers can't go around the UI)
    printf("\n-- mirror --\n");
    {
        flecs::world w;
        w.component<Meter>("Meter");
        w.component<Synth>("Synth");
        auto meterE = w.entity("M").set<Meter>({});
        auto synthE = w.entity("S").set<Synth>({});
        rpe::EcsMirror m;
        m.attach(&w);

        m.setInterest(static_cast<qulonglong>(meterE.id()), QStringLiteral("Meter"), { QStringLiteral("level") });
        w.progress(0.016f);
        m.queueEdit(QStringLiteral("gain"), rttr::variant(2.0f)); // writable — goes through
        w.progress(0.016f);
        check("the mirror still writes a writable field", meterE.get<Meter>().gain == 2.0f);

        rpe::TypeBridge::setReadOnly<Tuning>();
        m.setInterest(static_cast<qulonglong>(synthE.id()), QStringLiteral("Synth"), { QStringLiteral("volume") });
        w.progress(0.016f);
        m.queueEdit(QStringLiteral("tuning.gain"), rttr::variant(9.0));
        m.queueEdit(QStringLiteral("volume"), rttr::variant(8));
        w.progress(0.016f);
        check("the mirror refuses a write into a LOCKED type", synthE.get<Synth>().tuning.gain == 1.0);
        check("...while writing the component's free field", synthE.get<Synth>().volume == 8);
        rpe::TypeBridge::setReadOnly<Tuning>(false);
        m.detach();
    }

    // ── The watch list applies the same rule ────────────────────────────────────
    printf("\n-- watch list --\n");
    {
        rpe::PinnedPropertiesWidget pins;
        pins.pin(1, QStringLiteral("M"), QStringLiteral("Meter"), QStringLiteral("level"));
        pins.pin(1, QStringLiteral("M"), QStringLiteral("Meter"), QStringLiteral("gain"));
        auto* tree = pins.findChild<QTreeWidget*>();
        QTreeWidgetItem* levelRow = nullptr;
        QTreeWidgetItem* gainRow = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
        {
            if (tree->topLevelItem(i)->text(1).endsWith(QStringLiteral(".level")))
                levelRow = tree->topLevelItem(i);
            if (tree->topLevelItem(i)->text(1).endsWith(QStringLiteral(".gain")))
                gainRow = tree->topLevelItem(i);
        }
        check("a pinned read-only leaf is not editable", levelRow && !(levelRow->flags() & Qt::ItemIsEditable));
        check("...its value tooltip gives the reason",
              levelRow && levelRow->toolTip(2).contains(QStringLiteral("no setter")));
        check("a pinned writable leaf stays editable", gainRow && (gainRow->flags() & Qt::ItemIsEditable));
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

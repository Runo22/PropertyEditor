// Bitmask (flags) enum support: decomposed display, the multi-check editor,
// and writing combined values back through FlagsBridge (RPE_REGISTER_FLAGS).
#include <rpe/core/EditorHints.h>
#include <rpe/core/FlagsSupport.h>
#include <rpe/core/TypeRenderer.h>
#include <rpe/gui/EditorWidgets.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QTreeView>

#include <cstdint>
#include <cstdio>

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

namespace
{
    enum class Damage : uint32_t
    {
        None = 0,
        Fire = 1,
        Ice = 2,
        Poison = 4,
        Shock = 8,
        All = 15
    };

    // A plain (non-flags) enum: must keep the single-choice combo behaviour.
    enum class Quality : int
    {
        Low = 1,
        Mid = 2,
        High = 4
    };

    struct Mob
    {
        Damage dmg = Damage::None;
        Quality quality = Quality::Low;
    };
} // namespace

RTTR_REGISTRATION
{
    rttr::registration::enumeration<Damage>("Damage")(
        rttr::value("None", Damage::None), rttr::value("Fire", Damage::Fire),
        rttr::value("Ice", Damage::Ice), rttr::value("Poison", Damage::Poison),
        rttr::value("Shock", Damage::Shock), rttr::value("All", Damage::All));
    rttr::registration::enumeration<Quality>("Quality")(
        rttr::value("Low", Quality::Low), rttr::value("Mid", Quality::Mid),
        rttr::value("High", Quality::High));
    rttr::registration::class_<Mob>("Mob")
        .property("dmg", &Mob::dmg)(rttr::metadata(rpe::hint::Flags, true))
        .property("quality", &Mob::quality);

    RPE_REGISTER_FLAGS(Damage); // enables writing combined masks
}

static QModelIndex findByPath(QAbstractItemModel* m, const QString& path, const QModelIndex& parent = {})
{
    for (int r = 0; r < m->rowCount(parent); ++r)
    {
        const QModelIndex idx = m->index(r, 0, parent);
        if (idx.data(rpe::PropertyPathRole).toString() == path)
            return idx;
        const QModelIndex sub = findByPath(m, path, idx);
        if (sub.isValid())
            return sub;
    }
    return {};
}

static QString valueText(QAbstractItemModel* m, const QString& path)
{
    const QModelIndex idx = findByPath(m, path);
    return idx.isValid() ? idx.siblingAtColumn(1).data(Qt::DisplayRole).toString() : QStringLiteral("<no row>");
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    using rpe::TypeRenderer;

    // ── decomposition (no registration needed for display) ─────────────────────
    {
        check("single flag shows its name",
              TypeRenderer::flagsToDisplayString(rttr::variant(Damage::Fire)) == QStringLiteral("Fire"));
        check("combined shows names joined",
              TypeRenderer::flagsToDisplayString(rttr::variant(Damage(5))) == QStringLiteral("Fire | Poison"));
        check("named umbrella (All=15) wins over its bits",
              TypeRenderer::flagsToDisplayString(rttr::variant(Damage::All)) == QStringLiteral("All"));
        check("zero shows its named value",
              TypeRenderer::flagsToDisplayString(rttr::variant(Damage::None)) == QStringLiteral("None"));
        check("leftover bits show as hex",
              TypeRenderer::flagsToDisplayString(rttr::variant(Damage(1 | 16)))
                  == QStringLiteral("Fire | 0x10"));
        check("enumBits reads the integer", TypeRenderer::enumBits(rttr::variant(Damage(6))) == 6);
    }

    // ── FlagsBridge builds arbitrary masks (RTTR convert can't) ────────────────
    {
        check("FlagsBridge registered for Damage", rpe::FlagsBridge::canBuild(rttr::type::get<Damage>()));
        check("FlagsBridge NOT registered for Quality", !rpe::FlagsBridge::canBuild(rttr::type::get<Quality>()));
        const rttr::variant built = rpe::FlagsBridge::build(rttr::type::get<Damage>(), 5);
        check("built variant has the enum type", built.get_type() == rttr::type::get<Damage>());
        check("built variant carries the bits", TypeRenderer::enumBits(built) == 5);
    }

    Mob mob;
    rpe::PropertyEditor editor;
    editor.editObject(mob); // WriteBack

    auto* model = editor.model();
    auto* view = editor.view();
    auto* proxy = static_cast<QAbstractItemModel*>(view->model());

    // ── model display + role ───────────────────────────────────────────────────
    {
        mob.dmg = Damage(Damage::Fire); // set via assignment below through refresh
        mob.dmg = static_cast<Damage>(5);
        editor.refresh(rttr::instance(mob));
        check("flags property displays decomposed", valueText(model, QStringLiteral("dmg")) == QStringLiteral("Fire | Poison"));
        check("FlagsRole true for the flags property", findByPath(model, QStringLiteral("dmg")).data(rpe::FlagsRole).toBool());
        check("FlagsRole false for a plain enum", !findByPath(model, QStringLiteral("quality")).data(rpe::FlagsRole).toBool());
        check("plain enum still shows its single name", valueText(model, QStringLiteral("quality")) == QStringLiteral("Low"));
    }

    // ── the multi-check editor ─────────────────────────────────────────────────
    {
        auto* dlg = view->itemDelegateForColumn(1);
        const QModelIndex dmgIdx = findByPath(proxy, QStringLiteral("dmg")).siblingAtColumn(1);
        QWidget* ed = dlg->createEditor(view->viewport(), QStyleOptionViewItem(), dmgIdx);
        auto* fe = qobject_cast<rpe::FlagsEditor*>(ed);
        check("flags property gets a FlagsEditor", fe != nullptr);
        if (fe)
        {
            dlg->setEditorData(fe, dmgIdx);
            check("editor pre-checked to current bits (Fire|Poison)", fe->bits() == 5);

            // Toggle to Fire | Ice | Shock = 1 | 2 | 8 = 11 by driving the model.
            auto* im = qobject_cast<QStandardItemModel*>(fe->model());
            check("editor model built (named flags, minus the 0 entry)", im && im->rowCount() == 5);
            fe->setBits(11);
            check("setBits reflects in bits()", fe->bits() == 11);

            dlg->setModelData(fe, proxy, dmgIdx);
            check("combined mask written back to the object", static_cast<uint32_t>(mob.dmg) == 11u);
            check("display follows the committed mask",
                  valueText(model, QStringLiteral("dmg")) == QStringLiteral("Fire | Ice | Shock"));
        }
        delete ed;
    }

    // ── clearing every box writes the zero value ───────────────────────────────
    {
        auto* dlg = view->itemDelegateForColumn(1);
        const QModelIndex dmgIdx = findByPath(proxy, QStringLiteral("dmg")).siblingAtColumn(1);
        QWidget* ed = dlg->createEditor(view->viewport(), QStyleOptionViewItem(), dmgIdx);
        auto* fe = qobject_cast<rpe::FlagsEditor*>(ed);
        if (fe)
        {
            fe->setBits(0);
            dlg->setModelData(fe, proxy, dmgIdx);
            check("empty selection writes zero", static_cast<uint32_t>(mob.dmg) == 0u);
            check("zero displays as its named value", valueText(model, QStringLiteral("dmg")) == QStringLiteral("None"));
        }
        delete ed;
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

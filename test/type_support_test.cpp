// New type support: u16/u32 strings, QDateTime, chrono durations,
// std::map / std::unordered_map rows, and shared_ptr<T> expansion.
#include <rpe/core/RttrBridge.h>
#include <rpe/core/TypeRenderer.h>

#include <rttr/registration.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <QApplication>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QStyleOptionViewItem>
#include <QTreeView>

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

namespace
{
    struct Inner
    {
        int x = 41;
    };

    struct Zoo
    {
        std::u16string u16 { u"unicode16" };
        std::u32string u32 { U"unicode32" };
        QDateTime when { QDate(2026, 7, 20), QTime(12, 30, 0) };
        std::chrono::milliseconds cooldown { 250 };
        std::map<std::string, int> scores { { "alice", 1 }, { "bob", 2 } };
        std::unordered_map<int, float> weights { { 7, 0.5f }, { 3, 1.25f } };
        std::shared_ptr<Inner> ptr = std::make_shared<Inner>();
        std::shared_ptr<Inner> nothing; // stays null — must not crash anything
    };
} // namespace

RTTR_REGISTRATION
{
    rttr::registration::class_<Inner>("Inner").property("x", &Inner::x);
    rttr::registration::class_<Zoo>("Zoo")
        .property("u16", &Zoo::u16)
        .property("u32", &Zoo::u32)
        .property("when", &Zoo::when)
        .property("cooldown", &Zoo::cooldown)
        .property("scores", &Zoo::scores)
        .property("weights", &Zoo::weights)
        .property("ptr", &Zoo::ptr)
        .property("nothing", &Zoo::nothing);
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

    // ── display strings ────────────────────────────────────────────────────────
    {
        check("u16string renders", TypeRenderer::toDisplayString(rttr::variant(std::u16string(u"selam"))) == QStringLiteral("selam"));
        check("u32string renders", TypeRenderer::toDisplayString(rttr::variant(std::u32string(U"selam"))) == QStringLiteral("selam"));
        check("QDateTime renders",
              TypeRenderer::toDisplayString(rttr::variant(QDateTime(QDate(2026, 7, 20), QTime(12, 30, 0))))
                  == QStringLiteral("2026-07-20 12:30:00"));
        check("chrono ms renders with unit",
              TypeRenderer::toDisplayString(rttr::variant(std::chrono::milliseconds(250))) == QStringLiteral("250 ms"));
        check("chrono s renders with unit",
              TypeRenderer::toDisplayString(rttr::variant(std::chrono::seconds(9))) == QStringLiteral("9 s"));
        check("all are inline-editable",
              TypeRenderer::isInlineEditable(rttr::type::get<std::u16string>())
                  && TypeRenderer::isInlineEditable(rttr::type::get<std::u32string>())
                  && TypeRenderer::isInlineEditable(rttr::type::get<QDateTime>())
                  && TypeRenderer::isInlineEditable(rttr::type::get<std::chrono::milliseconds>()));
        check("map is expandable, not inline",
              TypeRenderer::isExpandable(rttr::type::get<std::map<std::string, int>>())
                  && !TypeRenderer::isInlineEditable(rttr::type::get<std::map<std::string, int>>()));
    }

    Zoo zoo;
    rpe::PropertyEditor editor;
    editor.editObject(zoo); // WriteBack

    auto* model = editor.model();
    auto* view = editor.view();
    auto* proxy = static_cast<QAbstractItemModel*>(view->model());

    // ── std::map rows ──────────────────────────────────────────────────────────
    {
        check("map row shows its count", valueText(model, QStringLiteral("scores")) == QStringLiteral("[2]"));
        check("map keys become child rows (sorted)",
              findByPath(model, QStringLiteral("scores.[alice]")).isValid()
                  && findByPath(model, QStringLiteral("scores.[bob]")).isValid());
        check("map child shows bare key as name",
              findByPath(model, QStringLiteral("scores.[alice]")).data(Qt::DisplayRole).toString() == QStringLiteral("alice"));
        check("map values display", valueText(model, QStringLiteral("scores.[alice]")) == QStringLiteral("1"));

        const QModelIndex bob = findByPath(model, QStringLiteral("scores.[bob]")).siblingAtColumn(1);
        check("map value edit succeeds",
              model->setData(bob, QVariant::fromValue(rttr::variant(5)), Qt::EditRole));
        check("map value edit reached the object", zoo.scores["bob"] == 5);

        // Key-set change → rows rebuild on the next refresh.
        zoo.scores["carol"] = 9;
        editor.refresh(rttr::instance(zoo));
        check("new key appears after refresh",
              findByPath(model, QStringLiteral("scores.[carol]")).isValid()
                  && valueText(model, QStringLiteral("scores")) == QStringLiteral("[3]"));
    }

    // ── std::unordered_map rows (sorted for a stable UI) ───────────────────────
    {
        const QModelIndex wNode = findByPath(model, QStringLiteral("weights"));
        check("unordered_map rows sorted by key string",
              model->index(0, 0, wNode).data(Qt::DisplayRole).toString() == QStringLiteral("3")
                  && model->index(1, 0, wNode).data(Qt::DisplayRole).toString() == QStringLiteral("7"));
        check("int-keyed value readable via bridge path",
              rpe::bridge::getValueByPath(rttr::instance(zoo), QStringLiteral("weights.[7]")).to_float() == 0.5f);
        const QModelIndex w7 = findByPath(model, QStringLiteral("weights.[7]")).siblingAtColumn(1);
        check("int-keyed value edit succeeds",
              model->setData(w7, QVariant::fromValue(rttr::variant(2.5f)), Qt::EditRole));
        check("int-keyed edit reached the object", zoo.weights[7] == 2.5f);
    }

    // ── shared_ptr<T>: expands to the pointee's fields, edits in place ─────────
    {
        check("shared_ptr expands to pointee fields", findByPath(model, QStringLiteral("ptr.x")).isValid());
        check("pointee value displays", valueText(model, QStringLiteral("ptr.x")) == QStringLiteral("41"));
        const QModelIndex px = findByPath(model, QStringLiteral("ptr.x")).siblingAtColumn(1);
        check("pointee edit succeeds", model->setData(px, QVariant::fromValue(rttr::variant(99)), Qt::EditRole));
        check("edit mutated the SAME object the shared_ptr owns", zoo.ptr->x == 99);

        // Null shared_ptr: schema rows exist, values stay blank, nothing crashes.
        editor.refresh(rttr::instance(zoo));
        check("null shared_ptr row exists with blank value",
              findByPath(model, QStringLiteral("nothing.x")).isValid()
                  && valueText(model, QStringLiteral("nothing.x")).isEmpty());
    }

    // ── delegate editors: chrono spin (with unit suffix) + QDateTimeEdit ───────
    {
        auto* dlg = view->itemDelegateForColumn(1);

        const QModelIndex cd = findByPath(proxy, QStringLiteral("cooldown")).siblingAtColumn(1);
        QWidget* ed = dlg->createEditor(view->viewport(), QStyleOptionViewItem(), cd);
        auto* sb = qobject_cast<QDoubleSpinBox*>(ed);
        check("chrono gets a spin editor with unit suffix", sb && sb->suffix() == QStringLiteral(" ms"));
        if (sb)
        {
            dlg->setEditorData(sb, cd);
            check("chrono editor pre-filled with count", sb->value() == 250.0);
            sb->setValue(400);
            dlg->setModelData(sb, proxy, cd);
            check("chrono commit reached the object", zoo.cooldown == std::chrono::milliseconds(400));
            check("chrono row re-displays with unit", valueText(model, QStringLiteral("cooldown")) == QStringLiteral("400 ms"));
        }
        delete ed;

        const QModelIndex wh = findByPath(proxy, QStringLiteral("when")).siblingAtColumn(1);
        QWidget* ed2 = dlg->createEditor(view->viewport(), QStyleOptionViewItem(), wh);
        auto* dte = qobject_cast<QDateTimeEdit*>(ed2);
        check("QDateTime gets a date-time editor", dte != nullptr);
        if (dte)
        {
            dlg->setEditorData(dte, wh);
            check("date editor pre-filled", dte->dateTime() == QDateTime(QDate(2026, 7, 20), QTime(12, 30, 0)));
            dte->setDateTime(QDateTime(QDate(2027, 1, 2), QTime(3, 4, 5)));
            dlg->setModelData(dte, proxy, wh);
            check("date commit reached the object", zoo.when == QDateTime(QDate(2027, 1, 2), QTime(3, 4, 5)));
        }
        delete ed2;

        const QModelIndex u16i = findByPath(proxy, QStringLiteral("u16")).siblingAtColumn(1);
        QWidget* ed3 = dlg->createEditor(view->viewport(), QStyleOptionViewItem(), u16i);
        auto* le = qobject_cast<QLineEdit*>(ed3);
        check("u16string gets a line edit", le != nullptr);
        if (le)
        {
            dlg->setEditorData(le, u16i);
            check("u16 editor pre-filled", le->text() == QStringLiteral("unicode16"));
            le->setText(QStringLiteral("yeni16"));
            dlg->setModelData(le, proxy, u16i);
            check("u16 commit reached the object", zoo.u16 == u"yeni16");
        }
        delete ed3;
    }

    // ── u32string via model setData ────────────────────────────────────────────
    {
        const QModelIndex u32i = findByPath(model, QStringLiteral("u32")).siblingAtColumn(1);
        check("u32 edit via model succeeds",
              model->setData(u32i, QVariant::fromValue(rttr::variant(std::u32string(U"yeni32"))), Qt::EditRole));
        check("u32 edit reached the object", zoo.u32 == U"yeni32");
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

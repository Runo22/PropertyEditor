// string_view read-only display, RPE_REGISTER_PAIR, and the collapsed-struct
// "[a, b]" summary (hidden when the row is expanded).
#include <rpe/core/PairSupport.h>
#include <rpe/core/TypeRenderer.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <QApplication>
#include <QTreeView>

#include <cstdio>
#include <string_view>
#include <utility>

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

namespace
{
    struct Wide
    {
        int a = 1, b = 2, c = 3, d = 4, e = 5;
    };

    struct Holder
    {
        std::pair<int, double> range { 3, 1.5 };
        Wide wide;
        std::string_view label { "hello" };
    };
} // namespace

RTTR_REGISTRATION
{
    RPE_REGISTER_PAIR(int, double);
    rttr::registration::class_<Wide>("Wide")
        .property("a", &Wide::a)
        .property("b", &Wide::b)
        .property("c", &Wide::c)
        .property("d", &Wide::d)
        .property("e", &Wide::e);
    rttr::registration::class_<Holder>("Holder")
        .property("range", &Holder::range)
        .property("wide", &Holder::wide)
        .property("label", &Holder::label);
}

// Depth-first search for the column-0 index whose PropertyPathRole == path.
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

    // ── string_view: displayed as text, deliberately not editable ──────────────
    {
        const std::string_view sv { "hello" };
        check("string_view renders its text",
              rpe::TypeRenderer::toDisplayString(rttr::variant(sv)) == QStringLiteral("hello"));
        check("null string_view renders empty",
              rpe::TypeRenderer::toDisplayString(rttr::variant(std::string_view {})).isEmpty());
        check("string_view is NOT inline-editable",
              !rpe::TypeRenderer::isInlineEditable(rttr::type::get<std::string_view>()));
    }

    Holder holder;
    rpe::PropertyEditor editor;
    editor.editObject(holder); // WriteBack + expandToDepth(0)

    auto* model = editor.model();
    auto* view = editor.view();
    auto* proxy = static_cast<QAbstractItemModel*>(view->model());

    // ── pair registered via the macro shows up as a two-row struct ─────────────
    {
        check("pair row exists with children first/second",
              findByPath(model, QStringLiteral("range.first")).isValid()
                  && findByPath(model, QStringLiteral("range.second")).isValid());
        check("string_view leaf is not editable in the grid",
              !(model->flags(findByPath(model, QStringLiteral("label")).siblingAtColumn(1)) & Qt::ItemIsEditable));
    }

    // ── expanded (bindType auto-expands depth 0): no summary on the parent ─────
    {
        check("expanded pair row shows no summary", valueText(model, QStringLiteral("range")).isEmpty());
        check("children carry the values",
              valueText(model, QStringLiteral("range.first")) == QStringLiteral("3")
                  && valueText(model, QStringLiteral("range.second")) == QStringLiteral("1.5"));
    }

    // ── collapse via the VIEW → model summary appears ──────────────────────────
    {
        const QModelIndex rangeProxy = findByPath(proxy, QStringLiteral("range"));
        view->collapse(rangeProxy);
        check("collapsed pair row shows [3, 1.5]",
              valueText(model, QStringLiteral("range")) == QStringLiteral("[3, 1.5]"));

        const QModelIndex wideProxy = findByPath(proxy, QStringLiteral("wide"));
        view->collapse(wideProxy);
        check("collapsed 5-field struct stays blank (>4 fields)",
              valueText(model, QStringLiteral("wide")).isEmpty());
    }

    // ── collapsed small struct's leaves are still watched (mirror feed) ────────
    {
        const QStringList paths = editor.visibleLeafPaths(true);
        check("collapsed pair leaves included in watch paths",
              paths.contains(QStringLiteral("range.first")) && paths.contains(QStringLiteral("range.second")));
        check("collapsed wide struct's leaves NOT watched",
              !paths.contains(QStringLiteral("wide.a")));
    }

    // ── WriteBack edit through the pair path + summary follows ─────────────────
    {
        const QModelIndex firstIdx = findByPath(model, QStringLiteral("range.first")).siblingAtColumn(1);
        check("setData on range.first succeeds",
              model->setData(firstIdx, QVariant::fromValue(rttr::variant(7)), Qt::EditRole));
        check("edit reached the object", holder.range.first == 7);
        check("collapsed summary follows the edit",
              valueText(model, QStringLiteral("range")) == QStringLiteral("[7, 1.5]"));
    }

    // ── child change while collapsed repaints the parent's value cell ──────────
    {
        const QModelIndex rangeSrcVal = findByPath(model, QStringLiteral("range")).siblingAtColumn(1);
        bool parentRepainted = false;
        QObject::connect(model, &QAbstractItemModel::dataChanged, model,
                         [&](const QModelIndex& tl, const QModelIndex& br, const QVector<int>&) {
                             if (tl == rangeSrcVal && br == rangeSrcVal)
                                 parentRepainted = true;
                         });
        holder.range.second = 9.25;
        editor.refresh(rttr::instance(holder));
        check("summary updates from a live refresh",
              valueText(model, QStringLiteral("range")) == QStringLiteral("[7, 9.25]"));
        check("parent value cell got dataChanged", parentRepainted);
    }

    // ── expand again via the VIEW → summary hides, children show ───────────────
    {
        const QModelIndex rangeProxy = findByPath(proxy, QStringLiteral("range"));
        view->expand(rangeProxy);
        check("re-expanding hides the summary", valueText(model, QStringLiteral("range")).isEmpty());
        check("children still live",
              valueText(model, QStringLiteral("range.second")) == QStringLiteral("9.25"));
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

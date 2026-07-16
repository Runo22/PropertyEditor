// VariantEditor::updateVariant — values-only refresh of the owned copy: no
// model reset (schema/expansion/open editors survive), locally-edited rows are
// left alone, and a type change falls back to a full rebind.
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>
#include <rpe/gui/VariantEditor.h>

#include <rttr/registration.h>

#include <QApplication>

#include <cstdio>

struct Camera
{
    double fov = 60.0;
    double aperture = 2.8;
};
struct Other
{
    int x = 1;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Camera>("Camera").property("fov", &Camera::fov).property("aperture", &Camera::aperture);
    rttr::registration::class_<Other>("Other").property("x", &Other::x);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static QString cell(rpe::PropertyModel* m, int row)
{
    return m->index(row, 1, {}).data(Qt::DisplayRole).toString();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    rpe::VariantEditor editor;
    rpe::PropertyModel* model = editor.editor()->model();

    int resets = 0;
    QObject::connect(model, &QAbstractItemModel::modelReset, model, [&] { ++resets; });

    editor.setVariant(rttr::variant(Camera { 60.0, 2.8 }));
    const int resetsAfterBind = resets;
    check("setVariant binds and shows values", cell(model, 0) == QStringLiteral("60"));

    // ── Values-only update: NO model reset, values change ─────────────────────
    editor.updateVariant(rttr::variant(Camera { 35.0, 4.0 }));
    check("updateVariant refreshes values", cell(model, 0) == QStringLiteral("35") && cell(model, 1) == QStringLiteral("4"));
    check("updateVariant does NOT reset the model", resets == resetsAfterBind);

    // ── A row being edited (pinned as a local edit) is left alone ─────────────
    model->beginLocalEdit(QStringLiteral("aperture")); // what an open editor does
    editor.updateVariant(rttr::variant(Camera { 20.0, 9.9 }));
    check("live update flows into unpinned rows", cell(model, 0) == QStringLiteral("20"));
    check("the row being edited is not stomped", cell(model, 1) == QStringLiteral("4"));
    model->resetNode(QStringLiteral("aperture"));

    // ── Edited copy is readable back via variant() ─────────────────────────────
    check("variant() reflects the latest update", editor.variant().get_value<Camera>().fov == 20.0);

    // ── Type change falls back to a full rebind ────────────────────────────────
    editor.updateVariant(rttr::variant(Other { 7 }));
    check("type change falls back to setVariant (model reset)", resets > resetsAfterBind);
    check("new type shows after fallback", cell(model, 0) == QStringLiteral("7"));

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

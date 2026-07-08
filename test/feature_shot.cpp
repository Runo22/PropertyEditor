// Renders the two new features to PNGs for review, offscreen (no display):
//   <prefix>_array.png  — a std::vector expanded into per-element rows
//   <prefix>_files.png  — std::filesystem::path rows with a FilePathEditor open
//                         (line edit + "Browse…" button), plus a folder-picker row
#include <rpe/core/EditorHints.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QAbstractItemModel>
#include <QApplication>
#include <QHeaderView>
#include <QPixmap>
#include <QTreeView>

#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

struct Material
{
    fs::path albedoTexture = "textures/rock_albedo.png"; // open-file editor (auto)
    fs::path normalMap = "textures/rock_normal.png";     // save-file editor (hint)
    fs::path contentRoot = "assets/materials";           // directory editor (hint)
    std::vector<int> mipLevels = { 2048, 1024, 512, 256, 64 };
    std::vector<double> roughness = { 0.1, 0.35, 0.8 };
};

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<Material>("Material")
        .property("albedoTexture", &Material::albedoTexture)
        .property("normalMap", &Material::normalMap)(metadata(rpe::hint::Editor, rpe::editor::SaveFile))
        .property("contentRoot", &Material::contentRoot)(metadata(rpe::hint::Editor, rpe::editor::Directory))
        .property("mipLevels", &Material::mipLevels)
        .property("roughness", &Material::roughness);
}

// Depth-first search of the view's (proxy) model for the row whose path == name.
static QModelIndex findByPath(QAbstractItemModel* m, const QString& name, const QModelIndex& parent = {})
{
    for (int r = 0; r < m->rowCount(parent); ++r)
    {
        const QModelIndex idx = m->index(r, 0, parent);
        if (idx.data(rpe::PropertyPathRole).toString() == name)
            return idx;
        if (const QModelIndex hit = findByPath(m, name, idx); hit.isValid())
            return hit;
    }
    return {};
}

static void saveShot(QWidget* w, const QString& path)
{
    const QPixmap pm = w->grab();
    printf("%s %s (%dx%d)\n", pm.save(path, "PNG") ? "saved" : "FAILED", path.toUtf8().constData(), pm.width(), pm.height());
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QString prefix = argc > 1 ? QString::fromUtf8(argv[1]) : QStringLiteral("feature");

    Material mat;

    rpe::PropertyEditor editor;
    editor.setToolbarVisible(false);
    editor.editObject(mat); // bind + write-back edit + refresh (builds array rows)
    editor.expandAll();
    editor.resize(440, 460);

    QTreeView* view = editor.view();
    view->header()->resizeSection(0, 210);
    editor.show();
    QCoreApplication::processEvents();

    // ── 1) Arrays expanded into element rows ──────────────────────────────────
    saveShot(&editor, prefix + QStringLiteral("_array.png"));

    // ── 2) FilePathEditor open on the albedo (open-file) row ──────────────────
    if (const QModelIndex idx = findByPath(view->model(), QStringLiteral("albedoTexture")); idx.isValid())
    {
        const QModelIndex valueIdx = idx.siblingAtColumn(1);
        view->setCurrentIndex(valueIdx);
        view->openPersistentEditor(valueIdx);
        QCoreApplication::processEvents();
        saveShot(&editor, prefix + QStringLiteral("_files.png"));
        view->closePersistentEditor(valueIdx);
    }

    return 0;
}

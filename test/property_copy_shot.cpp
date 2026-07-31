// Renders the property grid and its real right-click menu (Copy value / name /
// "name = value" at the top). Usage: rpe_property_copy_shot <output-dir>
#include <rpe/core/TypeBridge.h>
#include <rpe/gui/DarkStyle.h>
#include <rpe/gui/PropertyEditor.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QColor>
#include <QMenu>
#include <QPixmap>
#include <QTimer>
#include <QTreeView>

#include <cstdio>
#include <functional>

namespace
{
    struct Vec3
    {
        float x = 0.0f, y = 1.0f, z = 0.0f;
    };
    struct Light
    {
        double intensity = 7.5;
        bool castShadows = true;
        QColor tint = QColor(255, 200, 120);
        Vec3 direction;
    };
} // namespace

RTTR_REGISTRATION
{
    rttr::registration::class_<Vec3>("Vec3")
        .property("x", &Vec3::x)
        .property("y", &Vec3::y)
        .property("z", &Vec3::z);
    rttr::registration::class_<Light>("Light")
        .property("intensity", &Light::intensity)
        .property("castShadows", &Light::castShadows)
        .property("tint", &Light::tint)
        .property("direction", &Light::direction);
}

static void save(QWidget* w, const QString& path)
{
    const QPixmap pm = w->grab();
    printf("%s %s (%dx%d)\n", pm.save(path, "PNG") ? "saved" : "FAILED", path.toUtf8().constData(), pm.width(), pm.height());
}

static QPoint posOf(QTreeView* view, const QString& name)
{
    auto* model = view->model();
    std::function<QModelIndex(const QModelIndex&)> find = [&](const QModelIndex& parent) -> QModelIndex {
        for (int r = 0; r < model->rowCount(parent); ++r)
        {
            const QModelIndex idx = model->index(r, 0, parent);
            if (idx.data(Qt::DisplayRole).toString() == name)
                return idx;
            const QModelIndex sub = find(idx);
            if (sub.isValid())
                return sub;
        }
        return {};
    };
    const QModelIndex idx = find(QModelIndex());
    return idx.isValid() ? view->visualRect(idx).center() : QPoint(0, 0);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setStyleSheet(rpe::darkStyleSheet());
    const QString dir = argc > 1 ? QString::fromUtf8(argv[1]) : QStringLiteral(".");

    Light light;
    rpe::PropertyEditor editor;
    editor.setPinningEnabled(true); // so "Pin to watch list" also shows
    editor.editObject(light);
    editor.setToolbarVisible(false);
    editor.resize(300, 220);
    editor.show();
    QApplication::processEvents();
    editor.expandAll();
    QApplication::processEvents();

    save(&editor, dir + QStringLiteral("/property_grid.png"));

    // Grab the REAL context menu, opened on the "intensity" row.
    const QPoint pos = posOf(editor.view(), QStringLiteral("intensity"));
    QTimer::singleShot(0, [&] {
        if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        {
            m->grab().save(dir + QStringLiteral("/property_copy_menu.png"), "PNG");
            printf("saved %s\n", (dir + QStringLiteral("/property_copy_menu.png")).toUtf8().constData());
            m->close();
        }
    });
    QMetaObject::invokeMethod(&editor, "_onContextMenu", Qt::DirectConnection, Q_ARG(QPoint, pos));

    return 0;
}

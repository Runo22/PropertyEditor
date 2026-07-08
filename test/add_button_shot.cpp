// Renders the component list's "Add" button on a DARK background, so the green
// (coloured) plus icon can be reviewed for visibility. Offscreen; no display.
//   usage: rpe_add_button_shot <out.png>
#include <rpe/ecs/ComponentListWidget.h>

#include <QApplication>
#include <QPixmap>

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QString out = argc > 1 ? QString::fromUtf8(argv[1]) : QStringLiteral("add_button.png");

    // A dark theme, like the host the user targets.
    app.setStyleSheet(QStringLiteral(
        "QWidget { background: #232629; color: #d6d9dd; }"
        "QListWidget { background: #1b1e20; border: 1px solid #33373b; }"));

    rpe::ComponentListWidget w;
    w.setComponentEditingEnabled(true); // reveals the Add button
    w.setComponentNames({ QStringLiteral("Transform"), QStringLiteral("Health"), QStringLiteral("RigidBody") });
    w.setAddableComponents({ QStringLiteral("Mesh"), QStringLiteral("Collider") });
    w.resize(320, 200);
    w.show();
    QCoreApplication::processEvents();

    const QPixmap pm = w.grab();
    printf("%s %s (%dx%d)\n", pm.save(out, "PNG") ? "saved" : "FAILED", out.toUtf8().constData(), pm.width(), pm.height());

    // A 3x zoom of the header + first rows, so the green plus and the red trash
    // bins both read clearly.
    const QPixmap crop = pm.copy(0, 0, pm.width(), 78);
    crop.scaled(crop.width() * 3, crop.height() * 3, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        .save(out + QStringLiteral(".zoom.png"), "PNG");
    return 0;
}

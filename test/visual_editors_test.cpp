// Three editor additions:
//
//  • Inline vector row — a struct of 2–4 writable numbers (Vec3, a size, a float
//    colour) edits on its OWN row: one box per field, x/y/z/w (r/g/b/a) with axis
//    colours. Only the fields the user changed are written, each through its own
//    path — read-only and mirror routing apply per field as usual.
//  • Drag-to-scrub — drag a number's NAME sideways to change it.
//  • Colour swatch for STRING colours (the rpe::editor::Color hint), like QColor.
#include <rpe/core/EditorHints.h>
#include <rpe/core/TypeBridge.h>
#include <rpe/gui/EditorWidgets.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QColor>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QSpinBox>
#include <QStyleOptionViewItem>
#include <QTreeView>

#include <cstdio>

struct Vec3
{
    double x = 1, y = 2, z = 3;
};
struct Size2
{
    int width = 640;
    int height = 480;
};
struct Lens // NOT a vector: one field is a string
{
    double focal = 35;
    std::string name = "prime";
};
struct Sealed // a vector-shaped struct whose y is read-only → not inline-editable
{
    double x = 0;
    double y() const
    {
        return 0;
    }
};
struct Camera
{
    Vec3 position;
    Size2 resolution;
    Lens lens;
    Sealed sealed;
    double fov = 60;
    int samples = 4;
    bool enabled = true;
    std::string tint = "#ff336699";
};

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<Vec3>("Vec3").property("x", &Vec3::x).property("y", &Vec3::y).property("z", &Vec3::z);
    registration::class_<Size2>("Size2").property("width", &Size2::width).property("height", &Size2::height);
    registration::class_<Lens>("Lens").property("focal", &Lens::focal).property("name", &Lens::name);
    registration::class_<Sealed>("Sealed").property("x", &Sealed::x).property_readonly("y", &Sealed::y);
    registration::class_<Camera>("Camera")
        .property("position", &Camera::position)
        .property("resolution", &Camera::resolution)
        .property("lens", &Camera::lens)
        .property("sealed", &Camera::sealed)
        .property("fov", &Camera::fov)(metadata(rpe::hint::Min, 10.0), metadata(rpe::hint::Max, 120.0),
                                       metadata(rpe::hint::Step, 1.0))
        .property("samples", &Camera::samples)
        .property("enabled", &Camera::enabled)
        .property("tint", &Camera::tint)(metadata(rpe::hint::Editor, rpe::editor::Color));
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

static void mouse(QWidget* w, QEvent::Type type, QPoint pos, Qt::MouseButton btn, Qt::MouseButtons buttons,
                  Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent ev(type, pos, w->mapToGlobal(pos), btn, buttons, mods);
    QCoreApplication::sendEvent(w, &ev);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    Camera cam;
    rpe::PropertyEditor ed;
    ed.resize(520, 420);
    ed.editObject(cam); // WriteBack
    ed.show();
    QCoreApplication::processEvents();
    auto* view = ed.view();
    auto* proxy = view->model();
    auto* dlg = view->itemDelegateForColumn(1);
    auto cell = [&](const char* path) { return findPath(proxy, QString::fromLatin1(path)).siblingAtColumn(1); };

    // ── Inline vector row ───────────────────────────────────────────────────────
    printf("\n-- inline vector --\n");
    check("a Vec3 row is editable on its own row", bool(cell("position").flags() & Qt::ItemIsEditable));
    check("so is a 2-int struct (Size2)", bool(cell("resolution").flags() & Qt::ItemIsEditable));
    check("a struct with a non-numeric field is not", !(cell("lens").flags() & Qt::ItemIsEditable));
    check("a struct with a READ-ONLY field is not", !(cell("sealed").flags() & Qt::ItemIsEditable));
    {
        const QModelIndex idx = cell("position");
        QWidget* w = dlg->createEditor(view->viewport(), QStyleOptionViewItem(), idx);
        auto* ve = qobject_cast<rpe::VectorEditor*>(w);
        check("it opens a VectorEditor", ve != nullptr);
        if (ve)
        {
            dlg->setEditorData(w, idx);
            check("one box per field", ve->count() == 3);
            check("filled with the current values", ve->value(0) == 1 && ve->value(1) == 2 && ve->value(2) == 3);
            const QList<QLabel*> labels = ve->findChildren<QLabel*>();
            check("axis labels X / Y / Z", labels.size() == 3 && labels[0]->text() == QStringLiteral("X")
                                               && labels[1]->text() == QStringLiteral("Y")
                                               && labels[2]->text() == QStringLiteral("Z"));
            check("x is red, y green, z blue",
                  rpe::VectorEditor::axisColor(QStringLiteral("x")).red() > 150
                      && rpe::VectorEditor::axisColor(QStringLiteral("y")).green() > 150
                      && rpe::VectorEditor::axisColor(QStringLiteral("z")).blue() > 150);

            // Live value changes while open must not overwrite the user's typing.
            cam.position.y = 99;
            ed.refresh(rttr::instance(cam));
            dlg->setEditorData(w, idx);
            check("a live update doesn't overwrite an open editor", ve->value(1) == 2);

            ve->setValue(0, 5.5); // the user edits x only
            dlg->setModelData(w, proxy, idx);
            check("committing writes the changed field", cam.position.x == 5.5);
            check("...and leaves untouched fields alone (y keeps its live 99)", cam.position.y == 99);
        }
        delete w;
    }
    {
        const QModelIndex idx = cell("resolution");
        QWidget* w = dlg->createEditor(view->viewport(), QStyleOptionViewItem(), idx);
        auto* ve = qobject_cast<rpe::VectorEditor*>(w);
        if (ve)
        {
            dlg->setEditorData(w, idx);
            check("non-axis fields are labelled with their names",
                  ve->findChildren<QLabel*>().first()->text() == QStringLiteral("width"));
            check("integer fields get integer boxes", qobject_cast<QSpinBox*>(ve->spinBox(0)) != nullptr);
            ve->setValue(1, 1080);
            dlg->setModelData(w, proxy, idx);
            check("an int field is written as an int", cam.resolution.height == 1080 && cam.resolution.width == 640);
        }
        delete w;
    }

    // ── Drag-to-scrub ───────────────────────────────────────────────────────────
    printf("\n-- drag to scrub --\n");
    {
        QWidget* vp = view->viewport();
        auto namePos = [&](const char* path) {
            return view->visualRect(findPath(proxy, QString::fromLatin1(path))).center();
        };
        const QPoint fov = namePos("fov");

        mouse(vp, QEvent::MouseMove, fov, Qt::NoButton, Qt::NoButton);
        check("hovering a number's name shows a horizontal-resize cursor",
              vp->cursor().shape() == Qt::SizeHorCursor);
        mouse(vp, QEvent::MouseMove, namePos("enabled"), Qt::NoButton, Qt::NoButton);
        check("...but not a bool's", vp->cursor().shape() != Qt::SizeHorCursor);

        // Crossing the drag threshold STARTS the drag (no jump): measure from there.
        const QPoint t(12, 0); // past QApplication::startDragDistance() (10 px)

        // fov: start 60, Step 1, 4 px per step → +40 px = +10.
        mouse(vp, QEvent::MouseButtonPress, fov, Qt::LeftButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseMove, fov + t, Qt::NoButton, Qt::LeftButton);
        check("crossing the drag threshold doesn't jump the value", cam.fov == 60.0);
        mouse(vp, QEvent::MouseMove, fov + t + QPoint(40, 0), Qt::NoButton, Qt::LeftButton);
        check("dragging right increases the value (60 → 70)", cam.fov == 70.0);
        mouse(vp, QEvent::MouseMove, fov + t + QPoint(40, 0), Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
        check("Shift makes it fine (×0.1 → 61)", cam.fov == 61.0);
        mouse(vp, QEvent::MouseMove, fov + t + QPoint(4000, 0), Qt::NoButton, Qt::LeftButton);
        check("it stops at the Max hint (120)", cam.fov == 120.0);
        mouse(vp, QEvent::MouseButtonRelease, fov + QPoint(4000, 0), Qt::LeftButton, Qt::NoButton);

        // Esc mid-drag restores.
        mouse(vp, QEvent::MouseButtonPress, fov, Qt::LeftButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseMove, fov - t, Qt::NoButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseMove, fov - t - QPoint(80, 0), Qt::NoButton, Qt::LeftButton);
        check("dragging left decreases it", cam.fov == 100.0);
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(view, &esc);
        check("Esc mid-drag restores the starting value", cam.fov == 120.0);
        mouse(vp, QEvent::MouseButtonRelease, fov, Qt::LeftButton, Qt::NoButton);

        // A plain click (under the drag threshold) changes nothing.
        const QPoint samples = namePos("samples");
        mouse(vp, QEvent::MouseButtonPress, samples, Qt::LeftButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseMove, samples + QPoint(1, 0), Qt::NoButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseButtonRelease, samples + QPoint(1, 0), Qt::LeftButton, Qt::NoButton);
        check("a click without a drag changes nothing", cam.samples == 4);

        // Integers move in whole steps.
        mouse(vp, QEvent::MouseButtonPress, samples, Qt::LeftButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseMove, samples + t, Qt::NoButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseMove, samples + t + QPoint(8, 0), Qt::NoButton, Qt::LeftButton);
        check("an int scrubs in whole steps (4 + 8px/4 → 6)", cam.samples == 6);
        mouse(vp, QEvent::MouseMove, samples + t + QPoint(9, 0), Qt::NoButton, Qt::LeftButton);
        check("...and rounds, never fractional (9px → 2.25 steps → still 6)", cam.samples == 6);
        mouse(vp, QEvent::MouseButtonRelease, samples + QPoint(9, 0), Qt::LeftButton, Qt::NoButton);

        // Read-only editors don't scrub; nor does a disabled feature.
        ed.setReadOnly(true);
        mouse(vp, QEvent::MouseButtonPress, samples, Qt::LeftButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseMove, samples + t, Qt::NoButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseMove, samples + QPoint(40, 0), Qt::NoButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseButtonRelease, samples + QPoint(40, 0), Qt::LeftButton, Qt::NoButton);
        check("a read-only editor doesn't scrub", cam.samples == 6);
        ed.setReadOnly(false);
        ed.setDragToScrubEnabled(false);
        mouse(vp, QEvent::MouseButtonPress, samples, Qt::LeftButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseMove, samples + t, Qt::NoButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseMove, samples + QPoint(40, 0), Qt::NoButton, Qt::LeftButton);
        mouse(vp, QEvent::MouseButtonRelease, samples + QPoint(40, 0), Qt::LeftButton, Qt::NoButton);
        check("setDragToScrubEnabled(false) turns it off", cam.samples == 6);
        ed.setDragToScrubEnabled(true);
    }

    // ── Colour swatch for a string colour ───────────────────────────────────────
    printf("\n-- string colour swatch --\n");
    {
        const QVariant deco = cell("tint").data(Qt::DecorationRole);
        check("a Color-hinted string shows a swatch", deco.canConvert<QColor>() && deco.value<QColor>() == QColor("#ff336699"));
        check("a plain number shows none", !cell("fov").data(Qt::DecorationRole).isValid());
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

// The property grid's right-click menu can copy a row's name / value / "name =
// value" to the clipboard (works in read-only mode too — it never mutates).
#include <rpe/gui/PropertyEditor.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QTimer>
#include <QTreeView>

#include <cstdio>
#include <functional>

namespace
{
    struct Foo
    {
        double mass = 7.5;
        bool active = true;
    };
} // namespace

RTTR_REGISTRATION
{
    rttr::registration::class_<Foo>("Foo").property("mass", &Foo::mass).property("active", &Foo::active);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

static void triggerInMenu(const QString& label)
{
    QTimer::singleShot(0, [label] {
        auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!m)
            return;
        for (QAction* a : m->actions())
            if (a->text() == label)
            {
                a->trigger();
                break;
            }
        m->close();
    });
}

// Viewport position of the row whose name-column shows `name`.
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
    return idx.isValid() ? view->visualRect(idx).center() : QPoint(-1, -1);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    Foo foo;
    rpe::PropertyEditor editor;
    editor.editObject(foo);
    editor.setReadOnly(true); // copy must still work
    editor.resize(360, 300);
    editor.show();
    QApplication::processEvents();
    editor.expandAll();
    QApplication::processEvents();

    auto* view = editor.view();
    QClipboard* clip = QApplication::clipboard();

    const QPoint massPos = posOf(view, QStringLiteral("mass"));
    check("found the 'mass' row", massPos.x() >= 0);

    // Copy value → "7.5"
    clip->clear();
    triggerInMenu(QStringLiteral("Copy value"));
    QMetaObject::invokeMethod(&editor, "_onContextMenu", Qt::DirectConnection, Q_ARG(QPoint, massPos));
    check("Copy value put the rendered value on the clipboard", clip->text() == QStringLiteral("7.5"));

    // Copy name → "mass"
    clip->clear();
    triggerInMenu(QStringLiteral("Copy name"));
    QMetaObject::invokeMethod(&editor, "_onContextMenu", Qt::DirectConnection, Q_ARG(QPoint, massPos));
    check("Copy name put the property name on the clipboard", clip->text() == QStringLiteral("mass"));

    // Copy "name = value" → "mass = 7.5"
    clip->clear();
    triggerInMenu(QStringLiteral("Copy \"name = value\""));
    QMetaObject::invokeMethod(&editor, "_onContextMenu", Qt::DirectConnection, Q_ARG(QPoint, massPos));
    check("Copy name = value joins both", clip->text() == QStringLiteral("mass = 7.5"));

    // A different row's value (bool renders "true").
    const QPoint activePos = posOf(view, QStringLiteral("active"));
    clip->clear();
    triggerInMenu(QStringLiteral("Copy value"));
    QMetaObject::invokeMethod(&editor, "_onContextMenu", Qt::DirectConnection, Q_ARG(QPoint, activePos));
    check("Copy value works on another row (bool → 'true')", clip->text() == QStringLiteral("true"));

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

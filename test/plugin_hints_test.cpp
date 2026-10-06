// rpe hints registered in ANOTHER module (a plugin DLL) must be honoured.
//
// RTTR compares a const char* metadata key by address, and each module has its own
// copy of a string literal — so Min/Max/Label/ReadOnly/… registered in a plugin
// were never found by rpe. Keys are now value-compared (rpe::hint::Key).
#include <rpe/core/TypeBridge.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <QApplication>

#include <cstdio>

extern "C" void hintPluginRegister(); // from the rpe_test_hint_plugin shared library

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

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    hintPluginRegister();

    const rttr::type gauge = rpe::TypeBridge::resolveByName("plug.Gauge");
    check("the plugin's type is bridged", gauge.is_valid());

    rpe::PropertyEditor ed;
    ed.bindType(gauge);
    auto* model = ed.model();
    const QModelIndex value = findPath(model, QStringLiteral("value"));
    check("a Min hint from the plugin is seen", value.siblingAtColumn(1).data(rpe::MinRole).toDouble() == 0.0
                                                    && value.siblingAtColumn(1).data(rpe::MinRole).isValid());
    check("a Max hint from the plugin is seen", value.siblingAtColumn(1).data(rpe::MaxRole).toDouble() == 10.0);
    check("a Label hint from the plugin is seen", value.data(Qt::DisplayRole).toString() == QStringLiteral("Gauge value"));

    const QModelIndex locked = findPath(model, QStringLiteral("locked")).siblingAtColumn(1);
    check("a ReadOnly PROPERTY hint from the plugin is honoured", !(model->flags(locked) & Qt::ItemIsEditable));


    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

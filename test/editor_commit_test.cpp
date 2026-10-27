// Two editor fixes:
//
//  • rpe::editor::Slider was declared but never implemented — a property hinted
//    with it got a plain spin box. It now gets a slider next to its spin box
//    (when the property has both Min and Max).
//  • Committing an editor the user didn't change used to WRITE anyway. For a
//    value outside the editor's Min/Max, opening the editor clamps what it
//    shows — so pressing Enter stored the clamped value the user never chose.
//    An unchanged commit now writes nothing (and leaves no local-edit draft).
//  • No rpe::editor::* hint had ever taken effect: the constants are const char*
//    VARIABLES, which RTTR stores as const char* — and its to_string() can't read
//    that back, so the hint arrived empty. (QColor / std::filesystem::path got
//    the right editor by type anyway, which hid it.)
#include <rpe/core/EditorHints.h>
#include <rpe/gui/EditorWidgets.h>
#include <rpe/gui/PropertyEditor.h>
#include <rpe/gui/PropertyModel.h>

#include <rttr/registration.h>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QPlainTextEdit>
#include <QSlider>
#include <QSpinBox>
#include <QStyleOptionViewItem>
#include <QTreeView>

#include <cstdio>

struct Mixer
{
    double gain = 0.5;    // Slider, 0..1
    int volume = 40;      // Slider, 0..100
    double ratio = 2.0;   // Slider hint but NO range → plain spin box
    double limit = 150.0; // Min 0, Max 100 — currently OUT of range
};

struct Labels // string properties, each with an rpe::editor::* hint
{
    std::string tint = "#ff112233";
    std::string notes = "a\nb";
    std::string mesh;
    std::string output;
    std::string folder;
};

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<Labels>("Labels")
        .property("tint", &Labels::tint)(metadata(rpe::hint::Editor, rpe::editor::Color))
        .property("notes", &Labels::notes)(metadata(rpe::hint::Editor, rpe::editor::Multiline))
        .property("mesh", &Labels::mesh)(metadata(rpe::hint::Editor, rpe::editor::FilePath))
        .property("output", &Labels::output)(metadata(rpe::hint::Editor, rpe::editor::SaveFile))
        .property("folder", &Labels::folder)(metadata(rpe::hint::Editor, rpe::editor::Directory));
    registration::class_<Mixer>("Mixer")
        .property("gain", &Mixer::gain)(metadata(rpe::hint::Editor, rpe::editor::Slider),
                                        metadata(rpe::hint::Min, 0.0), metadata(rpe::hint::Max, 1.0),
                                        metadata(rpe::hint::Step, 0.01))
        .property("volume", &Mixer::volume)(metadata(rpe::hint::Editor, rpe::editor::Slider),
                                            metadata(rpe::hint::Min, 0), metadata(rpe::hint::Max, 100))
        .property("ratio", &Mixer::ratio)(metadata(rpe::hint::Editor, rpe::editor::Slider))
        .property("limit", &Mixer::limit)(metadata(rpe::hint::Min, 0.0), metadata(rpe::hint::Max, 100.0));
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

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    Mixer mix;
    rpe::PropertyEditor ed;
    ed.editObject(mix); // WriteBack
    auto* view = ed.view();
    auto* proxy = view->model();
    auto* dlg = view->itemDelegateForColumn(1);
    auto open = [&](const char* path) {
        const QModelIndex idx = findPath(proxy, QString::fromLatin1(path)).siblingAtColumn(1);
        QWidget* w = dlg->createEditor(view->viewport(), QStyleOptionViewItem(), idx);
        if (w)
            dlg->setEditorData(w, idx);
        return std::make_pair(w, idx);
    };

    // ── Slider ──────────────────────────────────────────────────────────────────
    printf("\n-- slider --\n");
    {
        auto [w, idx] = open("gain");
        auto* se = qobject_cast<rpe::SliderEditor*>(w);
        check("a double with Slider + Min/Max gets a SliderEditor", se != nullptr);
        if (se)
        {
            auto* spin = qobject_cast<QDoubleSpinBox*>(se->spinBox());
            check("...holding the value in its spin box", spin && spin->value() == 0.5);
            check("...with the slider at the matching position",
                  se->slider()->value() == (se->slider()->maximum() - se->slider()->minimum()) / 2);
            check("the slider never takes focus (typing stays in the spin box)",
                  se->slider()->focusPolicy() == Qt::NoFocus && se->focusProxy() == se->spinBox());

            se->slider()->setValue(se->slider()->maximum());
            check("dragging the slider to its end sets EXACTLY Max", spin && spin->value() == 1.0);
            spin->setValue(0.25);
            check("typing in the spin box moves the slider",
                  se->slider()->value() == se->slider()->maximum() / 4);

            dlg->setModelData(w, proxy, idx);
            check("committing writes the value", mix.gain == 0.25);
        }
        dlg->destroyEditor(w, idx);
    }
    {
        auto [w, idx] = open("volume");
        auto* se = qobject_cast<rpe::SliderEditor*>(w);
        check("an int with Slider + Min/Max gets a SliderEditor too", se != nullptr);
        if (se)
        {
            se->slider()->setValue(se->slider()->maximum() * 3 / 4);
            check("...and the integer follows the slider", qobject_cast<QSpinBox*>(se->spinBox())->value() == 75);
            dlg->setModelData(w, proxy, idx);
            check("...committing writes it", mix.volume == 75);
        }
        dlg->destroyEditor(w, idx);
    }
    {
        auto [w, idx] = open("ratio");
        check("Slider WITHOUT a range falls back to the plain spin box",
              qobject_cast<QDoubleSpinBox*>(w) != nullptr && !qobject_cast<rpe::SliderEditor*>(w));
        dlg->destroyEditor(w, idx);
    }

    // ── Unchanged commits write nothing ────────────────────────────────────────
    printf("\n-- unchanged commit --\n");
    {
        auto [w, idx] = open("limit");
        auto* spin = qobject_cast<QDoubleSpinBox*>(w);
        check("the editor shows the out-of-range value clamped to Max", spin && spin->value() == 100.0);
        dlg->setModelData(w, proxy, idx); // the user just presses Enter
        check("an unchanged commit does NOT write the clamped value", mix.limit == 150.0);
        dlg->destroyEditor(w, idx);
        check("...and leaves no local-edit draft behind",
              !findPath(ed.model(), QStringLiteral("limit")).data(rpe::HasLocalEditRole).toBool());
    }
    {
        auto [w, idx] = open("limit");
        qobject_cast<QDoubleSpinBox*>(w)->setValue(42.0);
        dlg->setModelData(w, proxy, idx);
        check("a CHANGED value is written as before", mix.limit == 42.0);
        dlg->destroyEditor(w, idx);
    }

    // LocalEdit policy: an unchanged commit used to freeze the row as a "draft".
    {
        Mixer other;
        rpe::PropertyEditor le;
        le.bindType(rttr::type::get<Mixer>());
        le.refresh(rttr::instance(other)); // LocalEdit (default): display only
        auto* lv = le.view();
        auto* lp = lv->model();
        auto* ld = lv->itemDelegateForColumn(1);
        const QModelIndex idx = findPath(lp, QStringLiteral("volume")).siblingAtColumn(1);
        QWidget* w = ld->createEditor(lv->viewport(), QStyleOptionViewItem(), idx);
        ld->setEditorData(w, idx);
        ld->setModelData(w, lp, idx);
        ld->destroyEditor(w, idx);
        check("LocalEdit: an unchanged commit doesn't freeze the row as a draft",
              !findPath(le.model(), QStringLiteral("volume")).data(rpe::HasLocalEditRole).toBool());
    }

    // ── Every rpe::editor::* hint actually reaches the editor ──────────────────
    printf("\n-- editor hints --\n");
    {
        Labels lab;
        rpe::PropertyEditor le;
        le.editObject(lab);
        auto* lv = le.view();
        auto* lp = lv->model();
        auto* ld = lv->itemDelegateForColumn(1);
        auto editorFor = [&](const char* path) {
            const QModelIndex idx = findPath(lp, QString::fromLatin1(path)).siblingAtColumn(1);
            return ld->createEditor(lv->viewport(), QStyleOptionViewItem(), idx);
        };
        auto mode = [](QWidget* w) {
            auto* fe = qobject_cast<rpe::FilePathEditor*>(w);
            return fe ? static_cast<int>(fe->mode()) : -1;
        };
        check("editor::Color on a string → colour picker", qobject_cast<rpe::ColorEditor*>(editorFor("tint")) != nullptr);
        check("editor::Multiline on a string → multi-line text", qobject_cast<QPlainTextEdit*>(editorFor("notes")) != nullptr);
        check("editor::FilePath on a string → open-file picker",
              mode(editorFor("mesh")) == static_cast<int>(rpe::FilePathEditor::Mode::OpenFile));
        check("editor::SaveFile on a string → save-file picker",
              mode(editorFor("output")) == static_cast<int>(rpe::FilePathEditor::Mode::SaveFile));
        check("editor::Directory on a string → folder picker",
              mode(editorFor("folder")) == static_cast<int>(rpe::FilePathEditor::Mode::Directory));
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

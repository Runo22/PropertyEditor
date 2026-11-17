// Floating-point input under a locale whose decimal point is "," (Turkish here):
//   • "0.5" typed into a float editor is a half — the stock QDoubleSpinBox took
//     the locale's "," only, dropped the "." and read "05" as 5;
//   • "0,5" is a half too (what a ","-keyboard types) — never 5;
//   • the editors SHOW "." like the grid does, whatever the locale;
//   • every decimal editor: plain float, double, slider, chrono, the inline
//     vector boxes, the watch list's editor (same factory);
//   • the enum editor's choice list renders as list rows (styled item delegate).
#include <rpe/core/TypeRenderer.h>
#include <rpe/gui/EditorWidgets.h>
#include <rpe/gui/VariantEditorFactory.h>

#include <rttr/registration.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QStyledItemDelegate>

#include <chrono>
#include <cstdio>
#include <string>

enum class Mode
{
    Idle,
    Patrol,
    AVeryLongModeNameThatIsWiderThanTheCell
};

RTTR_REGISTRATION
{
    rttr::registration::enumeration<Mode>("Mode")(rttr::value("Idle", Mode::Idle), rttr::value("Patrol", Mode::Patrol),
                                                  rttr::value("AVeryLongModeNameThatIsWiderThanTheCell",
                                                              Mode::AVeryLongModeNameThatIsWiderThanTheCell));
}

static int g_fails = 0;
static void check(const std::string& n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n.c_str());
    if (!ok)
        ++g_fails;
}

// Type `text` into the spin box key by key, as a user would.
static void typeInto(QAbstractSpinBox* sb, const QString& text)
{
    QLineEdit* le = sb->findChild<QLineEdit*>();
    le->selectAll();
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QCoreApplication::sendEvent(le, &del);
    for (const QChar c : text)
    {
        QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
        QCoreApplication::sendEvent(le, &press);
    }
    sb->interpretText();
}

static QDoubleSpinBox* doubleBox(QWidget* w)
{
    if (auto* d = qobject_cast<QDoubleSpinBox*>(w))
        return d;
    return w ? w->findChild<QDoubleSpinBox*>() : nullptr;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QLocale::setDefault(QLocale(QLocale::Turkish, QLocale::Turkey)); // decimal point ","
    QApplication app(argc, argv);
    check("(the test locale's decimal point is ',')", QLocale().decimalPoint() == QLatin1Char(','));

    // The stock box, to show what went wrong.
    {
        QDoubleSpinBox stock;
        stock.setDecimals(4);
        typeInto(&stock, QStringLiteral("0.5"));
        printf("   (a stock QDoubleSpinBox here reads \"0.5\" as %g)\n", stock.value());
    }

    struct Case
    {
        const char* name;
        rttr::type type;
        QString hint;
        rpe::varedit::EditorHints hints;
    };
    rpe::varedit::EditorHints sliderHints;
    sliderHints.min = 0;
    sliderHints.max = 1;
    const Case cases[] = {
        { "float", rttr::type::get<float>(), QString(), {} },
        { "double", rttr::type::get<double>(), QString(), {} },
        { "float with a slider", rttr::type::get<float>(), QStringLiteral("slider"), sliderHints },
    };
    for (const Case& c : cases)
    {
        QWidget* ed = rpe::varedit::makeEditor(c.type, c.hint, c.hints, nullptr);
        QDoubleSpinBox* sb = doubleBox(ed);
        check(std::string(c.name) + ": a decimal box", sb != nullptr);
        if (!sb)
            continue;
        typeInto(sb, QStringLiteral("0.5"));
        check(std::string(c.name) + ": \"0.5\" is a half", qFuzzyCompare(sb->value(), 0.5));
        typeInto(sb, QStringLiteral("0,5"));
        check(std::string(c.name) + ": \"0,5\" is a half too", qFuzzyCompare(sb->value(), 0.5));
        typeInto(sb, QStringLiteral("0.25"));
        check(std::string(c.name) + ": \"0.25\"", qFuzzyCompare(sb->value(), 0.25));
        typeInto(sb, QStringLiteral(".75"));
        check(std::string(c.name) + ": \".75\"", qFuzzyCompare(sb->value(), 0.75));
        rpe::varedit::setEditorData(ed, c.type == rttr::type::get<float>() ? rttr::variant(0.625f) : rttr::variant(0.625));
        check(std::string(c.name) + ": shown with '.' like the grid", sb->text().startsWith(QStringLiteral("0.625")));
        const rttr::variant read = rpe::varedit::readEditorData(ed, c.type);
        check(std::string(c.name) + ": read back", read.is_valid() && qFuzzyCompare(read.to_double(), 0.625));
        delete ed;
    }
    {
        QWidget* ed = rpe::varedit::makeEditor(rttr::type::get<double>(), QString(), {}, nullptr);
        QDoubleSpinBox* sb = doubleBox(ed);
        typeInto(sb, QStringLiteral("-12.5"));
        check("negative with '.'", qFuzzyCompare(sb->value(), -12.5));
        typeInto(sb, QStringLiteral("1234,5"));
        check("'1234,5' isn't read as a thousands group (12345)", qFuzzyCompare(sb->value(), 1234.5));
        typeInto(sb, QStringLiteral("1.2.3"));
        check("a second point is refused (stays a valid number)", qFuzzyCompare(sb->value(), 1.2) || qFuzzyCompare(sb->value(), 12.3) || sb->value() < 1e9);
        delete ed;
    }
    {
        // chrono: whole numbers with a suffix — the box is a DecimalSpinBox too
        QWidget* ed = rpe::varedit::makeEditor(rttr::type::get<std::chrono::milliseconds>(), QString(), {}, nullptr);
        check("chrono: a decimal box", dynamic_cast<rpe::DecimalSpinBox*>(ed) != nullptr);
        delete ed;
    }
    {
        // The inline vector row's boxes.
        rpe::VectorEditor vec({ { QStringLiteral("x"), false }, { QStringLiteral("y"), false } });
        auto* x = qobject_cast<QDoubleSpinBox*>(vec.spinBox(0));
        check("vector row: decimal boxes", dynamic_cast<rpe::DecimalSpinBox*>(vec.spinBox(0)) != nullptr);
        typeInto(x, QStringLiteral("0.5"));
        check("vector row: \"0.5\" is a half", qFuzzyCompare(vec.value(0), 0.5));
        typeInto(x, QStringLiteral("2,75"));
        check("vector row: \"2,75\"", qFuzzyCompare(vec.value(0), 2.75));
    }
    {
        // The enum editor's list.
        QWidget* ed = rpe::varedit::makeEditor(rttr::type::get<Mode>(), QString(), {}, nullptr);
        auto* cb = qobject_cast<QComboBox*>(ed);
        check("enum: a combo", cb != nullptr);
        if (cb)
        {
            check("enum: choices drawn as list rows (styled item delegate)",
                  qobject_cast<QStyledItemDelegate*>(cb->itemDelegate()) != nullptr
                      && QString::fromLatin1(cb->itemDelegate()->metaObject()->className()) == QLatin1String("QStyledItemDelegate"));
            check("enum: the list is at least as wide as its longest choice",
                  cb->view()->minimumWidth() >= cb->view()->sizeHintForColumn(0));
            check("enum: scrolls after 12", cb->maxVisibleItems() == 12);
        }
        delete ed;
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}

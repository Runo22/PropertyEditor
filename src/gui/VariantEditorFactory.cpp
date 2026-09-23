#include "rpe/gui/VariantEditorFactory.h"

#include "rpe/core/EditorHints.h"
#include "rpe/core/FlagsSupport.h"
#include "rpe/core/OptionalSupport.h"
#include "rpe/core/TypeRenderer.h"
#include "rpe/gui/EditorWidgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>

#include <limits>
#include <string>

#include <rttr/enumeration.h>

namespace rpe::varedit
{

    namespace
    {

        bool isFloatType(rttr::type t)
        {
            return t == rttr::type::get<float>() || t == rttr::type::get<double>();
        }

        bool isUnsignedIntType(rttr::type t)
        {
            return t == rttr::type::get<unsigned char>()
                || t == rttr::type::get<unsigned short>()
                || t == rttr::type::get<unsigned int>()
                || t == rttr::type::get<unsigned long>()
                || t == rttr::type::get<unsigned long long>();
        }

        // Types whose value range exceeds QSpinBox's int range.
        bool isWideIntType(rttr::type t)
        {
            return t == rttr::type::get<long>()
                || t == rttr::type::get<unsigned long>()
                || t == rttr::type::get<long long>()
                || t == rttr::type::get<unsigned long long>()
                || t == rttr::type::get<unsigned int>();
        }

        bool isStringFamily(rttr::type t)
        {
            return t == rttr::type::get<QString>()
                || t == rttr::type::get<std::string>()
                || t == rttr::type::get<std::wstring>()
                || t == rttr::type::get<std::u16string>()
                || t == rttr::type::get<std::u32string>();
        }

        // Editor text → a variant of the exact declared string type (invalid if
        // the target isn't one of the string family).
        rttr::variant stringVariantFor(rttr::type t, const QString& s)
        {
            if (t == rttr::type::get<QString>())
            {
                return rttr::variant(s);
            }
            if (t == rttr::type::get<std::string>())
            {
                return rttr::variant(s.toStdString());
            }
            if (t == rttr::type::get<std::wstring>())
            {
                return rttr::variant(s.toStdWString());
            }
            if (t == rttr::type::get<std::u16string>())
            {
                return rttr::variant(s.toStdU16String());
            }
            if (t == rttr::type::get<std::u32string>())
            {
                return rttr::variant(s.toStdU32String());
            }
            return {};
        }

    } // namespace

    QWidget* makeEditor(rttr::type t, const QString& ed, const EditorHints& hints, QWidget* parent)
    {
        // bool
        if (t == rttr::type::get<bool>())
        {
            return new QCheckBox(parent);
        }

        // QColor (auto) or color hint
        if (t == rttr::type::get<QColor>() || ed == QLatin1String(editor::Color))
        {
            return new ColorEditor(parent);
        }

        // std::filesystem::path (auto) → line edit + browse button. A Directory /
        // SaveFile / FilePath hint pins the dialog kind; with NO hint the browse
        // button offers BOTH a file and a folder picker (the type alone can't say
        // which is meant), so a folder can be chosen without registering the property.
        if (TypeRenderer::isFilePath(t))
        {
            const FilePathEditor::Mode m = (ed == QLatin1String(editor::Directory)) ? FilePathEditor::Mode::Directory
                : (ed == QLatin1String(editor::SaveFile))                            ? FilePathEditor::Mode::SaveFile
                : (ed == QLatin1String(editor::FilePath))                            ? FilePathEditor::Mode::OpenFile
                                                                                     : FilePathEditor::Mode::FileOrDirectory;
            return new FilePathEditor(m, parent);
        }

        // QDateTime → calendar-popup date/time editor.
        if (t == rttr::type::get<QDateTime>())
        {
            auto* dte = new QDateTimeEdit(parent);
            dte->setCalendarPopup(true);
            dte->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            return dte;
        }

        // chrono duration → integer spin with the unit as suffix ("250 ms").
        if (TypeRenderer::isChronoDuration(t))
        {
            auto* sb = new QDoubleSpinBox(parent);
            sb->setDecimals(0);
            sb->setRange(-9e15, 9e15);
            sb->setSingleStep(hints.step.value_or(1));
            sb->setSuffix(QLatin1Char(' ') + TypeRenderer::chronoSuffix(t));
            return sb;
        }

        // strings
        if (isStringFamily(t))
        {
            if (ed == QLatin1String(editor::FilePath))
            {
                return new FilePathEditor(FilePathEditor::Mode::OpenFile, parent);
            }
            if (ed == QLatin1String(editor::SaveFile))
            {
                return new FilePathEditor(FilePathEditor::Mode::SaveFile, parent);
            }
            if (ed == QLatin1String(editor::Directory))
            {
                return new FilePathEditor(FilePathEditor::Mode::Directory, parent);
            }
            if (ed == QLatin1String(editor::Multiline))
            {
                auto* te = new QPlainTextEdit(parent);
                te->setTabChangesFocus(true);
                return te;
            }
            return new QLineEdit(parent);
        }

        // enum
        if (t.is_enumeration())
        {
            // Bitmask enum (hint::Flags): multi-check editor instead of a combo.
            if (hints.flags)
            {
                auto* flagsEd = new FlagsEditor(parent);
                const rttr::enumeration en = t.get_enumeration();
                QList<QPair<QString, qint64>> flags;
                for (const auto& n : en.get_names())
                {
                    const rttr::variant nv = en.name_to_value(n);
                    flags.append({ QString::fromUtf8(n.data(), static_cast<int>(n.size())),
                                   TypeRenderer::enumBits(nv) });
                }
                flagsEd->setFlags(flags);
                return flagsEd;
            }
            auto* cb = new QComboBox(parent);
            for (const auto& n : t.get_enumeration().get_names())
            {
                cb->addItem(QString::fromUtf8(n.data(), static_cast<int>(n.size())));
            }
            return cb;
        }

        // floating point
        if (isFloatType(t))
        {
            auto* sb = new QDoubleSpinBox(parent);
            sb->setDecimals(hints.decimals.value_or(4));
            sb->setRange(hints.min.value_or(-1e15), hints.max.value_or(1e15));
            sb->setSingleStep(hints.step.value_or(0.1));
            return sb;
        }

        // integral
        if (t.is_arithmetic())
        {
            const bool isUnsigned = isUnsignedIntType(t);
            // Wide integers don't fit QSpinBox's int range — use a validated line
            // edit so 64-bit / large unsigned values are never silently clamped.
            if (isWideIntType(t))
            {
                auto* le = new QLineEdit(parent);
                // Allow empty / lone "-" as intermediate states so the field can be
                // cleared and negatives typed; readEditorData's parse check gates commit.
                static const QRegularExpression reUnsigned(QStringLiteral("\\d{0,20}"));
                static const QRegularExpression reSigned(QStringLiteral("-?\\d{0,19}"));
                le->setValidator(new QRegularExpressionValidator(
                    isUnsigned ? reUnsigned : reSigned, le));
                return le;
            }
            auto* sb = new QSpinBox(parent);
            const int lo = isUnsigned ? 0 : std::numeric_limits<int>::min();
            sb->setRange(hints.min ? static_cast<int>(*hints.min) : lo,
                         hints.max ? static_cast<int>(*hints.max) : std::numeric_limits<int>::max());
            sb->setSingleStep(hints.step ? static_cast<int>(*hints.step) : 1);
            return sb;
        }

        return nullptr; // expandable / unsupported types are not inline-editable
    }

    void setEditorData(QWidget* editor, const rttr::variant& vIn)
    {
        if (!editor)
        {
            return;
        }
        rttr::variant v = TypeRenderer::unwrap(vIn);
        // Optional: edit its inner value (empty optionals populate with a default).
        if (OptionalBridge::isOptional(v.get_type()))
        {
            v = v.extract_wrapped_value();
        }
        const rttr::type t = v.get_type();
        // Text shown in string-like editors — empty (not "<invalid>") when the value
        // hasn't arrived yet, e.g. a path cell edited before the first mirror pump.
        const QString text = v.is_valid() ? TypeRenderer::toDisplayString(v) : QString();

        if (auto* cb = qobject_cast<QCheckBox*>(editor))
        {
            cb->setChecked(v.to_bool());
            return;
        }
        if (auto* ce = qobject_cast<ColorEditor*>(editor))
        {
            if (t == rttr::type::get<QColor>())
            {
                ce->setColor(v.get_value<QColor>());
            }
            else
            {
                // Color hint on a string property: parse "#AARRGGBB" / named colors.
                ce->setColor(QColor(TypeRenderer::toDisplayString(v)));
            }
            return;
        }
        if (auto* te = qobject_cast<QPlainTextEdit*>(editor))
        {
            te->setPlainText(text);
            return;
        }
        if (auto* le = qobject_cast<QLineEdit*>(editor))
        {
            le->setText(text);
            return;
        }
        if (auto* fe = qobject_cast<FilePathEditor*>(editor))
        {
            fe->setPath(text);
            return;
        }
        // FlagsEditor derives from QComboBox — check it first.
        if (auto* flagsEd = qobject_cast<FlagsEditor*>(editor))
        {
            flagsEd->setBits(TypeRenderer::enumBits(v));
            return;
        }
        if (auto* cb = qobject_cast<QComboBox*>(editor))
        {
            if (t.is_enumeration())
            {
                const rttr::string_view n = t.get_enumeration().value_to_name(v);
                if (!n.empty())
                {
                    cb->setCurrentText(QString::fromUtf8(n.data(), static_cast<int>(n.size())));
                }
            }
            return;
        }
        if (auto* dte = qobject_cast<QDateTimeEdit*>(editor))
        {
            if (v.is_valid() && t == rttr::type::get<QDateTime>())
            {
                dte->setDateTime(v.get_value<QDateTime>());
            }
            return;
        }
        if (auto* sb = qobject_cast<QDoubleSpinBox*>(editor))
        {
            if (TypeRenderer::isChronoDuration(t))
            {
                sb->setValue(static_cast<double>(TypeRenderer::chronoCount(v)));
            }
            else
            {
                sb->setValue(v.to_double());
            }
            return;
        }
        if (auto* sb = qobject_cast<QSpinBox*>(editor))
        {
            sb->setValue(v.to_int());
            return;
        }
    }

    rttr::variant readEditorData(QWidget* editor, rttr::type t)
    {
        if (!editor)
        {
            return {};
        }
        rttr::variant newVal;

        if (auto* cb = qobject_cast<QCheckBox*>(editor))
        {
            newVal = cb->isChecked();
        }
        else if (auto* ce = qobject_cast<ColorEditor*>(editor))
        {
            if (t == rttr::type::get<QColor>())
            {
                newVal = ce->color();
            }
            else if (const rttr::variant sv = stringVariantFor(t, ce->color().name(QColor::HexArgb)); sv.is_valid())
            {
                newVal = sv;
            }
        }
        else if (auto* te = qobject_cast<QPlainTextEdit*>(editor))
        {
            newVal = stringVariantFor(t, te->toPlainText());
            if (!newVal.is_valid())
            {
                newVal = te->toPlainText().toStdString();
            }
        }
        else if (auto* fe = qobject_cast<FilePathEditor*>(editor))
        {
            // Emit a QString for path (the bridge builds std::filesystem::path from
            // it); string-family targets get their exact type, others a std::string.
            if (TypeRenderer::isFilePath(t))
            {
                newVal = fe->path();
            }
            else
            {
                newVal = stringVariantFor(t, fe->path());
                if (!newVal.is_valid())
                {
                    newVal = fe->path().toStdString();
                }
            }
        }
        else if (auto* le = qobject_cast<QLineEdit*>(editor))
        {
            if (const rttr::variant sv = stringVariantFor(t, le->text()); sv.is_valid())
            {
                newVal = sv;
            }
            else if (t.is_arithmetic())
            {
                // Wide-integer editor: parse exactly, never clamp through int.
                bool ok = false;
                if (isUnsignedIntType(t))
                {
                    const qulonglong v = le->text().toULongLong(&ok);
                    if (ok)
                    {
                        newVal = static_cast<uint64_t>(v);
                    }
                }
                else
                {
                    const qlonglong v = le->text().toLongLong(&ok);
                    if (ok)
                    {
                        newVal = static_cast<int64_t>(v);
                    }
                }
            }
        }
        // FlagsEditor derives from QComboBox — check it first.
        else if (auto* flagsEd = qobject_cast<FlagsEditor*>(editor))
        {
            if (t.is_enumeration())
            {
                const qint64 bits = flagsEd->bits();
                // A combined mask has no name; only FlagsBridge (RPE_REGISTER_FLAGS)
                // can turn arbitrary bits back into the exact enum type.
                newVal = FlagsBridge::build(t, bits);
                if (!newVal.is_valid())
                {
                    // No builder registered: still commit if the result happens to be
                    // a single named value (name_to_value yields the exact enum type).
                    const rttr::enumeration en = t.get_enumeration();
                    for (const auto& n : en.get_names())
                    {
                        const rttr::variant nv = en.name_to_value(n);
                        if (TypeRenderer::enumBits(nv) == bits)
                        {
                            newVal = nv;
                            break;
                        }
                    }
                }
            }
        }
        else if (auto* combo = qobject_cast<QComboBox*>(editor))
        {
            if (t.is_enumeration())
            {
                newVal = t.get_enumeration().name_to_value(combo->currentText().toStdString());
            }
        }
        else if (auto* dte = qobject_cast<QDateTimeEdit*>(editor))
        {
            newVal = dte->dateTime();
        }
        else if (auto* dsb = qobject_cast<QDoubleSpinBox*>(editor))
        {
            if (TypeRenderer::isChronoDuration(t))
            {
                // Commit the exact duration type so LocalEdit rows display with
                // their unit too (coerce() would fix WriteBack either way).
                newVal = TypeRenderer::makeChronoDuration(t, static_cast<qint64>(dsb->value()));
            }
            else if (t == rttr::type::get<float>())
            {
                newVal = static_cast<float>(dsb->value());
            }
            else
            {
                newVal = dsb->value();
            }
        }
        else if (auto* isb = qobject_cast<QSpinBox*>(editor))
        {
            newVal = isb->value();
        }

        return newVal;
    }

} // namespace rpe::varedit

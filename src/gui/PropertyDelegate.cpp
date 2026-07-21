#include "rpe/gui/PropertyDelegate.h"

#include "rpe/core/EditorHints.h"
#include "rpe/core/FlagsSupport.h"
#include "rpe/core/OptionalSupport.h"
#include "rpe/core/TypeRenderer.h"
#include "rpe/gui/EditorWidgets.h"
#include "rpe/gui/PropertyModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QStyle>

#include <limits>
#include <string>

#include <rttr/enumeration.h>

namespace rpe
{

    namespace
    {

        // Read an optional numeric hint role; fall back to `def` when unset.
        double roleDouble(const QModelIndex& i, int role, double def)
        {
            const QVariant v = i.data(role);
            return v.isValid() ? v.toDouble() : def;
        }
        int roleInt(const QModelIndex& i, int role, int def)
        {
            const QVariant v = i.data(role);
            return v.isValid() ? v.toInt() : def;
        }

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

    PropertyDelegate::PropertyDelegate(PropertyModel* model, QObject* parent)
        : QStyledItemDelegate(parent)
        , _model(model)
    {
    }

    void PropertyDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        // While an inline editor is open over this value cell, paint ONLY the item
        // background — not the value text/decoration. Otherwise the previous value
        // (e.g. "true") bleeds through editors that don't fully fill the cell, most
        // visibly a QCheckBox (which paints just its indicator) or a QColor swatch.
        // The editor widget itself sits on top of this background.
        if (!_editPath.isEmpty() && index.data(PropertyPathRole).toString() == _editPath)
        {
            QStyleOptionViewItem opt(option);
            initStyleOption(&opt, index);
            opt.text.clear();
            opt.icon = QIcon();
            opt.features &= ~(QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration);
            const QWidget* w = opt.widget;
            QStyle* style = w ? w->style() : QApplication::style();
            style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, w);
            return;
        }
        QStyledItemDelegate::paint(painter, option, index);
    }

    // Builds the editor widget for a leaf of declared type `t` (with editor hint
    // `ed`). Returns nullptr for types that aren't inline-editable. Kept separate so
    // createEditor only pins the row once it knows an editor will actually open.
    QWidget* PropertyDelegate::_makeEditor(rttr::type t, const QString& ed, const QModelIndex& index, QWidget* parent) const
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
            sb->setSingleStep(roleDouble(index, StepRole, 1));
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
            if (index.data(FlagsRole).toBool())
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
            sb->setDecimals(roleInt(index, DecimalsRole, 4));
            sb->setRange(roleDouble(index, MinRole, -1e15), roleDouble(index, MaxRole, 1e15));
            sb->setSingleStep(roleDouble(index, StepRole, 0.1));
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
                // cleared and negatives typed; setModelData's parse check gates commit.
                static const QRegularExpression reUnsigned(QStringLiteral("\\d{0,20}"));
                static const QRegularExpression reSigned(QStringLiteral("-?\\d{0,19}"));
                le->setValidator(new QRegularExpressionValidator(
                    isUnsigned ? reUnsigned : reSigned, le));
                return le;
            }
            auto* sb = new QSpinBox(parent);
            const int lo = isUnsigned ? 0 : std::numeric_limits<int>::min();
            sb->setRange(roleInt(index, MinRole, lo), roleInt(index, MaxRole, std::numeric_limits<int>::max()));
            sb->setSingleStep(roleInt(index, StepRole, 1));
            return sb;
        }

        return nullptr; // expandable / unsupported types are not inline-editable
    }

    QWidget* PropertyDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const
    {
        if (!index.isValid())
        {
            return nullptr;
        }

        // Pick the editor from the node's DECLARED (schema) type, not the live value:
        // in mirror mode the value may not have arrived yet, and right after an edit
        // the live value is the editor's transient output — both would otherwise
        // select the wrong editor (e.g. a plain line edit for a path with no browse).
        const rttr::variant declared = index.data(DeclaredTypeRole).value<rttr::variant>();
        const rttr::type t = declared.is_valid()
            ? TypeRenderer::rawType(declared.get_value<rttr::type>())
            : TypeRenderer::rawType(index.data(RttrVariantRole).value<rttr::variant>().get_type());
        const QString ed = index.data(EditorHintRole).toString();

        QWidget* w = _makeEditor(t, ed, index, parent);
        if (!w)
        {
            return nullptr;
        }

        // Pin the row only now that an editor will actually open, so live refresh
        // can't clobber it — and a cell that yields no editor is never left stuck
        // stuck holding a local edit (which would freeze its live updates). Remember the prior pin
        // state so a cancelled edit can restore it.
        _editPath = index.data(PropertyPathRole).toString();
        _editHadLocalEdit = index.data(HasLocalEditRole).toBool();
        _editCommitted = false;
        _model->beginLocalEdit(_editPath);
        return w;
    }

    void PropertyDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
    {
        if (!editor || !index.isValid())
        {
            return;
        }
        const rttr::variant vIn = index.data(RttrVariantRole).value<rttr::variant>();
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

    void PropertyDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const
    {
        if (!editor || !index.isValid())
        {
            return;
        }
        // Use the declared (schema) type, not the live value — the value may be
        // absent, or (right after an edit) hold the editor's transient output, either
        // of which would misroute the conversion below.
        const rttr::variant declared = index.data(DeclaredTypeRole).value<rttr::variant>();
        const rttr::type t = declared.is_valid()
            ? TypeRenderer::rawType(declared.get_value<rttr::type>())
            : TypeRenderer::rawType(index.data(RttrVariantRole).value<rttr::variant>().get_type());

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

        if (newVal.is_valid())
        {
            _editCommitted = true;
            model->setData(index, QVariant::fromValue(newVal), Qt::EditRole);
        }
    }

    void PropertyDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const
    {
        editor->setGeometry(option.rect);
    }

    void PropertyDelegate::destroyEditor(QWidget* editor, const QModelIndex& index) const
    {
        // If the edit was cancelled (no commit) and the row was not pinned before we
        // opened the editor, release the implicit pin so live updates resume.
        if (!_editCommitted && !_editHadLocalEdit && !_editPath.isEmpty())
        {
            _model->resetNode(_editPath);
        }
        _editPath.clear();
        QStyledItemDelegate::destroyEditor(editor, index);
    }

} // namespace rpe

#include "rpe/gui/PropertyDelegate.h"

#include "rpe/core/EditorHints.h"
#include "rpe/core/TypeRenderer.h"
#include "rpe/gui/EditorWidgets.h"
#include "rpe/gui/PropertyModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>

#include <limits>

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

    } // namespace

    PropertyDelegate::PropertyDelegate(PropertyModel* model, QObject* parent)
        : QStyledItemDelegate(parent)
        , _model(model)
    {
    }

    QWidget* PropertyDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const
    {
        if (!index.isValid())
        {
            return nullptr;
        }

        // Pin the row while the editor is open so live refresh can't clobber it.
        // Remember the prior pin state so a cancelled edit can restore it.
        _editPath = index.data(PropertyPathRole).toString();
        _editWasOverridden = index.data(IsOverriddenRole).toBool();
        _editCommitted = false;
        _model->overrideNode(_editPath);

        const rttr::variant cur = index.data(RttrVariantRole).value<rttr::variant>();
        const rttr::type t = TypeRenderer::rawType(cur.get_type());
        const QString ed = index.data(EditorHintRole).toString();

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

        // strings
        if (t == rttr::type::get<std::string>() || t == rttr::type::get<QString>())
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

    void PropertyDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
    {
        if (!editor || !index.isValid())
        {
            return;
        }
        const rttr::variant vIn = index.data(RttrVariantRole).value<rttr::variant>();
        const rttr::variant v = TypeRenderer::unwrap(vIn);
        const rttr::type t = v.get_type();

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
            te->setPlainText(TypeRenderer::toDisplayString(v));
            return;
        }
        if (auto* le = qobject_cast<QLineEdit*>(editor))
        {
            le->setText(TypeRenderer::toDisplayString(v));
            return;
        }
        if (auto* fe = qobject_cast<FilePathEditor*>(editor))
        {
            fe->setPath(TypeRenderer::toDisplayString(v));
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
        if (auto* sb = qobject_cast<QDoubleSpinBox*>(editor))
        {
            sb->setValue(v.to_double());
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
        const rttr::variant oldVal = index.data(RttrVariantRole).value<rttr::variant>();
        const rttr::type t = TypeRenderer::rawType(oldVal.get_type());

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
            else if (t == rttr::type::get<QString>())
            {
                newVal = ce->color().name(QColor::HexArgb);
            }
            else if (t == rttr::type::get<std::string>())
            {
                newVal = ce->color().name(QColor::HexArgb).toStdString();
            }
        }
        else if (auto* te = qobject_cast<QPlainTextEdit*>(editor))
        {
            if (t == rttr::type::get<QString>())
            {
                newVal = te->toPlainText();
            }
            else
            {
                newVal = te->toPlainText().toStdString();
            }
        }
        else if (auto* fe = qobject_cast<FilePathEditor*>(editor))
        {
            if (t == rttr::type::get<QString>())
            {
                newVal = fe->path();
            }
            else
            {
                newVal = fe->path().toStdString();
            }
        }
        else if (auto* le = qobject_cast<QLineEdit*>(editor))
        {
            if (t == rttr::type::get<QString>())
            {
                newVal = le->text();
            }
            else if (t == rttr::type::get<std::string>())
            {
                newVal = le->text().toStdString();
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
        else if (auto* cb = qobject_cast<QComboBox*>(editor))
        {
            if (t.is_enumeration())
            {
                newVal = t.get_enumeration().name_to_value(cb->currentText().toStdString());
            }
        }
        else if (auto* sb = qobject_cast<QDoubleSpinBox*>(editor))
        {
            if (t == rttr::type::get<float>())
            {
                newVal = static_cast<float>(sb->value());
            }
            else
            {
                newVal = sb->value();
            }
        }
        else if (auto* sb = qobject_cast<QSpinBox*>(editor))
        {
            newVal = sb->value();
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
        if (!_editCommitted && !_editWasOverridden && !_editPath.isEmpty())
        {
            _model->resetNode(_editPath);
        }
        _editPath.clear();
        QStyledItemDelegate::destroyEditor(editor, index);
    }

} // namespace rpe

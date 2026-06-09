#include "rpe/gui/PropertyDelegate.h"

#include "rpe/core/EditorHints.h"
#include "rpe/core/TypeRenderer.h"
#include "rpe/gui/EditorWidgets.h"
#include "rpe/gui/PropertyModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSpinBox>

#include <limits>

#include <rttr/enumeration.h>

namespace rpe {

namespace {

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

} // namespace

PropertyDelegate::PropertyDelegate(PropertyModel* model, QObject* parent)
    : QStyledItemDelegate(parent), _model(model)
{}

QWidget* PropertyDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem&,
                                        const QModelIndex& index) const
{
    if (!index.isValid()) return nullptr;

    // Pin the row while the editor is open so live refresh can't clobber it.
    _model->overrideNode(index.data(PropertyPathRole).toString());

    const rttr::variant cur = index.data(RttrVariantRole).value<rttr::variant>();
    const rttr::type    t   = TypeRenderer::rawType(cur.get_type());
    const QString       ed  = index.data(EditorHintRole).toString();

    // bool
    if (t == rttr::type::get<bool>())
        return new QCheckBox(parent);

    // QColor (auto) or color hint
    if (t == rttr::type::get<QColor>() || ed == QLatin1String(editor::Color))
        return new ColorEditor(parent);

    // strings
    if (t == rttr::type::get<std::string>() || t == rttr::type::get<QString>()) {
        if (ed == QLatin1String(editor::FilePath))
            return new FilePathEditor(FilePathEditor::Mode::OpenFile, parent);
        if (ed == QLatin1String(editor::SaveFile))
            return new FilePathEditor(FilePathEditor::Mode::SaveFile, parent);
        if (ed == QLatin1String(editor::Directory))
            return new FilePathEditor(FilePathEditor::Mode::Directory, parent);
        if (ed == QLatin1String(editor::Multiline)) {
            auto* te = new QPlainTextEdit(parent);
            te->setTabChangesFocus(true);
            return te;
        }
        return new QLineEdit(parent);
    }

    // enum
    if (t.is_enumeration()) {
        auto* cb = new QComboBox(parent);
        for (const auto& n : t.get_enumeration().get_names())
            cb->addItem(QString::fromUtf8(n.data(), static_cast<int>(n.size())));
        return cb;
    }

    // floating point
    if (isFloatType(t)) {
        auto* sb = new QDoubleSpinBox(parent);
        sb->setDecimals(roleInt(index, DecimalsRole, 4));
        sb->setRange(roleDouble(index, MinRole, -1e15),
                     roleDouble(index, MaxRole,  1e15));
        sb->setSingleStep(roleDouble(index, StepRole, 0.1));
        return sb;
    }

    // integral
    if (t.is_arithmetic()) {
        const bool isUnsigned = (t == rttr::type::get<unsigned int>()
                              || t == rttr::type::get<unsigned long>()
                              || t == rttr::type::get<unsigned long long>()
                              || t == rttr::type::get<unsigned char>()
                              || t == rttr::type::get<unsigned short>());
        auto* sb = new QSpinBox(parent);
        const int lo = isUnsigned ? 0 : std::numeric_limits<int>::min();
        sb->setRange(roleInt(index, MinRole, lo),
                     roleInt(index, MaxRole, std::numeric_limits<int>::max()));
        sb->setSingleStep(roleInt(index, StepRole, 1));
        return sb;
    }

    return nullptr; // expandable / unsupported types are not inline-editable
}

void PropertyDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
    if (!editor || !index.isValid()) return;
    const rttr::variant vIn = index.data(RttrVariantRole).value<rttr::variant>();
    const rttr::variant v   = TypeRenderer::unwrap(vIn);
    const rttr::type    t   = v.get_type();

    if (auto* cb = qobject_cast<QCheckBox*>(editor)) {
        cb->setChecked(v.to_bool());
        return;
    }
    if (auto* ce = qobject_cast<ColorEditor*>(editor)) {
        if (t == rttr::type::get<QColor>()) ce->setColor(v.get_value<QColor>());
        return;
    }
    if (auto* te = qobject_cast<QPlainTextEdit*>(editor)) {
        te->setPlainText(TypeRenderer::toDisplayString(v));
        return;
    }
    if (auto* le = qobject_cast<QLineEdit*>(editor)) {
        le->setText(TypeRenderer::toDisplayString(v));
        return;
    }
    if (auto* fe = qobject_cast<FilePathEditor*>(editor)) {
        fe->setPath(TypeRenderer::toDisplayString(v));
        return;
    }
    if (auto* cb = qobject_cast<QComboBox*>(editor)) {
        if (t.is_enumeration()) {
            const rttr::string_view n = t.get_enumeration().value_to_name(v);
            if (!n.empty())
                cb->setCurrentText(QString::fromUtf8(n.data(), static_cast<int>(n.size())));
        }
        return;
    }
    if (auto* sb = qobject_cast<QDoubleSpinBox*>(editor)) {
        sb->setValue(v.to_double());
        return;
    }
    if (auto* sb = qobject_cast<QSpinBox*>(editor)) {
        sb->setValue(v.to_int());
        return;
    }
}

void PropertyDelegate::setModelData(QWidget* editor, QAbstractItemModel* model,
                                    const QModelIndex& index) const
{
    if (!editor || !index.isValid()) return;
    const rttr::variant oldVal = index.data(RttrVariantRole).value<rttr::variant>();
    const rttr::type    t      = TypeRenderer::rawType(oldVal.get_type());

    rttr::variant newVal;

    if (auto* cb = qobject_cast<QCheckBox*>(editor)) {
        newVal = cb->isChecked();
    } else if (auto* ce = qobject_cast<ColorEditor*>(editor)) {
        newVal = ce->color();
    } else if (auto* te = qobject_cast<QPlainTextEdit*>(editor)) {
        if (t == rttr::type::get<QString>()) newVal = te->toPlainText();
        else                                 newVal = te->toPlainText().toStdString();
    } else if (auto* fe = qobject_cast<FilePathEditor*>(editor)) {
        if (t == rttr::type::get<QString>()) newVal = fe->path();
        else                                 newVal = fe->path().toStdString();
    } else if (auto* le = qobject_cast<QLineEdit*>(editor)) {
        if (t == rttr::type::get<QString>()) newVal = le->text();
        else                                 newVal = le->text().toStdString();
    } else if (auto* cb = qobject_cast<QComboBox*>(editor)) {
        if (t.is_enumeration())
            newVal = t.get_enumeration().name_to_value(cb->currentText().toStdString());
    } else if (auto* sb = qobject_cast<QDoubleSpinBox*>(editor)) {
        if (t == rttr::type::get<float>()) newVal = static_cast<float>(sb->value());
        else                               newVal = sb->value();
    } else if (auto* sb = qobject_cast<QSpinBox*>(editor)) {
        newVal = sb->value();
    }

    if (newVal.is_valid())
        model->setData(index, QVariant::fromValue(newVal), Qt::EditRole);
}

void PropertyDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                                            const QModelIndex&) const
{
    editor->setGeometry(option.rect);
}

void PropertyDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    // Draw a small swatch for QColor leaves so they read at a glance.
    const rttr::variant v = index.data(RttrVariantRole).value<rttr::variant>();
    if (index.column() == 1 && v.is_valid()
        && TypeRenderer::rawType(v.get_type()) == rttr::type::get<QColor>()) {
        const QColor c = TypeRenderer::unwrap(v).get_value<QColor>();
        QStyledItemDelegate::paint(painter, opt, index); // background/selection
        QRect r = opt.rect.adjusted(4, 3, -4, -3);
        r.setWidth(qMin(r.width(), 36));
        painter->save();
        painter->setPen(QColor(0x55, 0x55, 0x55));
        painter->setBrush(c);
        painter->drawRect(r);
        painter->restore();
        return;
    }
    QStyledItemDelegate::paint(painter, opt, index);
}

} // namespace rpe

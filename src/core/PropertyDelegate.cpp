#include "rttr_property_editor/core/PropertyDelegate.h"
#include "rttr_property_editor/core/PropertyModel.h"
#include "rttr_property_editor/core/TypeRenderer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPainter>
#include <QSpinBox>
#include <QStyleOptionViewItem>

#include <rttr/enumeration>
#include <rttr/type>
#include <rttr/variant>

namespace rpe {

PropertyDelegate::PropertyDelegate(PropertyModel* model, QObject* parent)
    : QStyledItemDelegate(parent)
    , _model(model)
{}

QWidget* PropertyDelegate::createEditor(QWidget*                    parent,
                                        const QStyleOptionViewItem& /*option*/,
                                        const QModelIndex&          index) const
{
    if (!index.isValid()) return nullptr;

    // Notify model: implicit override while editor is open
    const QString path = index.data(PropertyPathRole).toString();
    _model->overrideNode(path);

    const rttr::variant curVal = index.data(RttrVariantRole).value<rttr::variant>();
    const rttr::type    t      = curVal.get_type();

    // double / float
    if (t == rttr::type::get<double>() || t == rttr::type::get<float>()) {
        auto* sb = new QDoubleSpinBox(parent);
        sb->setDecimals(8);
        sb->setRange(-1e18, 1e18);
        sb->setSingleStep(0.01);
        return sb;
    }

    // integers
    if (t == rttr::type::get<int>() || t == rttr::type::get<long long>()) {
        auto* sb = new QSpinBox(parent);
        sb->setRange(INT_MIN, INT_MAX);
        return sb;
    }
    if (t == rttr::type::get<unsigned int>() || t == rttr::type::get<unsigned long long>()) {
        auto* sb = new QSpinBox(parent);
        sb->setRange(0, INT_MAX);
        return sb;
    }

    // bool
    if (t == rttr::type::get<bool>()) {
        auto* cb = new QCheckBox(parent);
        return cb;
    }

    // std::string / QString
    if (t == rttr::type::get<std::string>() || t == rttr::type::get<QString>()) {
        return new QLineEdit(parent);
    }

    // enum
    if (t.is_enumeration()) {
        auto* cb = new QComboBox(parent);
        const auto en = t.get_enumeration();
        for (const auto& name : en.get_names())
            cb->addItem(QString::fromStdString(name.to_string()));
        return cb;
    }

    return nullptr; // expandable nodes are not directly editable
}

void PropertyDelegate::setEditorData(QWidget*           editor,
                                     const QModelIndex& index) const
{
    if (!index.isValid() || !editor) return;

    const rttr::variant v = index.data(RttrVariantRole).value<rttr::variant>();
    const rttr::type    t = v.get_type();

    if (auto* sb = qobject_cast<QDoubleSpinBox*>(editor)) {
        if (t == rttr::type::get<double>())
            sb->setValue(v.get_value<double>());
        else if (t == rttr::type::get<float>())
            sb->setValue(static_cast<double>(v.get_value<float>()));
        return;
    }
    if (auto* sb = qobject_cast<QSpinBox*>(editor)) {
        if (t == rttr::type::get<int>())
            sb->setValue(v.get_value<int>());
        else if (t == rttr::type::get<unsigned int>())
            sb->setValue(static_cast<int>(v.get_value<unsigned int>()));
        return;
    }
    if (auto* cb = qobject_cast<QCheckBox*>(editor)) {
        cb->setChecked(v.get_value<bool>());
        return;
    }
    if (auto* le = qobject_cast<QLineEdit*>(editor)) {
        if (t == rttr::type::get<std::string>())
            le->setText(QString::fromStdString(v.get_value<std::string>()));
        else if (t == rttr::type::get<QString>())
            le->setText(v.get_value<QString>());
        return;
    }
    if (auto* cb = qobject_cast<QComboBox*>(editor)) {
        if (t.is_enumeration()) {
            const auto en = t.get_enumeration();
            bool ok = false;
            const auto name = en.value_to_name(v, ok);
            if (ok)
                cb->setCurrentText(QString::fromStdString(name.to_string()));
        }
        return;
    }
}

void PropertyDelegate::setModelData(QWidget*            editor,
                                    QAbstractItemModel* model,
                                    const QModelIndex&  index) const
{
    if (!index.isValid() || !editor) return;

    const rttr::variant oldVal = index.data(RttrVariantRole).value<rttr::variant>();
    const rttr::type    t      = oldVal.get_type();

    rttr::variant newVal;

    if (auto* sb = qobject_cast<QDoubleSpinBox*>(editor)) {
        if (t == rttr::type::get<double>())
            newVal = sb->value();
        else if (t == rttr::type::get<float>())
            newVal = static_cast<float>(sb->value());
    } else if (auto* sb = qobject_cast<QSpinBox*>(editor)) {
        if (t == rttr::type::get<int>())
            newVal = sb->value();
        else if (t == rttr::type::get<unsigned int>())
            newVal = static_cast<unsigned int>(sb->value());
    } else if (auto* cb = qobject_cast<QCheckBox*>(editor)) {
        newVal = cb->isChecked();
    } else if (auto* le = qobject_cast<QLineEdit*>(editor)) {
        if (t == rttr::type::get<std::string>())
            newVal = le->text().toStdString();
        else if (t == rttr::type::get<QString>())
            newVal = le->text();
    } else if (auto* cb = qobject_cast<QComboBox*>(editor)) {
        if (t.is_enumeration()) {
            const auto en = t.get_enumeration();
            bool ok = false;
            newVal  = en.name_to_value(cb->currentText().toStdString(), ok);
        }
    }

    if (newVal.is_valid())
        model->setData(index, QVariant::fromValue(newVal), Qt::EditRole);
}

void PropertyDelegate::updateEditorGeometry(QWidget*                    editor,
                                            const QStyleOptionViewItem& option,
                                            const QModelIndex&          /*index*/) const
{
    editor->setGeometry(option.rect);
}

void PropertyDelegate::paint(QPainter*                   painter,
                             const QStyleOptionViewItem& option,
                             const QModelIndex&          index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    // Override rows get a warm background — already set via Qt::BackgroundRole;
    // just let the default paint handle it.
    QStyledItemDelegate::paint(painter, opt, index);
}

} // namespace rpe

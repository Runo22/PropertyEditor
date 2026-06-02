#include "rttr_property_editor/ecs/ComponentListWidget.h"

#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QVBoxLayout>

#include <rttr/type>

namespace rpe {

ComponentListWidget::ComponentListWidget(QWidget* parent)
    : QWidget(parent)
{
    _setupUi();
}

void ComponentListWidget::_setupUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* header = new QLabel(QStringLiteral("Components"), this);
    header->setStyleSheet(QStringLiteral("font-weight: bold; padding: 2px 4px;"));
    layout->addWidget(header);

    _listWidget = new QListWidget(this);
    layout->addWidget(_listWidget, 1);

    connect(_listWidget, &QListWidget::currentItemChanged,
            this,        &ComponentListWidget::_onSelectionChanged);
}

void ComponentListWidget::setEntity(flecs::world* world, flecs::entity e)
{
    _components.clear();
    _listWidget->blockSignals(true);
    _listWidget->clear();

    if (!world || !e.is_alive()) {
        _listWidget->blockSignals(false);
        return;
    }

    e.each([&](flecs::id id) {
        if (!id.is_entity()) return;

        flecs::entity compEntity = id.entity();
        const char*   rawName   = compEntity.name();
        if (!rawName || rawName[0] == '\0') return;

        const QString qname = QString::fromUtf8(rawName);

        // Auto-discovery: match component name to rttr type name
        rttr::type t = rttr::type::get_by_name(qname.toStdString());
        if (!t.is_valid()) return;

        // Get mutable pointer to component data
        void* ptr = e.get_mut_w_id(id);
        if (!ptr) return;

        ComponentInfo info{ id, t, ptr };
        _components.append(info);

        auto* item = new QListWidgetItem(qname);
        item->setData(Qt::UserRole, static_cast<int>(_components.size() - 1));
        _listWidget->addItem(item);
    });

    _listWidget->blockSignals(false);
}

void ComponentListWidget::clearEntity()
{
    _components.clear();
    _listWidget->clear();
    emit componentDeselected();
}

void ComponentListWidget::_onSelectionChanged()
{
    auto* item = _listWidget->currentItem();
    if (!item) {
        emit componentDeselected();
        return;
    }
    const int idx = item->data(Qt::UserRole).toInt();
    if (idx >= 0 && idx < _components.size())
        emit componentSelected(_components[idx]);
}

} // namespace rpe

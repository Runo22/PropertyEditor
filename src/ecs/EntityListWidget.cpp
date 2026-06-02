#include "rttr_property_editor/ecs/EntityListWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace rpe {

EntityListWidget::EntityListWidget(QWidget* parent)
    : QWidget(parent)
{
    _setupUi();
    _refreshTimer = new QTimer(this);
    _refreshTimer->setInterval(500);
    connect(_refreshTimer, &QTimer::timeout, this, &EntityListWidget::_refresh);
}

void EntityListWidget::_setupUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* header = new QLabel(QStringLiteral("Entities"), this);
    header->setStyleSheet(QStringLiteral("font-weight: bold; padding: 2px 4px;"));
    layout->addWidget(header);

    _filterEdit = new QLineEdit(this);
    _filterEdit->setPlaceholderText(QStringLiteral("Filter entities..."));
    _filterEdit->setClearButtonEnabled(true);
    layout->addWidget(_filterEdit);

    _listWidget = new QListWidget(this);
    layout->addWidget(_listWidget, 1);

    connect(_listWidget, &QListWidget::currentItemChanged,
            this,        &EntityListWidget::_onSelectionChanged);
    connect(_filterEdit,  &QLineEdit::textChanged,
            this,        &EntityListWidget::_onFilterChanged);
}

void EntityListWidget::setWorld(flecs::world* world)
{
    _world = world;
    if (_world)
        _refreshTimer->start();
    else
        _refreshTimer->stop();
    _refresh();
}

void EntityListWidget::setRefreshIntervalMs(int ms)
{
    _refreshTimer->setInterval(ms);
}

void EntityListWidget::_refresh()
{
    if (!_world) {
        _listWidget->clear();
        return;
    }

    const QString filter = _filterEdit->text().toLower();

    // Remember current selection
    flecs::entity_t selectedId = 0;
    if (auto* cur = _listWidget->currentItem())
        selectedId = static_cast<flecs::entity_t>(cur->data(Qt::UserRole).toULongLong());

    _listWidget->blockSignals(true);
    _listWidget->clear();

    _world->each([&](flecs::entity e) {
        if (!e.is_alive()) return;

        const char* rawName = e.name();
        const QString name  = rawName ? QString::fromUtf8(rawName) : QStringLiteral("(unnamed)");
        const QString label = QStringLiteral("%1  %2").arg(e.id()).arg(name);

        if (!filter.isEmpty() && !label.toLower().contains(filter))
            return;

        auto* item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, static_cast<qulonglong>(e.id()));
        _listWidget->addItem(item);

        if (e.id() == selectedId)
            _listWidget->setCurrentItem(item);
    });

    _listWidget->blockSignals(false);

    // If previously selected entity is gone or selection changed, re-emit
    if (!_listWidget->currentItem() && selectedId != 0)
        emit entityDeselected();
}

void EntityListWidget::_onSelectionChanged()
{
    auto* item = _listWidget->currentItem();
    if (!item) {
        emit entityDeselected();
        return;
    }
    const flecs::entity_t id = static_cast<flecs::entity_t>(
        item->data(Qt::UserRole).toULongLong());
    if (_world) {
        // world.entity(id) wraps the id; is_alive() checks liveness
        flecs::entity e = _world->entity(id);
        if (e.is_alive())
            emit entitySelected(e);
    }
}

void EntityListWidget::_onFilterChanged(const QString&)
{
    _refresh();
}

} // namespace rpe

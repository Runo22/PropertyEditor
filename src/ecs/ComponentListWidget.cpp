#include "rpe/ecs/ComponentListWidget.h"

#include "rpe/core/TypeBridge.h"

#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QVBoxLayout>

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

    auto* header = new QLabel(tr("Components"), this);
    header->setStyleSheet(QStringLiteral("font-weight: bold; padding: 2px 4px;"));
    layout->addWidget(header);

    _list = new QListWidget(this);
    layout->addWidget(_list, 1);

    connect(_list, &QListWidget::currentItemChanged,
            this,  &ComponentListWidget::_onSelectionChanged);
}

void ComponentListWidget::setEntity(flecs::world* world, flecs::entity e)
{
    _components.clear();
    _mirrorNames.clear();

    // Collect under the guard (entity/component reads touch the world);
    // populate the widget afterwards without holding it.
    QStringList names;
    withGuard(_guard, [&] {
        if (!world || !e.is_alive()) return;
        e.each([&](flecs::id id) {
            if (!id.is_entity()) return;
            flecs::entity comp = id.entity();
            const char* raw = comp.name();
            if (!raw || raw[0] == '\0') return;

            const rttr::type t = rttr::type::get_by_name(raw);
            if (!t.is_valid()) return;
            // Only components we can actually inspect/edit (a TypeBridge wrapper
            // turns the raw component pointer into an RTTR instance).
            if (!TypeBridge::has(t)) return;

            _components.append(ComponentInfo{ id, t });
            names.append(QString::fromUtf8(raw));
        });
    });

    _list->blockSignals(true);
    _list->clear();
    for (int i = 0; i < names.size(); ++i) {
        auto* item = new QListWidgetItem(names[i], _list);
        item->setData(Qt::UserRole, i);
    }
    _list->blockSignals(false);
    if (_list->count() > 0)
        _list->setCurrentRow(0);          // auto-select first → drives the editor
    else
        emit componentDeselected();
}

void ComponentListWidget::setWorldAccess(AccessGuard guard) { _guard = std::move(guard); }

void ComponentListWidget::setComponentNames(const QStringList& names)
{
    if (names == _mirrorNames) return;   // unchanged → keep selection, no flicker
    _mirrorNames = names;
    _components.clear();                  // mirror mode has no world-backed infos

    const QString prevSel = _list->currentItem() ? _list->currentItem()->text() : QString();

    _list->blockSignals(true);
    _list->clear();
    QListWidgetItem* reselect = nullptr;
    for (const QString& n : names) {
        auto* item = new QListWidgetItem(n, _list);
        if (n == prevSel) reselect = item;
    }
    _list->blockSignals(false);

    if (reselect)            _list->setCurrentItem(reselect);
    else if (_list->count()) _list->setCurrentRow(0);
    else                     emit componentDeselected();
}

void ComponentListWidget::clearEntity()
{
    _components.clear();
    _mirrorNames.clear();
    _list->clear();
    emit componentDeselected();
}

void ComponentListWidget::_onSelectionChanged()
{
    auto* item = _list->currentItem();
    if (!item) { emit componentDeselected(); return; }
    emit componentNameSelected(item->text());   // world-free; mirror mode
    const int idx = item->data(Qt::UserRole).toInt();
    if (idx >= 0 && idx < _components.size())
        emit componentSelected(_components[idx]);
}

} // namespace rpe

#include "rttr_property_editor/ecs/EntityComponentBrowser.h"
#include "rttr_property_editor/ecs/EntityListWidget.h"
#include "rttr_property_editor/ecs/ComponentListWidget.h"
#include "rttr_property_editor/widgets/PropertyEditor.h"

#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <rttr/instance>

namespace rpe {

EntityComponentBrowser::EntityComponentBrowser(QWidget* parent)
    : QWidget(parent)
{
    _setupUi();

    _liveTimer = new QTimer(this);
    _liveTimer->setInterval(20); // 50 Hz default
    connect(_liveTimer, &QTimer::timeout, this, &EntityComponentBrowser::_onLiveUpdate);
}

void EntityComponentBrowser::_setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    _hSplitter = new QSplitter(Qt::Horizontal, this);

    _entityList = new EntityListWidget(_hSplitter);
    _hSplitter->addWidget(_entityList);

    _vSplitter = new QSplitter(Qt::Vertical, _hSplitter);
    _componentList  = new ComponentListWidget(_vSplitter);
    _propertyEditor = new PropertyEditor(_vSplitter);
    _vSplitter->addWidget(_componentList);
    _vSplitter->addWidget(_propertyEditor);
    _vSplitter->setStretchFactor(0, 1);
    _vSplitter->setStretchFactor(1, 3);

    _hSplitter->addWidget(_vSplitter);
    _hSplitter->setStretchFactor(0, 1);
    _hSplitter->setStretchFactor(1, 2);

    mainLayout->addWidget(_hSplitter);

    connect(_entityList, &EntityListWidget::entitySelected,
            this,        &EntityComponentBrowser::_onEntitySelected);
    connect(_entityList, &EntityListWidget::entityDeselected,
            this,        &EntityComponentBrowser::_onEntityDeselected);
    connect(_componentList, &ComponentListWidget::componentSelected,
            this,           &EntityComponentBrowser::_onComponentSelected);
    connect(_componentList, &ComponentListWidget::componentDeselected,
            this,           &EntityComponentBrowser::_onComponentDeselected);
    connect(_propertyEditor, &PropertyEditor::propertyEdited,
            this,            &EntityComponentBrowser::propertyEdited);
}

void EntityComponentBrowser::setWorld(flecs::world* world)
{
    _world = world;
    _entityList->setWorld(world);
    _liveTimer->stop();
    _componentList->clearEntity();
    _propertyEditor->unbind();
}

void EntityComponentBrowser::setLiveUpdateIntervalMs(int ms)
{
    _liveTimer->setInterval(ms);
}

void EntityComponentBrowser::_onEntitySelected(flecs::entity e)
{
    _selectedEntity = e;
    _liveTimer->stop();
    _componentList->setEntity(_world, e);
    _propertyEditor->unbind();
}

void EntityComponentBrowser::_onEntityDeselected()
{
    _liveTimer->stop();
    _componentList->clearEntity();
    _propertyEditor->unbind();
    _selectedEntity = {};
}

void EntityComponentBrowser::_onComponentSelected(ComponentInfo info)
{
    _selectedComponent = info;
    if (!info.rttrType.is_valid() || !info.ptr) return;

    _propertyEditor->bindType(info.rttrType);

    rttr::instance inst(info.rttrType, info.ptr);
    _propertyEditor->refresh(inst);

    _liveTimer->start();
}

void EntityComponentBrowser::_onComponentDeselected()
{
    _liveTimer->stop();
    _propertyEditor->unbind();
    _selectedComponent = {};
}

void EntityComponentBrowser::_onLiveUpdate()
{
    if (!_world || !_selectedEntity.is_alive()) return;
    if (!_selectedComponent.rttrType.is_valid()) return;

    // Re-fetch the component pointer (stable within a frame in Flecs)
    void* ptr = _selectedEntity.get_mut_w_id(_selectedComponent.id);
    if (!ptr) return;

    rttr::instance inst(_selectedComponent.rttrType, ptr);
    _propertyEditor->refresh(inst);
}

} // namespace rpe

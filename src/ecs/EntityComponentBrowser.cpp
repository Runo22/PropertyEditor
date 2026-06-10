#include "rpe/ecs/EntityComponentBrowser.h"

#include "rpe/ecs/EntityListWidget.h"
#include "rpe/gui/PropertyEditor.h"

#include <QCheckBox>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <rttr/instance.h>

namespace rpe {

EntityComponentBrowser::EntityComponentBrowser(QWidget* parent)
    : QWidget(parent)
{
    _setupUi();
    _liveTimer = new QTimer(this);
    _liveTimer->setInterval(20);   // 50 Hz default
    connect(_liveTimer, &QTimer::timeout, this, &EntityComponentBrowser::_onLiveUpdate);
}

void EntityComponentBrowser::_setupUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* hSplit = new QSplitter(Qt::Horizontal, this);

    _entityList = new EntityListWidget(hSplit);
    hSplit->addWidget(_entityList);

    auto* right   = new QWidget(hSplit);
    auto* rLayout = new QVBoxLayout(right);
    rLayout->setContentsMargins(0, 0, 0, 0);
    rLayout->setSpacing(2);

    _writeCheck = new QCheckBox(tr("Write edits back to world"), right);
    _writeCheck->setToolTip(tr("On: edits modify the live component.\n"
                               "Off: edits are pinned as overrides only."));
    rLayout->addWidget(_writeCheck);

    auto* vSplit = new QSplitter(Qt::Vertical, right);
    _componentList  = new ComponentListWidget(vSplit);
    _propertyEditor = new PropertyEditor(vSplit);
    vSplit->addWidget(_componentList);
    vSplit->addWidget(_propertyEditor);
    vSplit->setStretchFactor(0, 1);
    vSplit->setStretchFactor(1, 3);
    rLayout->addWidget(vSplit, 1);

    hSplit->addWidget(right);
    hSplit->setStretchFactor(0, 1);
    hSplit->setStretchFactor(1, 2);
    layout->addWidget(hSplit);

    // The editor needs the *current* component pointer at read/write time.
    // We relink a persistent wrapper so the returned instance stays valid for
    // the duration of the synchronous get/set.
    _propertyEditor->setInstanceProvider([this]() -> rttr::instance {
        void* p = _liveComponentPtr();
        if (!p) return rttr::instance();
        _liveWrapper.relink(p);
        return _liveWrapper.instance();
    });

    connect(_entityList, &EntityListWidget::entitySelected,
            this, &EntityComponentBrowser::_onEntitySelected);
    connect(_entityList, &EntityListWidget::entityDeselected,
            this, &EntityComponentBrowser::_onEntityDeselected);
    connect(_componentList, &ComponentListWidget::componentSelected,
            this, &EntityComponentBrowser::_onComponentSelected);
    connect(_componentList, &ComponentListWidget::componentDeselected,
            this, &EntityComponentBrowser::_onComponentDeselected);
    connect(_propertyEditor, &PropertyEditor::propertyEdited,
            this, &EntityComponentBrowser::propertyEdited);
    connect(_writeCheck, &QCheckBox::toggled,
            this, &EntityComponentBrowser::_onWriteToggled);
}

void EntityComponentBrowser::setWorld(flecs::world* world)
{
    _world = world;
    _entityList->setWorld(world);
    _liveTimer->stop();
    _componentList->clearEntity();
    _propertyEditor->unbind();
}

void EntityComponentBrowser::setLiveUpdateIntervalMs(int ms) { _liveTimer->setInterval(ms); }

void EntityComponentBrowser::setEntityComponentFilter(const QString& name, bool enabled)
{
    _entityList->setRequiredComponent(name, enabled);
}

void EntityComponentBrowser::setEditPolicy(EditPolicy p)
{
    _propertyEditor->setEditPolicy(p);
    _writeCheck->setChecked(p == EditPolicy::WriteBack);
}

void EntityComponentBrowser::_onWriteToggled(bool on)
{
    _propertyEditor->setEditPolicy(on ? EditPolicy::WriteBack : EditPolicy::Override);
}

void* EntityComponentBrowser::_liveComponentPtr() const
{
    if (!_world || !_selectedEntity.is_alive() || !_selectedComponent.rttrType.is_valid())
        return nullptr;
    return _selectedEntity.get_mut(_selectedComponent.id.raw_id());
}

void EntityComponentBrowser::_onEntitySelected(flecs::entity e)
{
    _selectedEntity = e;
    _liveTimer->stop();
    _componentList->setEntity(_world, e);   // auto-selects first component
    emit entitySelected(e);
}

void EntityComponentBrowser::_onEntityDeselected()
{
    _liveTimer->stop();
    _selectedEntity = {};
    _componentList->clearEntity();
    _propertyEditor->unbind();
    emit entityDeselected();
}

void EntityComponentBrowser::_onComponentSelected(ComponentInfo info)
{
    _selectedComponent = info;
    if (!info.rttrType.is_valid()) return;

    _propertyEditor->bindType(info.rttrType);
    if (void* p = _liveComponentPtr()) {
        _liveWrapper = RttrVariantWrapper::makeLinked(info.rttrType, p);
        _propertyEditor->refresh(_liveWrapper.instance());
    }
    _liveTimer->start();
    emit componentSelected(info);
}

void EntityComponentBrowser::_onComponentDeselected()
{
    _liveTimer->stop();
    _selectedComponent = {};
    _propertyEditor->unbind();
    emit componentDeselected();
}

void EntityComponentBrowser::_onLiveUpdate()
{
    if (void* p = _liveComponentPtr()) {
        _liveWrapper.relink(p);
        _propertyEditor->refresh(_liveWrapper.instance());
    }
}

} // namespace rpe

#pragma once

#include "rpe/core/rttr_prelude.h"

#include "rpe/core/RttrVariantWrapper.h"
#include "rpe/ecs/ComponentListWidget.h"
#include "rpe/gui/PropertyModel.h"

#include <QWidget>

#include "rpe/ecs/flecs_prelude.h"

class QSplitter;
class QCheckBox;
class QTimer;

namespace rpe {

class EntityListWidget;
class PropertyEditor;

// ─────────────────────────────────────────────────────────────────────────────
//  EntityComponentBrowser — UE5-style three-level inspector:
//
//      Entities  ──▶  Components  ──▶  Properties
//
//  Pick an entity (optionally filtered to those with a Transform), pick one of
//  its RTTR-registered components, then view/edit its properties live. Edits can
//  either be pinned as overrides or written straight back into the world.
//
//  Embeddable as a window, a QDockWidget, or a side panel.
// ─────────────────────────────────────────────────────────────────────────────
class EntityComponentBrowser : public QWidget
{
    Q_OBJECT
public:
    explicit EntityComponentBrowser(QWidget* parent = nullptr);

    void setWorld(flecs::world* world);
    void setLiveUpdateIntervalMs(int ms);

    // Install when the flecs world is advanced by another thread (simulation
    // thread). Every world touch — entity/component enumeration, the 50Hz live
    // refresh, and WriteBack edits — then runs through this guard. Typical
    // implementation: lock a mutex the sim loop also takes around progress().
    // See rpe/core/AccessGuard.h for the contract and an example.
    void setWorldAccess(AccessGuard guard);

    // Restrict the entity list to entities having this component (e.g. "Transform").
    void setEntityComponentFilter(const QString& componentName, bool enabledByDefault = true);

    // Default edit policy for the property editor (Override or WriteBack).
    void setEditPolicy(EditPolicy p);

    PropertyEditor*      propertyEditor() const { return _propertyEditor; }
    EntityListWidget*    entityList()     const { return _entityList; }
    ComponentListWidget* componentList()  const { return _componentList; }

signals:
    void propertyEdited(const QString& path, const rttr::variant& newValue);

    // Selection pass-throughs so a host application can react to what the user
    // is inspecting (e.g. highlight the entity in a 3D view).
    void entitySelected(flecs::entity e);
    void entityDeselected();
    void componentSelected(const ComponentInfo& info);
    void componentDeselected();

private slots:
    void _onEntitySelected(flecs::entity e);
    void _onEntityDeselected();
    void _onComponentSelected(ComponentInfo info);
    void _onComponentDeselected();
    void _onLiveUpdate();
    void _onWriteToggled(bool on);

private:
    void  _setupUi();
    void* _liveComponentPtr() const;

    flecs::world*        _world          = nullptr;
    EntityListWidget*    _entityList     = nullptr;
    ComponentListWidget* _componentList  = nullptr;
    PropertyEditor*      _propertyEditor = nullptr;
    QCheckBox*           _writeCheck     = nullptr;
    QTimer*              _liveTimer      = nullptr;

    flecs::entity        _selectedEntity;
    ComponentInfo        _selectedComponent;
    RttrVariantWrapper   _liveWrapper;   // persistent storage backing the editor's instance
    AccessGuard          _guard;
};

} // namespace rpe

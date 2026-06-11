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
class EcsMirror;

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

    // ── Mirror mode (recommended for a separate simulation thread) ───────────
    // Drive the browser entirely from an EcsMirror instead of touching the world
    // from the GUI thread. The mirror's per-frame system runs inside your
    // world.progress() on the sim thread, so neither thread blocks and no mutex
    // is needed in your loop. Call instead of setWorld. See rpe/ecs/EcsMirror.h.
    void setMirror(EcsMirror* mirror);

    // When mirroring, copy/poll only the fields currently expanded in the tree
    // (default true) — collapsed/large fields cost nothing until opened.
    void setSnapshotOpenFieldsOnly(bool on) { _openFieldsOnly = on; }
    bool snapshotOpenFieldsOnly() const     { return _openFieldsOnly; }

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

    // mirror mode
    void _onMirrorPoll();
    void _onEntityIdSelected(qulonglong id);
    void _onComponentNameSelected(const QString& name);

private:
    void  _setupUi();
    void* _liveComponentPtr() const;
    void  _pushInterest();

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

    // mirror mode
    EcsMirror*           _mirror         = nullptr;
    QTimer*              _mirrorTimer    = nullptr;
    qulonglong           _mirrorEntity   = 0;
    QString              _mirrorComponent;
    bool                 _openFieldsOnly = true;
};

} // namespace rpe

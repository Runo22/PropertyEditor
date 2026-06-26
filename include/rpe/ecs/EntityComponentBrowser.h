#pragma once

#include "rpe/core/rttr_prelude.h"

#include "rpe/core/RttrVariantWrapper.h"
#include "rpe/ecs/ComponentListWidget.h"
#include "rpe/ecs/MirrorChannel.h"
#include "rpe/gui/PropertyModel.h"

#include <QWidget>

#include <memory>

#include "rpe/ecs/flecs_prelude.h"

class QSplitter;
class QCheckBox;
class QTimer;
class QVBoxLayout;

namespace rpe
{

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
        // Panel arrangement.
        enum class Layout
        {
            Wide,     // entities | (components / properties) — wide docks
            Vertical, // entities / components / properties stacked — UE-style sidebar
        };

        // Bundled browser options, with a single setter/getter (see settings()).
        // Lets a host snapshot/restore the inspector's configuration in one call.
        struct Settings
        {
            // Entity-list filter: when requiredComponentEnabled is true and the name
            // is non-empty, only entities carrying that component are listed. This
            // is the sole control for the filter (there is no in-list checkbox).
            QString requiredComponent;
            bool requiredComponentEnabled = true;
            // Mirror only the property leaves currently expanded in the tree.
            bool snapshotOpenFieldsOnly = true;
            // How property edits are applied (Override vs WriteBack).
            EditPolicy editPolicy = EditPolicy::Override;
            // Show the add/remove-component controls ("+" and per-row "×").
            bool allowComponentEditing = false;
            // Panel arrangement.
            Layout layout = Layout::Wide;
            // GUI poll cadence (ms) for the mirror and the direct-mode live refresh.
            int mirrorPollIntervalMs = 33;
            int liveUpdateIntervalMs = 20;
        };

        explicit EntityComponentBrowser(QWidget* parent = nullptr);

        void setWorld(flecs::world* world);
        void setLiveUpdateIntervalMs(int ms);

        // Switch panel arrangement (default Wide). Vertical stacks the three
        // panels top-to-bottom for a narrow Unreal-style sidebar.
        void setBrowserLayout(Layout layout);
        Layout browserLayout() const
        {
            return _browserLayout;
        }

        // ── Mirror mode (recommended for a separate simulation thread) ───────────
        // Drive the browser entirely from an EcsMirror instead of touching the world
        // from the GUI thread. The mirror's per-frame system runs inside your
        // world.progress() on the sim thread, so neither thread blocks and no mutex
        // is needed in your loop. Call instead of setWorld. See rpe/ecs/EcsMirror.h.
        void setMirror(EcsMirror* mirror);

        // When mirroring, copy/poll only the fields currently expanded in the tree
        // (default true) — collapsed/large fields cost nothing until opened.
        void setSnapshotOpenFieldsOnly(bool on)
        {
            _openFieldsOnly = on;
            _settings.snapshotOpenFieldsOnly = on;
        }
        bool snapshotOpenFieldsOnly() const
        {
            return _openFieldsOnly;
        }

        // Install when the flecs world is advanced by another thread (simulation
        // thread). Every world touch — entity/component enumeration, the 50Hz live
        // refresh, and WriteBack edits — then runs through this guard. Typical
        // implementation: lock a mutex the sim loop also takes around progress().
        // See rpe/core/AccessGuard.h for the contract and an example.
        void setWorldAccess(AccessGuard guard);

        // Default edit policy for the property editor (Override or WriteBack).
        void setEditPolicy(EditPolicy p);

        // Allow the user to add/remove components on the selected entity from the
        // component panel ("+" button and per-row "×"). Off by default. In mirror
        // mode the change is queued to the simulation thread; in direct mode it is
        // applied under the world guard. Requires a registered component catalog.
        void setComponentEditingEnabled(bool on);
        bool isComponentEditingEnabled() const
        {
            return _settings.allowComponentEditing;
        }

        // Bundled get/set of all the above options. setSettings applies every field
        // (filter, snapshot policy, edit policy, component-editing, layout, timers);
        // settings() returns the current configuration.
        void setSettings(const Settings& s);
        Settings settings() const
        {
            return _settings;
        }

        PropertyEditor* propertyEditor() const
        {
            return _propertyEditor;
        }
        EntityListWidget* entityList() const
        {
            return _entityList;
        }
        ComponentListWidget* componentList() const
        {
            return _componentList;
        }

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

        // component add/remove
        void _onAddComponent(const QString& name);
        void _onRemoveComponent(const QString& name);

    private:
        void _setupUi();
        void _applyLayout(Layout layout);
        void* _liveComponentPtr() const;
        void _pushInterest();
        // Recompute the "+" picker list: catalog minus components already present.
        void _updateAddable();
        // Apply the required-component filter (from _settings) to the entity list and,
        // in mirror mode, to the producer.
        void _applyEntityFilter();

        flecs::world* _world = nullptr;
        EntityListWidget* _entityList = nullptr;
        ComponentListWidget* _componentList = nullptr;
        PropertyEditor* _propertyEditor = nullptr;
        QCheckBox* _writeCheck = nullptr;
        QTimer* _liveTimer = nullptr;
        QVBoxLayout* _mainLayout = nullptr; // host layout; _layoutRoot swapped inside
        QWidget* _layoutRoot = nullptr;     // current splitter tree
        Layout _browserLayout = Layout::Wide;

        flecs::entity _selectedEntity;
        ComponentInfo _selectedComponent;
        RttrVariantWrapper _liveWrapper; // persistent storage backing the editor's instance
        AccessGuard _guard;

        // mirror mode — hold the shared channel (not the EcsMirror), so the GUI
        // keeps it alive and never dereferences a mirror destroyed on the sim
        // thread first. See MirrorChannel.
        std::shared_ptr<MirrorChannel> _channel;
        QTimer* _mirrorTimer = nullptr;
        qulonglong _mirrorEntity = 0;
        QString _mirrorComponent;
        bool _openFieldsOnly = true;

        // Add/remove-component state (mirror mode).
        QStringList _catalog;       // all bridged component names in the world
        QStringList _currentComps;  // components on the selected entity
        Settings _settings;
    };

} // namespace rpe

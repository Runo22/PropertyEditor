#pragma once

#include <QHash>
#include <QIcon>
#include <QPair>
#include <QVector>
#include <QWidget>

#include "rpe/core/AccessGuard.h"
#include "rpe/ecs/MirrorChannel.h"
#include "rpe/ecs/RowActions.h"
#include "rpe/ecs/flecs_prelude.h"

class QListWidget;
class QLineEdit;
class QTimer;
class QToolButton;
class QStyledItemDelegate;
class QMenu;

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  EntityListWidget — lists the entities of a flecs::world that carry at least
    //  one bridged component (the ones the inspector can display), labelled by name,
    //  else prefab name + id, else id.
    //
    //  A text filter box narrows the visible rows by label (live, both modes). An
    //  optional "required component" filter shows only entities that have a given
    //  component — the "list the ones with a transform" behaviour. Direct mode
    //  refreshes on a slow timer (the set rarely changes at frame rate); mirror mode
    //  is fed via setEntries().
    // ─────────────────────────────────────────────────────────────────────────────
    class EntityListWidget : public QWidget
    {
        Q_OBJECT

    public:
        explicit EntityListWidget(QWidget* parent = nullptr);

        void setWorld(flecs::world* world);
        void setRefreshIntervalMs(int ms);

        // Component to filter the (direct-mode) list by, plus whether the filter is
        // active. Empty name or enabled=false = no filter. Configured via the
        // browser's Settings — there is no in-list checkbox.
        void setRequiredComponent(const QString& componentName, bool enabled = true);

        // Guard wrapped around world reads when the world is owned by another
        // thread (see rpe/core/AccessGuard.h).
        void setWorldAccess(AccessGuard guard);

        // Stop the world-polling timer (mirror mode feeds the list via setEntries).
        void stopAutoRefresh();
        // Externally provided (id, label) entries — used by EcsMirror integration.
        void setEntries(const QVector<QPair<qulonglong, QString>>& entries);

        // Select the entity with this id. If it isn't in the list yet (e.g. the
        // mirror hasn't fed it), the request is remembered and applied as soon as it
        // appears. Returns true if it was selected immediately. Call on the GUI thread.
        bool selectById(qulonglong id);

        // Display label of the currently selected entity (empty if none).
        QString currentLabel() const;

        // ── Add-entity (prefab spawning) ─────────────────────────────────────────
        // Show the "Add" button (a spawn picker of prefabs). Off by default.
        void setEntityAddingEnabled(bool on);
        // Spawnable prefabs offered by the picker, grouped by their `group` tag.
        void setAddablePrefabs(const QVector<MirrorChannel::PrefabEntry>& prefabs);
        // Optional icon shown next to a group header, keyed by the group tag name.
        void setPrefabGroupIcons(const QHash<QString, QIcon>& icons);

        // ── Deletion + context menu ──────────────────────────────────────────────
        // Show a per-row trash glyph (two-step confirm, like the component list) and
        // a "Delete" entry in the right-click menu; both emit removeEntityRequested.
        void setEntityRemovingEnabled(bool on);
        // Extra right-click entries (run app-specific work on the clicked entity).
        void setContextActions(const QVector<EntityAction>& actions);
        // Dynamic hook: called each time the entity right-click menu is built, with
        // the clicked entity id and the live QMenu, so the host can add whatever
        // buttons/submenus it wants — conditional on that entity. Runs after the
        // built-in Delete and any static setContextActions entries.
        void setContextMenuHook(std::function<void(qulonglong entityId, QMenu& menu)> hook);

    signals:
        void entitySelected(flecs::entity e); // direct mode (world available)
        void entityIdSelected(qulonglong id); // always; mirror mode uses this
        void entityDeselected();
        // The user picked a prefab to spawn; the host instantiates it (mirror queues
        // a SpawnPrefab structural, direct mode spawns under the world guard).
        void spawnPrefabRequested(qulonglong prefabId);
        // The user asked to delete an entity (trash glyph or the menu's "Delete").
        void removeEntityRequested(qulonglong entityId);

    private slots:
        void _refresh();
        void _onSelectionChanged();
        void _onAddEntityClicked();
        void _onContextMenu(const QPoint& pos);

    private:
        void _setupUi();
        void _applyEntries(const QVector<QPair<qulonglong, QString>>& entries);
        // Re-derive the visible rows from _sourceEntries by applying the text filter.
        // Used when only the filter text changed (no need to re-query / re-feed).
        void _applyTextFilter();

        flecs::world* _world = nullptr;
        QListWidget* _list = nullptr;
        QLineEdit* _filterEdit = nullptr;
        QToolButton* _addBtn = nullptr;
        QTimer* _timer = nullptr;
        QStyledItemDelegate* _rowDelegate = nullptr; // trash-button delegate
        QVector<MirrorChannel::PrefabEntry> _prefabs;
        QHash<QString, QIcon> _groupIcons;
        QVector<EntityAction> _contextActions;
        std::function<void(qulonglong, QMenu&)> _menuHook;
        QString _requiredComponent;
        bool _requiredEnabled = false;
        AccessGuard _guard;
        // Full, unfiltered (id, label) set — from the world query (direct mode) or
        // the mirror feed (mirror mode). The text filter is applied on top of this,
        // so editing/clearing the filter never loses entries.
        QVector<QPair<qulonglong, QString>> _sourceEntries;
        // Last visible (id, label) set — refresh skips the rebuild when unchanged.
        QVector<QPair<qulonglong, QString>> _lastEntries;
        // A pending selectById() request whose entity isn't in the list yet (0 = none).
        // Applied on the next rebuild that contains it.
        qulonglong _requestedId = 0;
        // The authoritative current selection (0 = none). Updated on every real
        // selection change; a list rebuild (entity add/remove) keeps this selection
        // when it survives — silently, so a host driving selection isn't disturbed —
        // and only falls back to the first entity when it's genuinely gone.
        qulonglong _selectedId = 0;
    };

} // namespace rpe

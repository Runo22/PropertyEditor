#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QStringList>
#include <QVector>
#include <QWidget>

#include "rpe/core/AccessGuard.h"
#include "rpe/ecs/MirrorChannel.h"
#include "rpe/ecs/flecs_prelude.h"

#include <QIcon>

#include <functional>

class QListWidget;
class QLineEdit;
class QToolButton;
class QStyledItemDelegate;
class QEvent;
class QPoint;
class QMenu;

namespace rpe
{

    // Resolved component of an entity that has a matching RTTR type.
    struct ComponentInfo
    {
        flecs::id id;
        rttr::type rttrType = rttr::type::get<void>(); // rttr::type has no default ctor
    };

    // ─────────────────────────────────────────────────────────────────────────────
    //  ComponentListWidget — lists the RTTR-discoverable components on an entity.
    //
    //  Auto-discovery: a flecs component is shown when its name resolves to a
    //  registered rttr::type (rttr::type::get_by_name). No manual registration of
    //  the component<->type mapping is required.
    // ─────────────────────────────────────────────────────────────────────────────
    class ComponentListWidget : public QWidget
    {
        Q_OBJECT

    public:
        explicit ComponentListWidget(QWidget* parent = nullptr);

        void setEntity(flecs::world* world, flecs::entity e);
        void clearEntity();

        // Guard wrapped around world reads when the world is owned by another
        // thread (see rpe/core/AccessGuard.h).
        void setWorldAccess(AccessGuard guard);

        // Externally provided component names (mirror mode); selection is reported
        // via componentNameSelected. Convenience wrapper over setComponentRows with
        // every entry as a DATA row.
        void setComponentNames(const QStringList& names);

        // Full composition (mirror mode): data components plus TAG / PAIR badge
        // rows. Tag/pair rows are not selectable (nothing to inspect) but carry the
        // flecs id so the per-row remove works on them too.
        void setComponentRows(const QVector<MirrorChannel::ComponentRow>& rows);

        // Currently-selected component name, or empty if none. Used to re-bind the
        // property tree to the same component after an entity switch (the list keeps
        // its selection but doesn't re-emit when the component set is unchanged).
        QString currentComponentName() const;

        // Show the add/remove controls (a "+" button and a per-row "×"). Off by
        // default — inspection is read-only unless the host opts in.
        void setComponentEditingEnabled(bool on);
        bool isComponentEditingEnabled() const
        {
            return _editingEnabled;
        }

        // Component names offered by the "+" picker (typically the world catalog
        // minus the components already on the entity). Entries flagged as tags are
        // rendered dimmed/italic in the picker.
        void setAddableComponents(const QStringList& names);
        void setAddableEntries(const QVector<MirrorChannel::CatalogEntry>& entries);

        // Extra right-click menu entries. The callback receives the clicked row's
        // selection key and its flecs id; the browser wraps them to add the entity.
        struct MenuAction
        {
            QString label;
            QIcon icon;
            std::function<void(const QString& key, qulonglong rawId)> callback;
        };
        void setContextActions(const QVector<MenuAction>& actions);
        // Dynamic hook: called when the component right-click menu is built, with the
        // clicked row's key + flecs id and the live QMenu (host adds its own entries).
        void setContextMenuHook(std::function<void(const QString& key, qulonglong rawId, QMenu& menu)> hook);

    signals:
        void componentSelected(ComponentInfo info);      // direct mode (world available)
        void componentNameSelected(const QString& name); // mirror mode
        void componentDeselected();

        // Structural-edit requests (only emitted when editing is enabled).
        void addComponentRequested(const QString& name);
        void removeComponentRequested(const QString& name);
        // By flecs id — emitted for rows that carry one (tags, pairs; mirror rows).
        void removeComponentIdRequested(qulonglong rawId);

    protected:
        // Reverts a pending delete-confirm when the list loses focus.
        bool eventFilter(QObject* obj, QEvent* ev) override;

    private slots:
        void _onSelectionChanged();
        void _onAddClicked();
        void _onContextMenu(const QPoint& pos);

    private:
        void _setupUi();
        void _applyFilter(); // hide list rows not matching the filter text

        QListWidget* _list = nullptr;
        QLineEdit* _filterEdit = nullptr;
        QToolButton* _addBtn = nullptr;
        QStyledItemDelegate* _rowDelegate = nullptr; // actually a RemoveButtonDelegate
        QVector<ComponentInfo> _components;
        QVector<MirrorChannel::ComponentRow> _mirrorRows; // last externally-fed set (dedup)
        QVector<MirrorChannel::CatalogEntry> _addable;    // "+" picker entries
        QVector<MenuAction> _contextActions;
        std::function<void(const QString&, qulonglong, QMenu&)> _menuHook;
        bool _editingEnabled = false;
        AccessGuard _guard;
    };

} // namespace rpe

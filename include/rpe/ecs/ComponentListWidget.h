#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QStringList>
#include <QVector>
#include <QWidget>

#include "rpe/core/AccessGuard.h"
#include "rpe/ecs/flecs_prelude.h"

class QListWidget;
class QToolButton;
class QStyledItemDelegate;

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
        // via componentNameSelected.
        void setComponentNames(const QStringList& names);

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
        // minus the components already on the entity).
        void setAddableComponents(const QStringList& names);

    signals:
        void componentSelected(ComponentInfo info);      // direct mode (world available)
        void componentNameSelected(const QString& name); // mirror mode
        void componentDeselected();

        // Structural-edit requests (only emitted when editing is enabled).
        void addComponentRequested(const QString& name);
        void removeComponentRequested(const QString& name);

    private slots:
        void _onSelectionChanged();
        void _onAddClicked();

    private:
        void _setupUi();

        QListWidget* _list = nullptr;
        QToolButton* _addBtn = nullptr;
        QStyledItemDelegate* _rowDelegate = nullptr; // actually a RemoveButtonDelegate
        QVector<ComponentInfo> _components;
        QStringList _mirrorNames;  // last externally-fed name set (dedup)
        QStringList _addable;      // names offered by the "+" picker
        bool _editingEnabled = false;
        AccessGuard _guard;
    };

} // namespace rpe

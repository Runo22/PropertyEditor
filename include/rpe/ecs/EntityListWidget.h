#pragma once

#include <QPair>
#include <QVector>
#include <QWidget>

#include "rpe/core/AccessGuard.h"
#include "rpe/ecs/flecs_prelude.h"

class QListWidget;
class QLineEdit;
class QCheckBox;
class QTimer;

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  EntityListWidget — lists named entities of a flecs::world.
    //
    //  Supports an optional "required component" filter: when set (e.g. a Transform
    //  component), the list shows only entities that have that component — this is
    //  the "list the ones with a transform" behaviour. Refreshes on a slow timer
    //  since the entity set rarely changes at frame rate.
    // ─────────────────────────────────────────────────────────────────────────────
    class EntityListWidget : public QWidget
    {
        Q_OBJECT

    public:
        explicit EntityListWidget(QWidget* parent = nullptr);

        void setWorld(flecs::world* world);
        void setRefreshIntervalMs(int ms);

        // Name of a component to filter by (flecs component name). Empty = no filter.
        // `enabledByDefault` checks the toggle so the filter is active immediately.
        void setRequiredComponent(const QString& componentName, bool enabledByDefault = true);

        // Guard wrapped around world reads when the world is owned by another
        // thread (see rpe/core/AccessGuard.h).
        void setWorldAccess(AccessGuard guard);

        // Stop the world-polling timer (mirror mode feeds the list via setEntries).
        void stopAutoRefresh();
        // Externally provided (id, label) entries — used by EcsMirror integration.
        void setEntries(const QVector<QPair<qulonglong, QString>>& entries);

    signals:
        void entitySelected(flecs::entity e); // direct mode (world available)
        void entityIdSelected(qulonglong id); // always; mirror mode uses this
        void entityDeselected();

    private slots:
        void _refresh();
        void _onSelectionChanged();

    private:
        void _setupUi();
        void _applyEntries(const QVector<QPair<qulonglong, QString>>& entries);

        flecs::world* _world = nullptr;
        QListWidget* _list = nullptr;
        QLineEdit* _filterEdit = nullptr;
        QCheckBox* _requiredCheck = nullptr;
        QTimer* _timer = nullptr;
        QString _requiredComponent;
        AccessGuard _guard;
        // Last visible (id, label) set — refresh skips the rebuild when unchanged.
        QVector<QPair<qulonglong, QString>> _lastEntries;
    };

} // namespace rpe

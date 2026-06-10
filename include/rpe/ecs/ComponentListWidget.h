#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QVector>
#include <QWidget>

#include "rpe/core/AccessGuard.h"
#include "rpe/ecs/flecs_prelude.h"

class QListWidget;

namespace rpe {

// Resolved component of an entity that has a matching RTTR type.
struct ComponentInfo {
    flecs::id  id;
    rttr::type rttrType = rttr::type::get<void>();   // rttr::type has no default ctor
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

signals:
    void componentSelected(ComponentInfo info);
    void componentDeselected();

private slots:
    void _onSelectionChanged();

private:
    void _setupUi();

    QListWidget*           _list = nullptr;
    QVector<ComponentInfo> _components;
    AccessGuard            _guard;
};

} // namespace rpe

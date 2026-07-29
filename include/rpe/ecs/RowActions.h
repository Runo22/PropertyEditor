#pragma once

#include <QIcon>
#include <QString>
#include <QtGlobal>

#include <functional>

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  Custom right-click menu entries the host can add to the browser's lists.
    //
    //  They sit alongside the built-in actions (Delete entity / Remove component) in
    //  the same context menu, styled the same, so a host can attach app-specific
    //  operations — "Focus camera on this entity", "Copy component", … — without
    //  subclassing anything. Register via EntityComponentBrowser::addEntityAction /
    //  addComponentAction. The callback runs on the GUI thread.
    // ─────────────────────────────────────────────────────────────────────────────

    struct EntityAction
    {
        QString label;
        QIcon icon; // optional
        std::function<void(qulonglong entityId)> callback;
    };

    struct ComponentAction
    {
        QString label;
        QIcon icon; // optional
        // component: the selection key (a data component's path, or a pair's
        // "Rel (Target)"); rawId: its flecs id (0 for legacy name-only rows).
        std::function<void(qulonglong entityId, const QString& component, qulonglong rawId)> callback;
    };

} // namespace rpe

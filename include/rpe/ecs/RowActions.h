#pragma once

#include <QIcon>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>

class QMenu;

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
    //
    //  `menu` files the entry under a submenu path, one element per level:
    //  { "Debug", "Physics" } → Debug ▸ Physics ▸ <label>. Entries sharing a path
    //  share the submenu — also with entries a menu hook adds through subMenu().
    //  It's a list, not a "Debug/Physics" string, so a label or level may contain
    //  any character ("I/O", "Save/Load").
    // ─────────────────────────────────────────────────────────────────────────────

    struct EntityAction
    {
        QString label;
        QIcon icon; // optional
        std::function<void(qulonglong entityId)> callback;
        QStringList menu; // optional submenu path; empty = top level
    };

    struct ComponentAction
    {
        QString label;
        QIcon icon; // optional
        // component: the selection key (a data component's path, or a pair's
        // "Rel (Target)"); rawId: its flecs id (0 for legacy name-only rows).
        std::function<void(qulonglong entityId, const QString& component, qulonglong rawId)> callback;
        QStringList menu; // optional submenu path; empty = top level
    };

    // The submenu at `path` under `root`, created on first use and REUSED after:
    // a level matches an existing submenu with the same title (ignoring '&'
    // mnemonics), so entries from static actions, several menu hooks or several
    // plugins land in one "Debug" submenu instead of three. Empty or blank levels
    // are skipped; an empty path returns `root`. For use in menu hooks:
    //
    //   browser.addEntityMenuHook([](qulonglong id, QMenu& menu) {
    //       rpe::subMenu(menu, { "Debug", "Physics" })->addAction("Wake", [id] { wake(id); });
    //   });
    QMenu* subMenu(QMenu& root, const QStringList& path);

} // namespace rpe

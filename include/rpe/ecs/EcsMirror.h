#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QHash>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <mutex>
#include <utility>
#include <vector>

#include "rpe/ecs/flecs_prelude.h"

namespace rpe {

// ─────────────────────────────────────────────────────────────────────────────
//  EcsMirror — thread separation between a flecs world (advanced by a simulation
//  thread) and the Qt GUI, WITHOUT locking the simulation loop.
//
//  attach() registers a once-per-frame flecs system, so all world access happens
//  *inside the caller's existing world.progress()* on the simulation thread —
//  you don't change your loop. Each frame the system:
//    • snapshots the named-entity list (optionally filtered by a component),
//    • enumerates the bridged components of the "interest" entity,
//    • reads the interest leaf values into self-contained value copies, and
//    • applies any edits the GUI queued.
//  The GUI thread only ever reads those copies (poll*) and pushes intent
//  (setInterest / queueEdit) — it never touches the world, so no data race.
//
//  Costs: one value-copy per watched leaf per frame (only the *open* fields the
//  GUI asks for), and ~1 frame of latency. See README threading section.
//
//  All public methods are safe to call from the GUI thread except pump(), which
//  the registered system calls on the simulation thread (you may also call it
//  yourself from the sim thread if you prefer not to register a system).
// ─────────────────────────────────────────────────────────────────────────────
class EcsMirror
{
public:
    struct EntityEntry {
        qulonglong id = 0;
        QString    label;
        bool operator==(const EntityEntry& o) const { return id == o.id && label == o.label; }
        bool operator!=(const EntityEntry& o) const { return !(*this == o); }
    };
    struct ValueUpdate {
        QString       path;
        rttr::variant value;
    };

    EcsMirror() = default;
    ~EcsMirror();

    EcsMirror(const EcsMirror&)            = delete;
    EcsMirror& operator=(const EcsMirror&) = delete;

    // ── simulation thread (or before progress() starts) ──────────────────────
    void attach(flecs::world* world);   // registers the per-frame system
    void detach();
    void pump();                        // one snapshot/apply cycle (sim thread)
    bool isAttached() const { return _world != nullptr; }

    // ── GUI thread: intent ───────────────────────────────────────────────────
    void setRequiredComponent(const QString& componentName);  // entity-list filter
    void setInterest(qulonglong entity, const QString& componentName,
                     const QStringList& leafPaths);           // what to mirror
    void clearInterest();
    void queueEdit(const QString& path, rttr::variant value); // applied next frame

    // ── GUI thread: results (poll on a timer) ────────────────────────────────
    bool pollEntities(QVector<EntityEntry>& out);   // true if changed since last poll
    bool pollComponents(QStringList& out);          // true if changed since last poll
    std::vector<ValueUpdate> pollValues();          // leaf values that changed

private:
    void _registerSystem();

    flecs::world*  _world = nullptr;
    flecs::system  _system{};
    bool           _haveSystem = false;

    // Shared state guarded by _m.
    mutable std::mutex _m;
    qulonglong   _inEntity = 0;
    QString      _inComponent;
    QStringList  _inPaths;
    QString      _required;
    std::vector<std::pair<QString, rttr::variant>> _edits;

    QVector<EntityEntry>     _outEntities;
    bool                     _outEntitiesDirty = false;
    QStringList              _outComponents;
    bool                     _outComponentsDirty = false;
    std::vector<ValueUpdate> _outValues;

    // Simulation-thread-only state (no lock needed).
    QVector<EntityEntry>   _lastEntities;
    QStringList            _lastComponents;
    QHash<QString, QString> _lastValueStr;   // path -> last display, for dedup
    qulonglong             _lastInterestEntity = 0;
    QString                _lastInterestComponent;
};

} // namespace rpe

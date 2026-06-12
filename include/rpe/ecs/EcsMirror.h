#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>

#include "rpe/ecs/flecs_prelude.h"
#include "rpe/ecs/MirrorChannel.h"

namespace rpe
{

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
        // The shared data channel carries these (re-exported for convenience).
        using EntityEntry = MirrorChannel::EntityEntry;
        using ValueUpdate = MirrorChannel::ValueUpdate;

        EcsMirror();
        ~EcsMirror();

        EcsMirror(const EcsMirror&) = delete;
        EcsMirror& operator=(const EcsMirror&) = delete;

        // ── simulation thread ─────────────────────────────────────────────────────
        // attach()/detach() MUST be called on the thread that runs world.progress()
        // (structural world changes are not thread-safe). attach() may be called
        // even from *inside* progress() — e.g. a system that loads a plugin at
        // runtime: when the world is readonly it auto-defers the install to
        // frame-end (ecs_run_post_frame), so it is always safe on the sim thread.
        void attach(flecs::world* world); // registers the per-frame system
        void detach();
        void pump(); // one snapshot/apply cycle (sim thread)
        bool isAttached() const
        {
            return _world != nullptr;
        }

        // ── GUI thread: intent ───────────────────────────────────────────────────
        void setRequiredComponent(const QString& componentName); // entity-list filter
        void setInterest(qulonglong entity, const QString& componentName,
                         const QStringList& leafPaths); // what to mirror
        void clearInterest();
        void queueEdit(const QString& path, rttr::variant value); // applied next frame

        // ── GUI thread: results (poll on a timer) ────────────────────────────────
        bool pollEntities(QVector<EntityEntry>& out); // true if changed since last poll
        bool pollComponents(QStringList& out);        // true if changed since last poll
        std::vector<ValueUpdate> pollValues();        // leaf values that changed

        // The shared channel. The GUI (EntityComponentBrowser) keeps its own
        // shared_ptr to this, so it stays valid even if this EcsMirror is
        // destroyed first (see MirrorChannel).
        std::shared_ptr<MirrorChannel> channel() const
        {
            return _ch;
        }

    private:
        void _install();
        static void _installTrampoline(ecs_world_t* world, void* ctx);
        static void _teardownTrampoline(ecs_world_t* world, void* ctx);

        std::shared_ptr<MirrorChannel> _ch; // shared with the GUI consumer

        // Liveness token shared with the system callback and any deferred install,
        // so they no-op safely if this EcsMirror is destroyed before they run.
        std::shared_ptr<std::atomic<bool>> _alive;

        flecs::world* _world = nullptr;
        flecs::system _system {};
        flecs::query<> _entityQuery {}; // cached: built once at attach, never in pump()
        bool _haveSystem = false;
        bool _haveQuery = false;

        // Simulation-thread-only state (no lock needed).
        QVector<EntityEntry> _lastEntities;
        QStringList _lastComponents;
        QHash<QString, QString> _lastValueStr; // path -> last display, for dedup
        qulonglong _lastInterestEntity = 0;
        QString _lastInterestComponent;
    };

} // namespace rpe

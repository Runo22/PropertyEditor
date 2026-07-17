#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_set>

#include "rpe/ecs/flecs_prelude.h"
#include "rpe/ecs/MirrorChannel.h"

namespace rpe
{

    // Shared control block coordinating the per-frame pump (runs on the simulation
    // thread, inside progress()) with teardown (may run on another thread). It
    // outlives the EcsMirror: the pump system captures a shared_ptr to it, so the
    // system left installed after the mirror is gone reads only valid memory and
    // simply no-ops.
    struct MirrorLiveToken
    {
        // Held for the whole duration of a pump AND while teardown marks the mirror
        // dead, so the two never overlap: an in-flight pump can't touch a
        // half-destroyed mirror, and teardown waits for any running pump to finish.
        std::mutex pumpMutex;
        // Cleared when the mirror is destroyed / detached: the pump then no-ops
        // (checked under pumpMutex) instead of dereferencing a dead `this`.
        std::atomic<bool> alive { true };
    };

    // ─────────────────────────────────────────────────────────────────────────────
    //  EcsMirror — thread separation between a flecs world (advanced by a simulation
    //  thread) and the Qt GUI, WITHOUT locking the simulation loop.
    //
    //  attach() registers a once-per-frame flecs system, so all world access happens
    //  *inside the caller's existing world.progress()* on the simulation thread —
    //  you don't change your loop. Each frame the system:
    //    • snapshots the entity list — every entity carrying at least one bridged
    //      component, labelled by name / prefab name / id (optionally filtered to
    //      those carrying a required component),
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
        using PinKey = MirrorChannel::PinKey;
        using PinValue = MirrorChannel::PinValue;

        EcsMirror();
        ~EcsMirror();

        EcsMirror(const EcsMirror&) = delete;
        EcsMirror& operator=(const EcsMirror&) = delete;

        // ── simulation thread ─────────────────────────────────────────────────────
        // attach() MUST be called on the thread that runs world.progress()
        // (structural world changes are not thread-safe). attach() may be called
        // even from *inside* progress() — e.g. a system that loads a plugin at
        // runtime: when the world is readonly it auto-defers the install to
        // frame-end (ecs_run_post_frame), so it is always safe on the sim thread.
        //
        // detach() and ~EcsMirror() are safe to call from ANY thread, in ANY world
        // state (even while the sim thread is mid-progress(), or after the world has
        // already been destroyed): they NEVER touch the world. They wait out any
        // in-flight pump, mark the mirror dead so the pump no-ops from then on, and
        // drop their flecs handles (a no-op on the world). The now-inert system is
        // left installed; it costs nothing per frame and is reaped at world fini (or
        // reused if you attach() again). This is what makes "the runtime owning the
        // world tears down before the editor owning the mirror" safe.
        // How the per-frame snapshot/apply is driven:
        enum class PumpMode
        {
            // Registers a once-per-frame flecs task (default; zero integration —
            // you don't change your loop). The task itself never touches the world
            // (no sync barrier under world.set_threads()); it defers the snapshot to
            // frame-end via ecs_run_post_frame, which runs on the sim thread after
            // the pipeline merged and the workers went idle — race-free, barrier-free.
            System,
            // No system is registered; YOU call pump() once per loop, right AFTER
            // world.progress() returns (world merged, workers idle). Equivalent
            // safety/cost to System — use it when you want explicit control of when
            // the snapshot runs.
            Manual,
        };
        // Only the UNDERLYING world must stay alive — the flecs::world wrapper you
        // pass may be a temporary (e.g. a stack wrapper around an engine-provided
        // ecs_world_t* in a plugin); the mirror stores the raw world pointer, never
        // the wrapper's address.
        void attach(flecs::world* world, PumpMode mode = PumpMode::System);
        void detach();
        void pump(); // one snapshot/apply cycle (sim thread); uses the bound world
        bool isAttached() const
        {
            return _world != nullptr;
        }

        // Cap how often the snapshot/apply actually runs, in wall-clock terms,
        // regardless of how fast the sim ticks (or how often you call pump()). The
        // GUI only needs ~30–60 Hz, so on a fast sim this avoids doing the work
        // thousands of times per second. 0 = run every time (default). Set e.g. 60.
        void setMaxPumpRateHz(double hz);

        // Lightweight pump diagnostics, updated on the simulation thread. Read them
        // from anywhere for logging (plain fields — a torn read is harmless for
        // diagnostics). Answers "is the rate cap active?" (skipped grows) and "how
        // expensive is a pump / an entity scan HERE?" (…Ms fields), which is the
        // data needed to tell mirror cost apart from everything else in a build.
        struct PumpStats
        {
            quint64 pumps = 0;          // pumps actually executed since attach()
            quint64 skipped = 0;        // pumps skipped by the rate cap since attach()
            double lastPumpMs = 0.0;    // duration of the most recent pump
            double maxPumpMs = 0.0;     // worst pump since attach()
            double lastScanMs = 0.0;    // duration of the most recent entity-list scan
            double maxScanMs = 0.0;     // worst entity-list scan since attach()
            double lastCatalogMs = 0.0; // duration of the most recent catalog scan
        };
        PumpStats pumpStats() const
        {
            return _stats;
        }

        // Wall-clock intervals for the two FULL-WORLD scans the pump performs on the
        // simulation thread: the entity list (every entity + its components) and the
        // add-component catalog (every component type). These are the expensive,
        // bursty parts of a pump — running them per-N-pumps (the old behaviour) tied
        // their cost to the frame rate and produced periodic frame-time spikes.
        // Defaults: entity list 500 ms, catalog 2000 ms. 0 = scan every pump.
        // Structural changes and filter changes force an immediate rescan regardless.
        void setScanIntervalsMs(int entityListMs, int catalogMs);

        // ── GUI thread: intent ───────────────────────────────────────────────────
        void setRequiredComponent(const QString& componentName); // entity-list filter
        void setInterest(qulonglong entity, const QString& componentName,
                         const QStringList& leafPaths); // what to mirror
        void clearInterest();
        void queueEdit(const QString& path, rttr::variant value); // applied next frame

        // Structural component edits. Queued here (GUI thread) and applied on the
        // simulation thread inside the per-frame system — structural world changes
        // are never safe from another thread. `component` is the flecs component
        // name as listed. Adding a component the entity already has, or removing one
        // it lacks, is a harmless no-op.
        void addComponent(qulonglong entity, const QString& component);
        void removeComponent(qulonglong entity, const QString& component);

        // ── GUI thread: pinned watches ───────────────────────────────────────────
        // Pins are mirrored EVERY pump, independent of the selected entity — the
        // backing of a watch widget that shows properties from several entities at
        // once. setPins replaces the whole set; queuePinEdit writes a pinned value
        // back on the sim thread (like queueEdit, but explicitly addressed).
        void setPins(const QVector<PinKey>& pins);
        void queuePinEdit(const PinKey& key, rttr::variant value);
        std::vector<PinValue> pollPinValues();

        // ── GUI thread: results (poll on a timer) ────────────────────────────────
        bool pollEntities(QVector<EntityEntry>& out); // true if changed since last poll
        bool pollComponents(QStringList& out);        // true if changed since last poll
        std::vector<ValueUpdate> pollValues();        // leaf values that changed
        bool pollCatalog(QStringList& out); // bridged component names available to add

        // The shared channel. The GUI (EntityComponentBrowser) keeps its own
        // shared_ptr to this, so it stays valid even if this EcsMirror is
        // destroyed first (see MirrorChannel).
        std::shared_ptr<MirrorChannel> channel() const
        {
            return _ch;
        }

    private:
        void _install();
        // Core of pump(). Always runs on the sim thread with the world merged and
        // the workers idle (frame-end trampoline, or manual pump() after progress()).
        void _pumpImpl(const flecs::world& world);
        static void _installTrampoline(ecs_world_t* world, void* ctx);
        // Frame-end pump (System mode): scheduled by the per-frame task via
        // ecs_run_post_frame; runs on the sim thread with the world merged.
        static void _pumpTrampoline(ecs_world_t* world, void* ctx);
        // Wall-clock rate limiter: true (and stamps the clock) if enough time has
        // passed since the last pump; false to skip this one. Sim-thread only.
        bool _rateAllows();

        std::shared_ptr<MirrorChannel> _ch; // shared with the GUI consumer

        // Liveness/synchronisation token shared with the system callback and any
        // deferred install, so they no-op safely if this EcsMirror is destroyed
        // before they run, and so an in-flight pump never overlaps teardown. A
        // fresh token is created per attach() (see attach()).
        std::shared_ptr<MirrorLiveToken> _alive;

        // The UNDERLYING world (ecs_world_t*), not the caller's flecs::world wrapper
        // object: the wrapper passed to attach() may be a temporary/stack object
        // (common in plugins that wrap an engine-provided pointer) and can die right
        // after attach() returns. The raw pointer stays valid for the world's whole
        // life; scoped flecs::world wrappers are built from it where needed.
        ecs_world_t* _world = nullptr;
        PumpMode _mode = PumpMode::System;
        // Minimum wall-clock gap between pumps, seconds (0 = every pump). Atomic:
        // setMaxPumpRateHz is called from the GUI thread, _rateAllows reads on the
        // sim thread. See setMaxPumpRateHz.
        std::atomic<double> _minPumpGapSec { 0.0 };
        std::chrono::steady_clock::time_point _lastPump {};
        PumpStats _stats; // sim-thread writes; see pumpStats()
        flecs::system _system {};
        flecs::query<> _entityQuery {};    // cached: built once at attach, never in pump()
        flecs::query<> _componentQuery {}; // cached: all components, for the bridged-id set
        // flecs ids of the components that resolve to a bridged RTTR type. Refreshed
        // during the (throttled) entity scan so the per-entity check is an O(1) hash
        // lookup instead of a resolveByName() registry scan per component.
        std::unordered_set<uint64_t> _bridgedIds;
        uint64_t _reqId = 0;          // resolved required-component id (with _bridgedIds)
        int _lastComponentCount = -1; // skip the bridged-id rebuild when unchanged...
        uint64_t _bridgeGen = 0;      // ...unless the TypeBridge registry changed
        bool _haveSystem = false;
        bool _haveQuery = false;

        // Simulation-thread-only state (no lock needed).
        QVector<EntityEntry> _lastEntities;
        QStringList _lastComponents;
        QHash<QString, QString> _lastValueStr; // path -> last display, for dedup
        qulonglong _lastInterestEntity = 0;
        QString _lastInterestComponent;
        QString _lastRequired;    // entity-list filter, to detect changes
        QStringList _lastCatalog; // last published add-component catalog (dedup)

        // Wall-clock throttles for the full-world scans (see setScanIntervalsMs).
        // Epoch-initialised so the FIRST pump after attach() always scans.
        std::chrono::duration<double> _entityScanGap { 0.5 };
        std::chrono::duration<double> _catalogScanGap { 2.0 };
        std::chrono::steady_clock::time_point _lastEntityScan {};
        std::chrono::steady_clock::time_point _lastCatalogScan {};

        // Selected-entity component list, rebuilt only when the entity or its
        // archetype (flecs table) changes — NOT every pump. Building it walks the
        // entity's components doing string allocation + a registry lookup each, so
        // per-pump rebuilds were a constant drag at high frame rates.
        qulonglong _compsEntity = 0;
        const void* _compsTable = nullptr; // opaque ecs_table_t*
        uint64_t _compsGen = 0;            // TypeBridge generation the list was built at
        QStringList _selComps;             // full scoped names, parallel to _selCompIds
        QVector<uint64_t> _selCompIds;

        // Pinned-watch state (sim thread). Component id + RTTR type are cached by
        // name (a findComponentEntity walk / registry lookup otherwise — per pump);
        // the display-string dedup is cleared whenever the pin set changes so new
        // pins publish immediately.
        struct PinResolve
        {
            uint64_t compId = 0;
            rttr::type rtype = rttr::type::get<void>();
        };
        QHash<QString, PinResolve> _pinRt;
        QHash<QString, QString> _lastPinStr; // "e|comp|path" -> last display
        QVector<MirrorChannel::PinKey> _lastPins;

        // Per-pump hot-path caches (sim thread): the selected component's RTTR type
        // (resolveByName takes a registry mutex + string normalisation) and the
        // watched paths pre-split (splitPath allocates per read otherwise). Both
        // refresh only when the corresponding intent field changes.
        QString _selTypeName;
        rttr::type _selType = rttr::type::get<void>();
        QStringList _lastPathList;
        std::vector<QStringList> _splitPaths;
    };

} // namespace rpe

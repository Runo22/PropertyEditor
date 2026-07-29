#include "rpe/ecs/EcsMirror.h"

#include "rpe/core/RttrBridge.h"
#include "rpe/core/TypeBridge.h"
#include "rpe/core/TypeRenderer.h"
#include "rpe/ecs/ComponentScan.h"

#include <algorithm>

namespace rpe
{

    namespace
    {
        // Heap context for the ecs_run_post_frame trampolines (deferred install and
        // the frame-end pump). Its ctx is a raw void* that the trampoline frees; the
        // shared token lets it no-op safely if the mirror died in the meantime.
        struct DeferredCtx
        {
            std::shared_ptr<MirrorLiveToken> alive;
            EcsMirror* self;
        };

        // Short (unscoped) form of a name: the segment after the last "::".
        QString shortName(const QString& s)
        {
            const int pos = s.lastIndexOf(QStringLiteral("::"));
            return pos >= 0 ? s.mid(pos + 2) : s;
        }

        // Some asset pipelines suffix prefab entity names with " Prefab". Trim a
        // trailing whitespace-delimited "prefab" (any case) for display, so the
        // add-entity picker and instance labels read as "Goblin", not "Goblin
        // Prefab". Conservative: only when preceded by whitespace (leaves
        // "MyPrefab"/"Prefab" alone) and never blanks the whole name.
        QString trimPrefabSuffix(QString s)
        {
            s = s.trimmed();
            static const QString suffix = QStringLiteral(" prefab");
            if (s.size() > suffix.size()
                && s.right(suffix.size()).compare(suffix, Qt::CaseInsensitive) == 0)
            {
                s.chop(suffix.size());
                s = s.trimmed();
            }
            return s;
        }

        // Display label for an entity: its name, else its prefab's name + id,
        // else just the id. (No leading id for named entities.)
        QString entityLabel(const flecs::entity& e)
        {
            const char* n = e.name();
            if (n && n[0] != '\0')
            {
                return QString::fromUtf8(n);
            }
            const flecs::entity prefab = e.target(flecs::IsA);
            if (prefab.is_valid())
            {
                const char* pn = prefab.name();
                if (pn && pn[0] != '\0')
                {
                    return QStringLiteral("%1  #%2").arg(trimPrefabSuffix(QString::fromUtf8(pn))).arg(e.id());
                }
            }
            return QStringLiteral("#%1").arg(e.id());
        }
    } // namespace

    EcsMirror::EcsMirror()
        : _ch(std::make_shared<MirrorChannel>())
        , _alive(std::make_shared<MirrorLiveToken>())
    {
    }

    EcsMirror::~EcsMirror()
    {
        // Mark dead under the pump lock: this blocks until any in-flight pump on the
        // sim thread finishes (so it never touches this soon-to-be-freed object),
        // and makes every later pump / deferred install no-op. Only then do we tear
        // down and free our state.
        {
            std::lock_guard<std::mutex> lk(_alive->pumpMutex);
            _alive->alive.store(false, std::memory_order_release);
        }
        detach();
        // Tell any GUI consumer still holding the channel that no more data is
        // coming; its shared_ptr keeps the channel alive, so its poll*() calls
        // stay valid (they just return nothing) instead of touching freed memory.
        _ch->markProducerGone();
    }

    void EcsMirror::attach(flecs::world* world, PumpMode mode)
    {
        // MUST be called on the simulation thread (the one that runs progress()).
        // Creating the system + query are structural changes; a flecs world is
        // not safe for structural changes from another thread, nor while it is in
        // readonly mode (mid-progress).
        //
        // If attach() is called from *inside* progress() — e.g. a system loads the
        // plugin at runtime — the world is readonly and installing now would
        // crash. In that case we defer the install to ecs_run_post_frame, which
        // runs at frame-end on this same thread, after readonly mode is lifted.
        //
        // PumpMode::Manual registers no system — you call pump() yourself after
        // world.progress(). PumpMode::System (default) registers a task that defers
        // the snapshot to frame-end; neither mode stalls the multi-threaded pipeline.
        detach();
        _mode = mode;
        // Keep only the underlying world pointer. The flecs::world WRAPPER the
        // caller passes may be a temporary (e.g. a stack wrapper around an
        // engine-provided ecs_world_t* in a plugin) and often dies right after
        // attach() returns — its address must never be stored or dereferenced later.
        _world = world ? world->c_ptr() : nullptr;
        if (!_world)
        {
            return;
        }
        // Fresh token for this binding. A new MirrorLiveToken means any system left
        // behind by a previous detach (which stays inert, keyed to the old token)
        // can never come back to life through this new attach. (The system entity is
        // reused — same name — so re-attach revives exactly one system per world.)
        _alive = std::make_shared<MirrorLiveToken>();
        // Fresh binding → scan immediately on the first pump and rebuild the
        // selected-entity component cache (its table pointer belongs to the old world).
        _lastEntityScan = {};
        _lastCatalogScan = {};
        _compsEntity = 0;
        _compsTable = nullptr;
        _selComps.clear();
        _selCompIds.clear();
        _selRows.clear();
        _lastCompRows.clear();
        _lastPrefabs.clear(); // prefab ids belong to the old world
        _pinRt.clear(); // component ids/types belong to the old world
        _lastPinStr.clear();
        _lastPins.clear();
        _selTypes.clear();
        _lastPathList.clear();
        _splitPaths.clear();
        _stats = {};
        _bridgedIds.clear(); // ids belong to the old world
        _reqId = 0;
        _lastComponentCount = -1;
        _scanActive = false; // cycle state belongs to the old world
        _scanIds.clear();
        _scanPos = 0;
        _scanStaging.clear();
        _scanVerdict.clear();
        _scanWorkMs = 0.0;
        ecs_world_t* w = _world;
        if (ecs_stage_is_readonly(w))
        {
            ecs_run_post_frame(w, &EcsMirror::_installTrampoline, new DeferredCtx { _alive, this });
        }
        else
        {
            _install();
        }
    }

    void EcsMirror::_installTrampoline(ecs_world_t*, void* ctx)
    {
        auto* c = static_cast<DeferredCtx*>(ctx);
        // alive==false → the EcsMirror was destroyed before frame-end; do not
        // touch it. Else, skip if it was detached (or already installed) meanwhile.
        if (c->alive->alive.load(std::memory_order_acquire) && c->self->_world && !c->self->_haveSystem)
        {
            c->self->_install();
        }
        delete c;
    }

    void EcsMirror::detach()
    {
        // Teardown NEVER touches the world — so it is safe from any thread, while the
        // sim thread is mid-progress(), and even after the world has been destroyed.
        //
        // Destructing the system here would be unsafe: it is a structural world
        // change (races a concurrent progress()) and would free the pump callback's
        // storage while it may be executing on the sim thread. Instead we just
        // *neutralise*: take the pump lock — which waits out any in-flight pump and
        // then blocks future ones — and mark the mirror dead. From then on the system
        // runs but no-ops (it checks `alive` under the same lock before touching
        // `this`), costing nothing per frame; the world reaps it at fini, or attach()
        // revives it (the system has a fixed name, so there is only ever one).
        if (_haveSystem || _haveQuery)
        {
            std::lock_guard<std::mutex> lk(_alive->pumpMutex);
            _alive->alive.store(false, std::memory_order_release);
        }
        // Drop our handles. The flecs C++ handle move-assignment only overwrites the
        // stored pointer — it never calls into the world — so this is safe even if
        // the world is already gone or owned by another (busy) thread.
        _haveQuery = false;
        _haveSystem = false;
        _entityQuery = flecs::query<>();
        _componentQuery = flecs::query<>();
        _bridgedIds.clear();
        _system = flecs::system();
        _world = nullptr;
    }

    void EcsMirror::_install()
    {
        // Scoped wrapper over the raw world pointer (claims/releases a poly ref —
        // balanced, never finalises a world the host still owns). Safe here: the
        // world is alive during attach()/the deferred install at frame-end.
        flecs::world w(_world);

        // Build the entity query ONCE here (never inside the readonly system). It
        // matches *all* entities (flecs::Any); pump() filters to those with a
        // bridged component (and the optional required filter), so the cached query
        // never needs rebuilding.
        _entityQuery = w.query_builder().with(flecs::Any).build();
        // Cached query over all component types, used to (re)build the bridged-id set.
        _componentQuery = w.query_builder().with<flecs::Component>().build();
        _haveQuery = true;

        // Manual mode: no system. The host calls pump() itself after progress().
        if (_mode == PumpMode::Manual)
        {
            return;
        }

        // A term-less system is a task: its run callback fires once per frame inside
        // world.progress(), on the simulation thread (flecs runs non-multi_threaded
        // ops on stage 0 only). The captured 'alive' token makes the callback a
        // no-op once this EcsMirror is detached/destroyed — the system is
        // intentionally left installed after teardown (it just no-ops) and is reaped
        // at world fini, so it never destructs itself mid-run.
        //
        // The callback does NOT touch the world (workers may be mutating components
        // concurrently — the task declares no terms, so flecs schedules it without a
        // sync point). Instead it schedules the actual snapshot via
        // ecs_run_post_frame, which ecs_frame_end executes on this same thread AFTER
        // the pipeline finished: world merged, readonly over, workers idle — so the
        // read is race-free WITHOUT the per-frame sync barrier that immediate() would
        // force. On an uncapped sim that barrier used to dominate (thousands of
        // worker syncs per second); this way the world never stalls for the mirror.
        auto token = _alive;
        _system = w.system("rpe::EcsMirror")
                      .kind(flecs::PostUpdate)
                      .run([this, token](flecs::iter& it) {
                          ecs_world_t* stage = it.world().c_ptr();
                          while (it.next())
                          { /* task: no tables to iterate */
                          }
                          // Rate-gate at schedule time (under the pump lock — this may
                          // be dead otherwise) so a skipped frame costs no allocation.
                          std::lock_guard<std::mutex> lk(token->pumpMutex);
                          if (!token->alive.load(std::memory_order_acquire) || !_rateAllows())
                          {
                              return;
                          }
                          ecs_run_post_frame(stage, &EcsMirror::_pumpTrampoline, new DeferredCtx { token, this });
                      });
        _haveSystem = true;
    }

    void EcsMirror::_pumpTrampoline(ecs_world_t* world, void* ctx)
    {
        // Runs inside ecs_frame_end on the sim thread: pipeline done, world merged,
        // readonly over, workers idle — the safe window to read components. `world`
        // is the live world pointer flecs hands the action — build a scoped wrapper
        // from it (never dereference any caller-owned wrapper object).
        auto* c = static_cast<DeferredCtx*>(ctx);
        {
            std::lock_guard<std::mutex> lk(c->alive->pumpMutex);
            if (c->alive->alive.load(std::memory_order_acquire) && c->self->_world)
            {
                flecs::world w(world);
                c->self->_pumpImpl(w);
            }
        }
        delete c;
    }

    // ── GUI thread: intent / results — delegate to the shared channel ───────────

    void EcsMirror::setRequiredComponent(const QString& componentName)
    {
        _ch->setRequiredComponent(componentName);
    }

    void EcsMirror::setInterest(qulonglong entity, const QString& componentName, const QStringList& leafPaths)
    {
        _ch->setInterest(entity, componentName, leafPaths);
    }

    void EcsMirror::clearInterest()
    {
        _ch->clearInterest();
    }

    void EcsMirror::queueEdit(const QString& path, rttr::variant value)
    {
        _ch->queueEdit(path, std::move(value));
    }

    void EcsMirror::addComponent(qulonglong entity, const QString& component)
    {
        _ch->queueStructural(MirrorChannel::StructuralKind::AddComponent, entity, component);
    }

    void EcsMirror::removeComponent(qulonglong entity, const QString& component)
    {
        _ch->queueStructural(MirrorChannel::StructuralKind::RemoveComponent, entity, component);
    }

    void EcsMirror::setSpawnConfigurator(std::function<void(flecs::entity)> fn)
    {
        std::lock_guard<std::mutex> lk(_spawnMutex);
        _spawnConfig = std::move(fn);
    }

    void EcsMirror::setPins(const QVector<PinKey>& pins)
    {
        _ch->setPins(pins);
    }

    void EcsMirror::queuePinEdit(const PinKey& key, rttr::variant value)
    {
        _ch->queuePinEdit(key, std::move(value));
    }

    std::vector<EcsMirror::PinValue> EcsMirror::pollPinValues()
    {
        return _ch->pollPinValues();
    }

    bool EcsMirror::pollEntities(QVector<EntityEntry>& out)
    {
        return _ch->pollEntities(out);
    }

    bool EcsMirror::pollComponents(QStringList& out)
    {
        return _ch->pollComponents(out);
    }

    bool EcsMirror::pollCatalog(QStringList& out)
    {
        return _ch->pollCatalog(out);
    }

    std::vector<EcsMirror::ValueUpdate> EcsMirror::pollValues()
    {
        return _ch->pollValues();
    }

    // ── simulation thread ───────────────────────────────────────────────────────

    void EcsMirror::setMaxPumpRateHz(double hz)
    {
        _minPumpGapSec.store(hz > 0.0 ? 1.0 / hz : 0.0, std::memory_order_relaxed);
    }

    void EcsMirror::setScanIntervalsMs(int entityListMs, int catalogMs)
    {
        _entityScanGap = std::chrono::duration<double>(entityListMs > 0 ? entityListMs / 1000.0 : 0.0);
        _catalogScanGap = std::chrono::duration<double>(catalogMs > 0 ? catalogMs / 1000.0 : 0.0);
    }

    bool EcsMirror::_rateAllows()
    {
        const double gap = _minPumpGapSec.load(std::memory_order_relaxed);
        if (gap <= 0.0)
        {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - _lastPump).count() < gap)
        {
            ++_stats.skipped;
            return false;
        }
        _lastPump = now;
        return true;
    }

    void EcsMirror::pump()
    {
        if (!_world)
        {
            return;
        }
        // Same synchronisation as the registered system: hold the pump lock so a
        // concurrent detach()/destructor on another thread waits this out and never
        // marks the mirror dead mid-pump. Call this once per loop, AFTER progress().
        std::lock_guard<std::mutex> lk(_alive->pumpMutex);
        if (!_alive->alive.load(std::memory_order_acquire) || !_rateAllows())
        {
            return;
        }
        flecs::world w(_world); // scoped wrapper; the caller guarantees the world is alive here
        _pumpImpl(w);
    }

    void EcsMirror::_pumpImpl(const flecs::world& world)
    {
        // Time the whole pump (RAII — covers every early return) for pumpStats().
        struct PumpTimer
        {
            PumpStats& s;
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            ~PumpTimer()
            {
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                ++s.pumps;
                s.lastPumpMs = ms;
                if (ms > s.maxPumpMs)
                {
                    s.maxPumpMs = ms;
                }
            }
        } pumpTimer { _stats };

        // Snapshot GUI intent from the shared channel.
        MirrorChannel::Intent in = _ch->takeIntent();
        const qulonglong entity = in.entity;
        const QString& component = in.component;
        const QString& required = in.required;
        const QStringList& paths = in.paths;
        auto& edits = in.edits;

        // ── Structural edits (add/remove components) ───────────────────────────────
        // Applied here on the simulation thread, where structural world changes are
        // safe. The system is immediate(), so the world is non-readonly at this
        // point. Adding/removing changes the entity's archetype, so we then force a
        // fresh scan/publish of the component list, entity list and catalog.
        bool structuralApplied = false;
        for (const MirrorChannel::StructuralEdit& s : in.structurals)
        {
            // Spawn: no target entity — instantiate a fresh one from the prefab, then
            // hand it to the host's configurator so it can set/override components
            // (setting AFTER is_a overrides the prefab's shared values). rawId is the
            // prefab id.
            if (s.kind == MirrorChannel::StructuralKind::SpawnPrefab)
            {
                const flecs::entity prefab = world.entity(static_cast<flecs::entity_t>(s.rawId));
                if (!prefab.is_alive())
                {
                    continue;
                }
                flecs::entity ne = world.entity().is_a(static_cast<flecs::entity_t>(s.rawId));
                // Give the instance a real NAME from the prefab (minus any " Prefab"
                // suffix), so it reads as "Zombie" rather than an anonymous id. flecs
                // aborts on a duplicate name in a scope, so uniquify with a numeric
                // suffix ("Zombie", "Zombie (1)", …). Set BEFORE the configurator so
                // the host can still override it.
                const char* pn = prefab.name();
                const QString base = pn ? trimPrefabSuffix(QString::fromUtf8(pn)) : QString();
                if (!base.isEmpty())
                {
                    std::string name = base.toStdString();
                    for (int i = 1; world.lookup(name.c_str()).is_valid(); ++i)
                    {
                        name = base.toStdString() + " (" + std::to_string(i) + ")";
                    }
                    ne.set_name(name.c_str());
                }
                std::function<void(flecs::entity)> cfg;
                {
                    std::lock_guard<std::mutex> lk(_spawnMutex);
                    cfg = _spawnConfig;
                }
                if (cfg)
                {
                    cfg(ne);
                }
                structuralApplied = true;
                continue;
            }

            flecs::entity e = world.entity(s.entity);
            if (!e.is_alive())
            {
                continue;
            }
            if (s.kind == MirrorChannel::StructuralKind::DestroyEntity)
            {
                e.destruct();
                structuralApplied = true;
                continue;
            }
            // By-id form: exact, and the ONLY way to address a pair. Also used for
            // tag rows so a leaf-name collision can never remove the wrong thing.
            if (s.rawId != 0)
            {
                if (s.kind == MirrorChannel::StructuralKind::AddComponent)
                {
                    e.add(static_cast<flecs::id_t>(s.rawId));
                }
                else
                {
                    e.remove(static_cast<flecs::id_t>(s.rawId));
                }
                structuralApplied = true;
            }
            else if (s.kind == MirrorChannel::StructuralKind::AddComponent)
            {
                // bridgedOnly=false: the add catalog legitimately offers zero-size
                // TAGS, which have no RTTR bridge (nothing to inspect, only add).
                flecs::entity comp = findComponentEntity(world, s.component, /*bridgedOnly=*/false);
                if (comp.is_valid())
                {
                    e.add(comp);
                    structuralApplied = true;
                }
            }
            else // RemoveComponent (by name)
            {
                // Collect matches FIRST, then remove: e.remove() moves the entity to a
                // new table, so removing inside e.each() (which iterates the current
                // table's type) would corrupt the walk and could drop the wrong id.
                std::vector<flecs::id_t> toRemove;
                e.each([&](flecs::id id) {
                    if (!id.is_entity())
                    {
                        return;
                    }
                    const char* cn = id.entity().name();
                    if (cn && s.component == QString::fromUtf8(cn))
                    {
                        toRemove.push_back(id.raw_id());
                    }
                });
                for (const flecs::id_t rid : toRemove)
                {
                    e.remove(rid);
                    structuralApplied = true;
                }
            }
        }
        if (structuralApplied)
        {
            _lastCompRows.clear();
            _lastEntities.clear();
            _lastCatalog.clear();
            _lastPrefabs.clear();
        }

        // The GUI reset its view (re-selected the same entity/component, or its
        // selection was cleared and restored) and wants the data resent. The GUI can
        // raise this at ~30 Hz, so it MUST be cheap: re-publish the already-cached
        // lists rather than forcing an expensive full re-scan (that feedback loop —
        // slow sim → resync every frame → full scan every frame — is what pins the
        // simulation at a few fps). The interest values are re-read below (cheap: a
        // handful of watched paths).
        if (in.resync)
        {
            _lastValueStr.clear();
            _ch->publishEntities(_lastEntities);
            _ch->publishComponentRows(_lastCompRows);
            _ch->publishCatalogEntries(_lastCatalog);
            _ch->publishPrefabs(_lastPrefabs);
        }

        // Interest changed → reset per-leaf dedup so the new selection refreshes fully.
        if (entity != _lastInterestEntity || component != _lastInterestComponent)
        {
            _lastValueStr.clear();
            _lastInterestEntity = entity;
            _lastInterestComponent = component;
        }

        // ── Entity list ──────────────────────────────────────────────────────────
        // Show every entity (named or not — labelled by name, else prefab name,
        // else id) that has at least one *bridged* component, so the inspector
        // lists exactly the entities it can show. Optionally filtered to those
        // carrying the required component (matched by short name). This scans all
        // entities, so it is throttled on WALL CLOCK (not pump count): a per-N-pumps
        // gate ties the scan rate to the frame rate — on a fast sim that produced a
        // full-world scan burst (and a frame-time spike) every fraction of a second.
        const auto scanNow = std::chrono::steady_clock::now();
        const bool requiredChanged = (required != _lastRequired);
        _lastRequired = required;
        const bool scanDue = requiredChanged || structuralApplied
            || (scanNow - _lastEntityScan >= _entityScanGap);

        // A filter/structural change invalidates an in-flight cycle: restart with
        // fresh parameters instead of finishing a stale pass.
        if ((requiredChanged || structuralApplied) && _scanActive)
        {
            _scanActive = false;
        }

        if (!_scanActive && scanDue && _haveQuery)
        {
            // ── Begin a scan cycle ────────────────────────────────────────────────
            const auto beginT0 = std::chrono::steady_clock::now();
            const QString reqShort = required.isEmpty() ? QString() : shortName(required);

            // Refresh the set of bridged component ids (and locate the required one).
            // String work (flecs path allocation + registry lookup) per component
            // TYPE — skipped entirely when nothing could have changed: the type
            // count only ever grows, and bridge registrations bump the registry
            // generation.
            const int compCount = _componentQuery.count();
            const uint64_t bridgeGen = TypeBridge::registryGeneration();
            if (compCount != _lastComponentCount || bridgeGen != _bridgeGen
                || requiredChanged || structuralApplied || _bridgedIds.empty())
            {
                _lastComponentCount = compCount;
                _bridgeGen = bridgeGen;
                _bridgedIds.clear();
                _reqId = 0;
                _componentQuery.each([&](flecs::entity comp) {
                    const char* cn = comp.name();
                    if (!cn || cn[0] == '\0')
                    {
                        return;
                    }
                    const flecs::string path = comp.path(".", "");
                    const char* pc = path.c_str();
                    if (!pc)
                    {
                        return; // no path → nothing to resolve (also avoids string_view(nullptr))
                    }
                    if (QString::fromUtf8(pc).startsWith(QStringLiteral("flecs")))
                    {
                        return; // skip flecs' own components
                    }
                    if (TypeBridge::resolveByName(pc).is_valid())
                    {
                        _bridgedIds.insert(comp.raw_id());
                    }
                    if (!reqShort.isEmpty() && shortName(QString::fromUtf8(cn)) == reqShort)
                    {
                        _reqId = comp.raw_id();
                    }
                });
            }

            // Narrow the entity query to the required component when the filter
            // changes: flecs then visits ONLY entities that have it, instead of every
            // entity in the world. Rebuilt only on change (cheap, rare).
            if (requiredChanged)
            {
                flecs::world w = world; // the (non-staged) world in immediate mode
                if (!reqShort.isEmpty() && _reqId)
                {
                    _entityQuery = w.query_builder().with(_reqId).build();
                }
                else if (reqShort.isEmpty())
                {
                    _entityQuery = w.query_builder().with(flecs::Any).build();
                }
            }

            // Fast id snapshot, TABLE-wise (no labels, no per-entity component
            // walks) — bounded by the table count, so it stays cheap even when the
            // labelling work below is spread over many pumps.
            _scanIds.clear();
            _scanPos = 0;
            _scanStaging.clear();
            _scanVerdict.clear();
            _scanReqShort = reqShort;
            constexpr size_t kMaxSnapshot = 1000000; // memory bound, far above any browsable set
            _entityQuery.run([&](flecs::iter& it) {
                while (it.next())
                {
                    for (size_t i = 0; i < it.count(); ++i)
                    {
                        if (_scanIds.size() >= kMaxSnapshot)
                        {
                            return;
                        }
                        _scanIds.push_back(it.entity(i).id());
                    }
                }
            });
            _scanActive = true;
            _scanWorkMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - beginT0).count();
        }

        if (_scanActive)
        {
            // ── One budgeted slice ────────────────────────────────────────────────
            // The bridged/required check is resolved per TABLE (all entities of a
            // table share their type), so the per-entity work is a hash lookup plus
            // the label build for actual matches.
            const double budget = _scanBudgetMs.load(std::memory_order_relaxed);
            const auto sliceT0 = std::chrono::steady_clock::now();
            constexpr int kMaxEntities = 5000; // larger lists aren't usefully browsable
            const uint64_t reqId = _reqId;
            const bool reqEmpty = _scanReqShort.isEmpty();
            size_t processed = 0;
            while (_scanPos < _scanIds.size())
            {
                if (_scanStaging.size() >= kMaxEntities)
                {
                    _scanPos = _scanIds.size();
                    break;
                }
                const flecs::entity ent = world.entity(_scanIds[_scanPos]);
                ++_scanPos;
                ++processed;
                if (ent.is_alive())
                {
                    const void* tbl = ecs_get_table(world.c_ptr(), ent.id());
                    quint8 v = 0;
                    const auto itv = _scanVerdict.constFind(tbl);
                    if (itv != _scanVerdict.constEnd())
                    {
                        v = itv.value();
                    }
                    else
                    {
                        const ecs_type_t* type = tbl
                            ? ecs_table_get_type(static_cast<const ecs_table_t*>(tbl))
                            : nullptr;
                        if (type)
                        {
                            for (int32_t k = 0; k < type->count; ++k)
                            {
                                const uint64_t rid = type->array[k];
                                if (_bridgedIds.count(rid))
                                {
                                    v |= 1;
                                }
                                if (reqId && rid == reqId)
                                {
                                    v |= 2;
                                }
                            }
                        }
                        _scanVerdict.insert(tbl, v);
                    }
                    if ((v & 1) && (reqEmpty || (v & 2)))
                    {
                        _scanStaging.append({ static_cast<qulonglong>(ent.id()), entityLabel(ent) });
                    }
                }
                // Budget check every 32 entities: guarantees forward progress even
                // with a microscopic budget, and keeps the clock reads rare.
                if ((processed & 31u) == 0 && budget > 0.0
                    && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sliceT0).count() >= budget)
                {
                    break;
                }
            }
            _scanWorkMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sliceT0).count();

            if (_scanPos >= _scanIds.size())
            {
                // ── Cycle complete: publish + reschedule ─────────────────────────
                if (_scanStaging != _lastEntities)
                {
                    _lastEntities = _scanStaging;
                    _ch->publishEntities(_scanStaging);
                }
                _stats.lastScanMs = _scanWorkMs;
                if (_scanWorkMs > _stats.maxScanMs)
                {
                    _stats.maxScanMs = _scanWorkMs;
                }
                _scanActive = false;
                _scanVerdict.clear();
                _lastEntityScan = std::chrono::steady_clock::now(); // next cycle after the gap
            }
        }

        // ── Add-component catalog ──────────────────────────────────────────────────
        // The set of bridged component names in the world, for the GUI's "add
        // component" picker. Scanning every component is cheap but not free, so it is
        // throttled like the entity scan (the catalog rarely changes).
        const bool scanCatalog = structuralApplied || (scanNow - _lastCatalogScan >= _catalogScanGap);
        if (scanCatalog)
        {
            _lastCatalogScan = scanNow;
            const auto catT0 = std::chrono::steady_clock::now();
            // Addable = bridged data components + zero-size TAGS (presence markers
            // need no bridge — there is nothing to inspect, only add/remove).
            QVector<MirrorChannel::CatalogEntry> catalog;
            for (const ComponentResolution& c : scanComponents(world))
            {
                if (c.bridged || c.tag)
                {
                    // Full scoped path so the GUI's add picker can group by namespace;
                    // findComponentEntity accepts either the path or the leaf name.
                    catalog.append({ c.path.isEmpty() ? c.name : c.path, c.tag });
                }
            }
            std::sort(catalog.begin(), catalog.end(),
                      [](const MirrorChannel::CatalogEntry& a, const MirrorChannel::CatalogEntry& b) {
                          return a.path < b.path;
                      });
            if (catalog != _lastCatalog)
            {
                _lastCatalog = catalog;
                _ch->publishCatalogEntries(catalog);
            }
            _stats.lastCatalogMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - catT0).count();

            // ── Spawnable prefabs (add-entity picker) ─────────────────────────────
            // On the same cadence: prefab entities, filtered by the required component
            // (reusing the entity-list filter), each filed under the first host group
            // tag it carries. Only prefabs with a name are offered (spawn needs a
            // handle; the id is the spawn key).
            std::vector<std::pair<QString, uint64_t>> groupTags; // (name, tag id)
            groupTags.reserve(static_cast<size_t>(in.prefabGroups.size()));
            for (const QString& g : in.prefabGroups)
            {
                // Group tags are arbitrary named entities (plain tags), not necessarily
                // components — look them up by name/path, not via the component query.
                const flecs::entity te = world.lookup(g.toUtf8().constData());
                if (te.is_valid())
                {
                    groupTags.emplace_back(g, te.raw_id());
                }
            }
            QVector<MirrorChannel::PrefabEntry> prefabs;
            flecs::query<> pq = world.query_builder().with(flecs::Prefab).build();
            pq.each([&](flecs::entity p) {
                if (_reqId != 0 && !p.has(_reqId))
                {
                    return;
                }
                const char* pn = p.name();
                if (!pn || pn[0] == '\0')
                {
                    return;
                }
                QString group;
                for (const auto& [gname, gid] : groupTags)
                {
                    if (p.has(gid))
                    {
                        group = gname;
                        break;
                    }
                }
                prefabs.append({ static_cast<qulonglong>(p.id()), trimPrefabSuffix(QString::fromUtf8(pn)), group });
            });
            std::sort(prefabs.begin(), prefabs.end(),
                      [](const MirrorChannel::PrefabEntry& a, const MirrorChannel::PrefabEntry& b) {
                          return a.group != b.group ? a.group < b.group : a.name < b.name;
                      });
            if (prefabs != _lastPrefabs)
            {
                _lastPrefabs = prefabs;
                _ch->publishPrefabs(prefabs);
            }
        }

        // ── Pinned watches (independent of the selection) ──────────────────────────
        // Apply queued pin edits, then mirror every pinned leaf — pins keep flowing
        // for entities/components that are NOT selected, feeding the watch widget.
        if (!in.pinEdits.empty() || !in.pins.isEmpty())
        {
            const auto dedupKey = [](const MirrorChannel::PinKey& k) {
                return QStringLiteral("%1|%2|%3").arg(k.entity).arg(k.component, k.path);
            };
            // Entity + component-pointer + type resolution for one pin. Component id
            // AND RTTR type are cached by name — resolveByName takes a registry
            // mutex + string work, far too heavy per pin per pump. A stale id
            // (dead/re-created component entity) re-resolves once.
            const auto resolvePin = [&](const MirrorChannel::PinKey& k, void*& ptrOut, rttr::type& tOut, uint64_t& cidOut) -> bool {
                flecs::entity pe = world.entity(k.entity);
                if (!pe.is_alive())
                {
                    return false;
                }
                // Id-addressed (data-carrying pair): resolve directly by the pair id +
                // ecs_get_typeid, exactly as the selected-component listing does. Pairs
                // have no flecs name, so findComponentEntity/resolveByName can't reach
                // them. NOTE: rttr::type::get<void>() reports is_valid()==true, so a
                // freshly-defaulted PinResolve.rtype (void) must NOT be treated as
                // resolved — gate on compId (0 = never resolved) and reject void.
                if (k.rawId != 0)
                {
                    PinResolve pr = _pinRt.value(k.component);
                    if (pr.compId == 0)
                    {
                        const ecs_entity_t tid = ecs_get_typeid(world.c_ptr(), k.rawId);
                        if (tid == 0)
                        {
                            return false;
                        }
                        const flecs::string tp = world.entity(tid).path(".", "");
                        pr.rtype = TypeBridge::resolveByName(tp.c_str() ? tp.c_str() : "");
                        pr.compId = k.rawId;
                        if (pr.rtype.is_valid() && pr.rtype != rttr::type::get<void>())
                        {
                            _pinRt.insert(k.component, pr);
                        }
                    }
                    tOut = pr.rtype;
                    cidOut = k.rawId;
                    if (!tOut.is_valid() || tOut == rttr::type::get<void>())
                    {
                        return false;
                    }
                    ptrOut = pe.get_mut(k.rawId);
                    return ptrOut != nullptr;
                }
                PinResolve pr = _pinRt.value(k.component);
                if (pr.compId == 0 || !world.entity(pr.compId).is_alive())
                {
                    const flecs::entity comp = findComponentEntity(world, k.component);
                    if (!comp.is_valid())
                    {
                        return false;
                    }
                    // Data components only — get_mut on a tag/plain-entity id asserts
                    // in debug flecs builds (see the selected-component listing).
                    const flecs::Component* cd = comp.try_get<flecs::Component>();
                    if (!cd || cd->size <= 0)
                    {
                        return false;
                    }
                    pr.compId = comp.raw_id();
                    pr.rtype = TypeBridge::resolveByName(k.component.toUtf8().constData());
                    _pinRt.insert(k.component, pr);
                }
                else if (!pr.rtype.is_valid())
                {
                    // Never cache an invalid type: the bridge registration may land
                    // after the pin was created (plugin load order) — retry.
                    pr.rtype = TypeBridge::resolveByName(k.component.toUtf8().constData());
                    if (pr.rtype.is_valid())
                    {
                        _pinRt.insert(k.component, pr);
                    }
                }
                tOut = pr.rtype;
                cidOut = pr.compId;
                if (!tOut.is_valid())
                {
                    return false;
                }
                ptrOut = pe.get_mut(pr.compId);
                return ptrOut != nullptr;
            };

            for (auto& [k, v] : in.pinEdits)
            {
                void* pp = nullptr;
                rttr::type pt = rttr::type::get<void>();
                uint64_t cid = 0;
                if (!resolvePin(k, pp, pt, cid))
                {
                    continue;
                }
                rttr::variant access = TypeBridge::wrap(pt, pp);
                if (!access.is_valid())
                {
                    continue;
                }
                rttr::instance pinst(access);
                if (bridge::setValueByPath(pinst, k.path, v))
                {
                    // Announce the write like a hand-written set<T>() would: OnSet
                    // observers and query change detection see inspector edits too.
                    ecs_modified_id(world.c_ptr(), k.entity, cid);
                }
                _lastPinStr.remove(dedupKey(k)); // force an echo of the applied value
            }

            // The pin set changed → clear the dedup so newly pinned leaves publish
            // immediately (and dropped ones stop occupying the cache).
            if (in.pins != _lastPins)
            {
                _lastPins = in.pins;
                _lastPinStr.clear();
            }

            std::vector<MirrorChannel::PinValue> pinUpdates;
            for (const MirrorChannel::PinKey& k : in.pins)
            {
                void* pp = nullptr;
                rttr::type pt = rttr::type::get<void>();
                uint64_t cid = 0;
                if (!resolvePin(k, pp, pt, cid))
                {
                    continue;
                }
                rttr::variant access = TypeBridge::wrap(pt, pp);
                if (!access.is_valid())
                {
                    continue;
                }
                rttr::instance pinst(access);
                rttr::variant val = bridge::getValueByPath(pinst, k.path);
                if (!val.is_valid())
                {
                    continue;
                }
                const QString kk = dedupKey(k);
                const QString s = TypeRenderer::toDisplayString(val);
                if (_lastPinStr.value(kk) == s)
                {
                    continue;
                }
                _lastPinStr.insert(kk, s);
                pinUpdates.push_back({ k, std::move(val) });
            }
            if (!pinUpdates.empty())
            {
                _ch->publishPinValues(std::move(pinUpdates));
            }
        }

        if (entity == 0)
        {
            return;
        }
        flecs::entity e = world.entity(entity);
        if (!e.is_alive())
        {
            return;
        }

        // ── Components of the interest entity + resolve the selected one ──────────
        // Rebuilt only when the entity or its ARCHETYPE (flecs table) changes —
        // add/remove component moves the entity to another table, so the table
        // pointer is a exact, O(1) change detector. Building the list walks every
        // component doing a string allocation + registry lookup each; doing that
        // every pump was a constant per-frame drag on an uncapped sim.
        // The list also depends on WHICH components are bridged, so a late bridge
        // registration (plugin load order) must rebuild it too — the registry
        // generation catches that; the table alone wouldn't change.
        const void* table = ecs_get_table(world.c_ptr(), e.id());
        const uint64_t compsGen = TypeBridge::registryGeneration();
        if (entity != _compsEntity || table != _compsTable || compsGen != _compsGen)
        {
            _compsEntity = entity;
            _compsTable = table;
            _compsGen = compsGen;
            _selComps.clear();
            _selCompIds.clear();
            _selTypes.clear();
            _selRows.clear();
            e.each([&](flecs::id id) {
                // PAIRS. Which side carries data is flecs' own rule (ecs_get_typeid:
                // the relation's type first, else the target's). A data-carrying
                // pair whose type is bridged becomes a SELECTABLE, editable row
                // ("Damage \u2192 Fire"); dataless pairs stay badge rows. flecs-internal
                // relations (ChildOf/IsA/Identifier…) stay hidden.
                if (id.is_pair())
                {
                    const flecs::entity rel = id.first();
                    const char* rn = rel.name();
                    if (!rn || rn[0] == '\0')
                    {
                        return;
                    }
                    const flecs::string rp = rel.path(".", "");
                    const QString relPath = rp.c_str() ? QString::fromUtf8(rp.c_str()) : QString();
                    if (relPath.startsWith(QStringLiteral("flecs")))
                    {
                        return;
                    }
                    const flecs::entity tgt = id.second();
                    const char* tn = tgt.name();
                    const QString target = (tn && tn[0] != '\0')
                        ? QString::fromUtf8(tn)
                        : QStringLiteral("#%1").arg(static_cast<qulonglong>(tgt.id()));

                    const ecs_entity_t tid = ecs_get_typeid(world.c_ptr(), id.raw_id());
                    if (tid != 0)
                    {
                        const flecs::entity typeEnt = world.entity(tid);
                        const flecs::string tp = typeEnt.path(".", "");
                        const QString typePath = tp.c_str() ? QString::fromUtf8(tp.c_str()) : QString();
                        const rttr::type rt = TypeBridge::resolveByName(typePath.toUtf8().constData());
                        if (rt.is_valid())
                        {
                            MirrorChannel::ComponentRow row { relPath, target,
                                                              MirrorChannel::RowKind::PairData,
                                                              static_cast<qulonglong>(id.raw_id()),
                                                              typePath };
                            _selComps.append(row.key());
                            _selCompIds.append(id.raw_id());
                            _selTypes.push_back(rt);
                            _selRows.append(row);
                            return;
                        }
                    }
                    _selRows.append({ relPath, target, MirrorChannel::RowKind::Pair,
                                      static_cast<qulonglong>(id.raw_id()) });
                    return;
                }
                if (!id.is_entity())
                {
                    return;
                }
                const char* n = id.entity().name();
                if (!n || n[0] == '\0')
                {
                    return;
                }
                const flecs::string p = id.entity().path(".", "");
                const QString qn = QString::fromUtf8(p.c_str());
                if (qn.startsWith(QStringLiteral("flecs")))
                {
                    return;
                }
                // Only DATA components are inspectable. A zero-size tag — or a plain
                // entity attached to this entity — has nothing to edit; it is shown
                // as a TAG row (get_mut on a dataless id ASSERTS in debug flecs).
                const flecs::Component* cd = id.entity().try_get<flecs::Component>();
                if (!cd || cd->size <= 0)
                {
                    _selRows.append({ qn, QString(), MirrorChannel::RowKind::Tag,
                                      static_cast<qulonglong>(id.raw_id()) });
                    return;
                }
                // Identify the component by its FULL scoped path ("game.Transform"),
                // which is unique even when two components share a leaf name. The GUI
                // displays the leaf; resolveByName matches the path exactly against
                // the RTTR full name.
                const rttr::type dt = TypeBridge::resolveByName(qn.toUtf8().constData());
                if (!dt.is_valid())
                {
                    return; // data component without a bridge → not inspectable, hidden
                }
                _selComps.append(qn);
                _selCompIds.append(id.raw_id());
                _selTypes.push_back(dt);
                _selRows.append({ qn, QString(), MirrorChannel::RowKind::Data,
                                  static_cast<qulonglong>(id.raw_id()) });
            });
        }
        if (_selRows != _lastCompRows)
        {
            _lastCompRows = _selRows;
            _ch->publishComponentRows(_selRows);
        }

        const int selIdx = _selComps.indexOf(component);
        if (selIdx < 0)
        {
            return;
        }
        // The row's RTTR type was resolved at listing rebuild (parallel array) —
        // for a data pair this is the type the PAIR carries, not the relation name.
        const rttr::type t = _selTypes[static_cast<size_t>(selIdx)];
        if (!t.is_valid())
        {
            return;
        }
        void* ptr = e.get_mut(_selCompIds[selIdx]);
        if (!ptr)
        {
            return;
        }

        rttr::variant access = TypeBridge::wrap(t, ptr); // variant holding T*
        if (!access.is_valid())
        {
            return;
        }
        rttr::instance inst(access);

        // Apply queued edits (GUI -> sim). The modified() announcement happens at
        // the END of this function: an OnSet observer may add/remove components on
        // `e`, which moves it to another table and DANGLES `ptr`/`inst` — so it must
        // come after our last read through them.
        bool wroteAny = false;
        for (auto& [p, v] : edits)
        {
            wroteAny = bridge::setValueByPath(inst, p, v) || wroteAny;
        }

        // Read watched leaves (sim -> GUI), de-duplicated by display string. The
        // paths are pre-split once per interest change — splitPath would otherwise
        // allocate per path per pump, which adds up on a fast (or debug) sim.
        if (paths != _lastPathList)
        {
            _lastPathList = paths;
            _splitPaths.clear();
            _splitPaths.reserve(static_cast<size_t>(paths.size()));
            for (const QString& p : paths)
            {
                _splitPaths.push_back(bridge::splitPath(p));
            }
        }
        std::vector<ValueUpdate> updates;
        for (int i = 0; i < paths.size(); ++i)
        {
            const QString& p = paths[i];
            rttr::variant val = bridge::getValueByPath(inst, _splitPaths[static_cast<size_t>(i)]);
            if (!val.is_valid())
            {
                continue;
            }
            const QString s = TypeRenderer::toDisplayString(val);
            if (_lastValueStr.value(p) == s)
            {
                continue;
            }
            _lastValueStr.insert(p, s);
            updates.push_back({ p, std::move(val) });
        }
        if (!updates.empty())
        {
            _ch->publishValues(std::move(updates));
        }

        // Announce the edits like a hand-written set<T>() — one modified() per
        // component per batch, so OnSet observers / query change detection see
        // inspector edits. LAST on purpose: observers may structurally change `e`
        // (archetype move), which invalidates the `ptr` the reads above used.
        if (wroteAny)
        {
            ecs_modified_id(world.c_ptr(), e.id(), _selCompIds[selIdx]);
        }
    }

} // namespace rpe

#include "rpe/ecs/EcsMirror.h"

#include "rpe/core/RttrBridge.h"
#include "rpe/core/TypeBridge.h"
#include "rpe/core/TypeRenderer.h"

namespace rpe
{

    namespace
    {
        // Heap context for the deferred-install trampoline (its ctx is a raw void*
        // that the trampoline frees).
        struct InstallCtx
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
                    return QStringLiteral("%1  #%2").arg(QString::fromUtf8(pn)).arg(e.id());
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

    void EcsMirror::attach(flecs::world* world)
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
        detach();
        _world = world;
        if (!_world)
        {
            return;
        }
        // Fresh token for this binding. A new MirrorLiveToken means any system left
        // behind by a previous detach (which stays inert, keyed to the old token)
        // can never come back to life through this new attach. (The system entity is
        // reused — same name — so re-attach revives exactly one system per world.)
        _alive = std::make_shared<MirrorLiveToken>();
        ecs_world_t* w = _world->c_ptr();
        if (ecs_stage_is_readonly(w))
        {
            ecs_run_post_frame(w, &EcsMirror::_installTrampoline, new InstallCtx { _alive, this });
        }
        else
        {
            _install();
        }
    }

    void EcsMirror::_installTrampoline(ecs_world_t*, void* ctx)
    {
        auto* c = static_cast<InstallCtx*>(ctx);
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
        _system = flecs::system();
        _world = nullptr;
    }

    void EcsMirror::_install()
    {
        // Build the entity query ONCE here (never inside the readonly system). It
        // matches *all* entities (flecs::Any); pump() filters to those with a
        // bridged component (and the optional required filter), so the cached query
        // never needs rebuilding.
        _entityQuery = _world->query_builder().with(flecs::Any).build();
        _haveQuery = true;

        // A term-less system is a task: its run callback fires once per frame inside
        // world.progress(), on the simulation thread. The captured 'alive' token
        // makes the callback a no-op once this EcsMirror is detached/destroyed — the
        // system is intentionally left installed after teardown (it just no-ops) and
        // is reaped at world fini, so it never destructs itself mid-run.
        //
        // immediate(): the system declares no components, so under multi-threaded
        // progress (world.set_threads) flecs would otherwise run it CONCURRENTLY
        // with the systems that mutate components (it thinks the task touches
        // nothing) — pump() would then read components while workers write them.
        // Running immediate forces a sync point and runs the task single-threaded
        // on a merged, consistent world, so reads are race-free.
        auto token = _alive;
        _system = _world->system("rpe::EcsMirror")
                      .kind(flecs::PostUpdate)
                      .immediate()
                      .run([this, token](flecs::iter& it) {
                          // In immediate mode it.world() is the real (non-staged)
                          // world; entity ops are valid and consistent here.
                          flecs::world stage = it.world();
                          while (it.next())
                          { /* task: no tables to iterate */
                          }
                          // Hold the pump lock across the whole snapshot/apply: this
                          // is what makes teardown safe. detach()/~EcsMirror take the
                          // same lock before marking the mirror dead, so a pump in
                          // flight finishes first and a later pump sees alive==false
                          // and bails before touching the dead `this`.
                          std::lock_guard<std::mutex> lk(token->pumpMutex);
                          if (!token->alive.load(std::memory_order_acquire))
                          {
                              return;
                          }
                          _pumpImpl(stage);
                      });
        _haveSystem = true;
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

    bool EcsMirror::pollEntities(QVector<EntityEntry>& out)
    {
        return _ch->pollEntities(out);
    }

    bool EcsMirror::pollComponents(QStringList& out)
    {
        return _ch->pollComponents(out);
    }

    std::vector<EcsMirror::ValueUpdate> EcsMirror::pollValues()
    {
        return _ch->pollValues();
    }

    // ── simulation thread ───────────────────────────────────────────────────────

    void EcsMirror::pump()
    {
        if (!_world)
        {
            return;
        }
        // Same synchronisation as the registered system: hold the pump lock so a
        // concurrent detach()/destructor on another thread waits this out and never
        // marks the mirror dead mid-pump.
        std::lock_guard<std::mutex> lk(_alive->pumpMutex);
        if (!_alive->alive.load(std::memory_order_acquire))
        {
            return;
        }
        _pumpImpl(*_world);
    }

    void EcsMirror::_pumpImpl(const flecs::world& world)
    {
        // Snapshot GUI intent from the shared channel.
        MirrorChannel::Intent in = _ch->takeIntent();
        const qulonglong entity = in.entity;
        const QString& component = in.component;
        const QString& required = in.required;
        const QStringList& paths = in.paths;
        auto& edits = in.edits;

        // The GUI reset its view (re-selected the same entity/component, or its
        // selection was cleared and restored). We dedup publishes against what we
        // last sent, so without dropping those caches we would skip resending
        // byte-identical data and the reset view would stay empty. Clear them so
        // this pump resends the entity list, the component list and the values.
        if (in.resync)
        {
            _lastValueStr.clear();
            _lastComponents.clear();
            _lastEntities.clear();
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
        // entities, so it is throttled — the visible set rarely changes.
        const bool requiredChanged = (required != _lastRequired);
        _lastRequired = required;
        const bool scanEntities = requiredChanged || in.resync || (_entityScanTick++ % 6 == 0);
        if (scanEntities && _haveQuery)
        {
            const QString reqShort = required.isEmpty() ? QString() : shortName(required);
            QVector<EntityEntry> ents;
            _entityQuery.each([&](flecs::entity ent) {
                if (!ent.is_alive())
                {
                    return;
                }
                bool hasBridged = false;
                bool hasReq = reqShort.isEmpty();
                ent.each([&](flecs::id id) {
                    if (!id.is_entity())
                    {
                        return;
                    }
                    const char* cn = id.entity().name();
                    if (!cn || cn[0] == '\0')
                    {
                        return;
                    }
                    if (TypeBridge::resolveByName(cn).is_valid())
                    {
                        hasBridged = true;
                    }
                    if (!reqShort.isEmpty() && shortName(QString::fromUtf8(cn)) == reqShort)
                    {
                        hasReq = true;
                    }
                });
                if (hasBridged && hasReq)
                {
                    ents.append({ static_cast<qulonglong>(ent.id()), entityLabel(ent) });
                }
            });
            if (ents != _lastEntities)
            {
                _lastEntities = ents;
                _ch->publishEntities(ents);
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
        QStringList comps;
        flecs::id selId;
        bool haveSel = false;
        e.each([&](flecs::id id) {
            if (!id.is_entity())
            {
                return;
            }
            const char* n = id.entity().name();
            if (!n || n[0] == '\0')
            {
                return;
            }
            if (!TypeBridge::resolveByName(n).is_valid())
            {
                return;
            }
            const QString qn = QString::fromUtf8(n);
            comps.append(qn);
            if (qn == component)
            {
                selId = id;
                haveSel = true;
            }
        });
        if (comps != _lastComponents)
        {
            _lastComponents = comps;
            _ch->publishComponents(comps);
        }

        if (!haveSel)
        {
            return;
        }
        const rttr::type t = TypeBridge::resolveByName(component.toUtf8().constData());
        if (!t.is_valid())
        {
            return;
        }
        void* ptr = e.get_mut(selId.raw_id());
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

        // Apply queued edits (GUI -> sim).
        for (auto& [p, v] : edits)
        {
            bridge::setValueByPath(inst, p, v);
        }

        // Read watched leaves (sim -> GUI), de-duplicated by display string.
        std::vector<ValueUpdate> updates;
        for (const QString& p : paths)
        {
            rttr::variant val = bridge::getValueByPath(inst, p);
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
    }

} // namespace rpe

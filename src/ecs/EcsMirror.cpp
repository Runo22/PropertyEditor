#include "rpe/ecs/EcsMirror.h"

#include "rpe/core/RttrBridge.h"
#include "rpe/core/TypeBridge.h"
#include "rpe/core/TypeRenderer.h"

namespace rpe
{

    namespace
    {
        // Heap contexts for ecs_run_post_frame trampolines (their ctx is a raw
        // void* that the trampoline frees).
        struct InstallCtx
        {
            std::shared_ptr<std::atomic<bool>> alive;
            EcsMirror* self;
        };
        struct TeardownCtx
        {
            flecs::system sys;
            flecs::query<> query; // ref-counted handle; finalised when this ctx dies
            bool haveSystem;
        };
        // Heap context for the ecs_atfini hook. Holds a share of the mirror's
        // _worldAlive flag so it can outlive the mirror (the world may be destroyed
        // long after) and clear it when the world finally dies.
        struct WorldFiniCtx
        {
            std::shared_ptr<std::atomic<bool>> worldAlive;
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
        , _alive(std::make_shared<std::atomic<bool>>(true))
    {
    }

    EcsMirror::~EcsMirror()
    {
        // Mark dead first: a deferred install or a late system run will then no-op
        // instead of touching this (soon-to-be-freed) object.
        _alive->store(false, std::memory_order_release);
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
        // Fresh liveness flag for this binding; the atfini hook installed below
        // clears it if the world dies before us.
        _worldAlive = std::make_shared<std::atomic<bool>>(true);
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
        if (c->alive->load(std::memory_order_acquire) && c->self->_world && !c->self->_haveSystem)
        {
            c->self->_install();
        }
        delete c;
    }

    void EcsMirror::detach()
    {
        // If the world was already destroyed (atfini cleared _worldAlive), flecs has
        // deleted our system + query for us — touching them, the world pointer, or
        // even reading readonly state would dereference freed memory. Just drop our
        // (now dangling) handles. Resetting them below is safe: the flecs C++ handle
        // move-assignment only overwrites the pointer, it never calls into the world.
        const bool worldAlive = _worldAlive && _worldAlive->load(std::memory_order_acquire);

        // Structural teardown. If called while the world is readonly (mid-progress
        // — e.g. a system removes the plugin), defer it to frame-end via value
        // handles, so it stays safe even if this object is destroyed in between.
        if (worldAlive && (_haveSystem || _haveQuery))
        {
            ecs_world_t* w = _world ? _world->c_ptr() : nullptr;
            if (w && ecs_stage_is_readonly(w))
            {
                ecs_run_post_frame(w, &EcsMirror::_teardownTrampoline, new TeardownCtx { _system, _entityQuery, _haveSystem });
            }
            else if (_haveSystem && _system.is_alive())
            {
                _system.destruct(); // delete the system entity (safe outside readonly)
            }
            // The anonymous _entityQuery is ref-counted: resetting the handle below
            // releases our reference (it is finalised when the last ref drops — the
            // TeardownCtx copy in the deferred case, or here directly). We must NOT
            // call destruct() on it (that asserts; it is only for entity queries).
        }
        _haveQuery = false;
        _haveSystem = false;
        _entityQuery = flecs::query<>();
        _system = flecs::system();
        _world = nullptr;
    }

    void EcsMirror::_worldFiniTrampoline(ecs_world_t*, void* ctx)
    {
        auto* c = static_cast<WorldFiniCtx*>(ctx);
        // The world is being destroyed: signal any still-living mirror to skip its
        // flecs teardown. Safe even if the mirror is already gone — worldAlive is a
        // shared_ptr this context co-owns.
        if (c->worldAlive)
        {
            c->worldAlive->store(false, std::memory_order_release);
        }
        delete c;
    }

    void EcsMirror::_teardownTrampoline(ecs_world_t*, void* ctx)
    {
        auto* c = static_cast<TeardownCtx*>(ctx);
        if (c->haveSystem && c->sys.is_alive())
        {
            c->sys.destruct();
        }
        delete c; // c->query handle dtor finalises the anonymous query (outside readonly)
    }

    void EcsMirror::_install()
    {
        // Build the named-entity query ONCE here (never inside the readonly
        // system). It matches *all* entities (flecs::Any); pump() filters to those
        // with a bridged component (and the optional required filter), so the
        // cached query never needs rebuilding.
        _entityQuery = _world->query_builder().with(flecs::Any).build();
        _haveQuery = true;

        // Learn when the world is destroyed. If the runtime that owns the world
        // tears down before the editor that owns this mirror, this hook clears
        // _worldAlive so our detach()/destructor skip flecs teardown (the world
        // has already deleted our system + query) instead of crashing on freed
        // memory. The context carries a share of _worldAlive, so it stays valid
        // even if this EcsMirror is long gone when the world finally dies.
        ecs_atfini(_world->c_ptr(), &EcsMirror::_worldFiniTrampoline, new WorldFiniCtx { _worldAlive });

        // A term-less system is a task: its run callback fires once per frame inside
        // world.progress(), on the simulation thread. The captured 'alive' token
        // makes the callback a no-op if this EcsMirror has been destroyed (the
        // system may outlive it briefly until a deferred teardown removes it).
        //
        // immediate(): the system declares no components, so under multi-threaded
        // progress (world.set_threads) flecs would otherwise run it CONCURRENTLY
        // with the systems that mutate components (it thinks the task touches
        // nothing) — pump() would then read components while workers write them.
        // Running immediate forces a sync point and runs the task single-threaded
        // on a merged, consistent world, so reads are race-free.
        auto alive = _alive;
        _system = _world->system("rpe::EcsMirror")
                      .kind(flecs::PostUpdate)
                      .immediate()
                      .run([this, alive](flecs::iter& it) {
                          // In immediate mode it.world() is the real (non-staged)
                          // world; entity ops are valid and consistent here.
                          flecs::world stage = it.world();
                          while (it.next())
                          { /* task: no tables to iterate */
                          }
                          if (!alive->load(std::memory_order_acquire))
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
        if (_world)
        {
            _pumpImpl(*_world);
        }
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
        const bool scanEntities = requiredChanged || (_entityScanTick++ % 6 == 0);
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

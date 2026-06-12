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
        // Structural teardown. If called while the world is readonly (mid-progress
        // — e.g. a system removes the plugin), defer it to frame-end via value
        // handles, so it stays safe even if this object is destroyed in between.
        if (_haveSystem || _haveQuery)
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
        // system). The optional required-component filter is applied per-frame in
        // the callback via has(), so the cached query never needs rebuilding.
        _entityQuery = _world->query_builder().with<flecs::Identifier>(flecs::Name).build();
        _haveQuery = true;

        // A term-less system is a task: its run callback fires once per frame inside
        // world.progress(), on the simulation thread. The captured 'alive' token
        // makes the callback a no-op if this EcsMirror has been destroyed (the
        // system may outlive it briefly until a deferred teardown removes it).
        auto alive = _alive;
        _system = _world->system("rpe::EcsMirror")
                      .kind(flecs::PostUpdate)
                      .run([this, alive](flecs::iter& it) {
                          while (it.next())
                          { /* task: no tables to iterate */
                          }
                          if (!alive->load(std::memory_order_acquire))
                          {
                              return;
                          }
                          pump();
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

        // ── Entity list (named entities, optionally filtered by component) ────────
        // Iterate the CACHED query (never create one here — we're inside the
        // readonly system). The required-component filter is applied via has().
        QVector<EntityEntry> ents;
        if (_haveQuery)
        {
            flecs::entity_t reqId = 0;
            if (!required.isEmpty())
            {
                flecs::entity rc = _world->lookup(required.toUtf8().constData());
                if (rc.is_valid())
                {
                    reqId = rc.id();
                }
            }
            _entityQuery.each([&](flecs::entity e) {
                if (!e.is_alive())
                {
                    return;
                }
                if (reqId && !e.has(reqId))
                {
                    return;
                }
                const char* n = e.name();
                const QString name = n ? QString::fromUtf8(n) : QStringLiteral("(unnamed)");
                ents.append({ static_cast<qulonglong>(e.id()),
                              QStringLiteral("%1  %2").arg(e.id()).arg(name) });
            });
        }
        if (ents != _lastEntities)
        {
            _lastEntities = ents;
            _ch->publishEntities(ents);
        }

        if (entity == 0)
        {
            return;
        }
        flecs::entity e = _world->entity(entity);
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
            flecs::entity ce = id.entity();
            const char* n = ce.name();
            if (!n || n[0] == '\0')
            {
                return;
            }
            const rttr::type t = rttr::type::get_by_name(n);
            if (!t.is_valid() || !TypeBridge::has(t))
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
        const rttr::type t = rttr::type::get_by_name(component.toStdString());
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

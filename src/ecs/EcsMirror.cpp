#include "rpe/ecs/EcsMirror.h"

#include "rpe/core/RttrBridge.h"
#include "rpe/core/TypeBridge.h"
#include "rpe/core/TypeRenderer.h"

namespace rpe {

EcsMirror::~EcsMirror()
{
    detach();
}

void EcsMirror::attach(flecs::world* world)
{
    detach();
    _world = world;
    if (_world)
        _registerSystem();
}

void EcsMirror::detach()
{
    if (_haveSystem && _system.is_alive())
        _system.destruct();
    _haveSystem = false;
    _system     = flecs::system();
    _world      = nullptr;
}

void EcsMirror::_registerSystem()
{
    // A term-less system is a task: its run callback fires once per frame inside
    // world.progress(), on the simulation thread. No change to the caller's loop.
    _system = _world->system("rpe::EcsMirror")
                  .kind(flecs::PostUpdate)
                  .run([this](flecs::iter& it) {
                      while (it.next()) { /* task: no tables to iterate */ }
                      pump();
                  });
    _haveSystem = true;
}

// ── GUI thread: intent ──────────────────────────────────────────────────────

void EcsMirror::setRequiredComponent(const QString& componentName)
{
    std::lock_guard<std::mutex> lk(_m);
    _required = componentName;
}

void EcsMirror::setInterest(qulonglong entity, const QString& componentName,
                            const QStringList& leafPaths)
{
    std::lock_guard<std::mutex> lk(_m);
    _inEntity    = entity;
    _inComponent = componentName;
    _inPaths     = leafPaths;
}

void EcsMirror::clearInterest()
{
    std::lock_guard<std::mutex> lk(_m);
    _inEntity = 0;
    _inComponent.clear();
    _inPaths.clear();
}

void EcsMirror::queueEdit(const QString& path, rttr::variant value)
{
    std::lock_guard<std::mutex> lk(_m);
    _edits.emplace_back(path, std::move(value));
}

// ── GUI thread: results ─────────────────────────────────────────────────────

bool EcsMirror::pollEntities(QVector<EntityEntry>& out)
{
    std::lock_guard<std::mutex> lk(_m);
    if (!_outEntitiesDirty) return false;
    out = _outEntities;
    _outEntitiesDirty = false;
    return true;
}

bool EcsMirror::pollComponents(QStringList& out)
{
    std::lock_guard<std::mutex> lk(_m);
    if (!_outComponentsDirty) return false;
    out = _outComponents;
    _outComponentsDirty = false;
    return true;
}

std::vector<EcsMirror::ValueUpdate> EcsMirror::pollValues()
{
    std::lock_guard<std::mutex> lk(_m);
    std::vector<ValueUpdate> v;
    v.swap(_outValues);
    return v;
}

// ── simulation thread ───────────────────────────────────────────────────────

void EcsMirror::pump()
{
    if (!_world) return;

    // Snapshot GUI intent.
    qulonglong   entity;
    QString      component, required;
    QStringList  paths;
    std::vector<std::pair<QString, rttr::variant>> edits;
    {
        std::lock_guard<std::mutex> lk(_m);
        entity    = _inEntity;
        component = _inComponent;
        required  = _required;
        paths     = _inPaths;
        edits.swap(_edits);
    }

    // Interest changed → reset per-leaf dedup so the new selection refreshes fully.
    if (entity != _lastInterestEntity || component != _lastInterestComponent) {
        _lastValueStr.clear();
        _lastInterestEntity    = entity;
        _lastInterestComponent = component;
    }

    // ── Entity list (named entities, optionally filtered by component) ────────
    QVector<EntityEntry> ents;
    {
        auto qb = _world->query_builder().with<flecs::Identifier>(flecs::Name);
        if (!required.isEmpty()) {
            flecs::entity rc = _world->lookup(required.toUtf8().constData());
            if (rc.is_valid()) qb.with(rc);
        }
        flecs::query<> q = qb.build();
        q.each([&](flecs::entity e) {
            if (!e.is_alive()) return;
            const char* n = e.name();
            const QString name = n ? QString::fromUtf8(n) : QStringLiteral("(unnamed)");
            ents.append({ static_cast<qulonglong>(e.id()),
                          QStringLiteral("%1  %2").arg(e.id()).arg(name) });
        });
    }
    if (ents != _lastEntities) {
        _lastEntities = ents;
        std::lock_guard<std::mutex> lk(_m);
        _outEntities      = ents;
        _outEntitiesDirty = true;
    }

    if (entity == 0) return;
    flecs::entity e = _world->entity(entity);
    if (!e.is_alive()) return;

    // ── Components of the interest entity + resolve the selected one ──────────
    QStringList comps;
    flecs::id   selId;
    bool        haveSel = false;
    e.each([&](flecs::id id) {
        if (!id.is_entity()) return;
        flecs::entity ce = id.entity();
        const char* n = ce.name();
        if (!n || n[0] == '\0') return;
        const rttr::type t = rttr::type::get_by_name(n);
        if (!t.is_valid() || !TypeBridge::has(t)) return;
        const QString qn = QString::fromUtf8(n);
        comps.append(qn);
        if (qn == component) { selId = id; haveSel = true; }
    });
    if (comps != _lastComponents) {
        _lastComponents = comps;
        std::lock_guard<std::mutex> lk(_m);
        _outComponents      = comps;
        _outComponentsDirty = true;
    }

    if (!haveSel) return;
    const rttr::type t = rttr::type::get_by_name(component.toStdString());
    if (!t.is_valid()) return;
    void* ptr = e.get_mut(selId.raw_id());
    if (!ptr) return;

    rttr::variant  access = TypeBridge::wrap(t, ptr);   // variant holding T*
    if (!access.is_valid()) return;
    rttr::instance inst(access);

    // Apply queued edits (GUI -> sim).
    for (auto& [p, v] : edits)
        bridge::setValueByPath(inst, p, v);

    // Read watched leaves (sim -> GUI), de-duplicated by display string.
    std::vector<ValueUpdate> updates;
    for (const QString& p : paths) {
        rttr::variant val = bridge::getValueByPath(inst, p);
        if (!val.is_valid()) continue;
        const QString s = TypeRenderer::toDisplayString(val);
        if (_lastValueStr.value(p) == s) continue;
        _lastValueStr.insert(p, s);
        updates.push_back({ p, std::move(val) });
    }
    if (!updates.empty()) {
        std::lock_guard<std::mutex> lk(_m);
        for (auto& u : updates)
            _outValues.push_back(std::move(u));
    }
}

} // namespace rpe

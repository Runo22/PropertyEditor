#include "rpe/ecs/MirrorChannel.h"

namespace rpe
{

    // ── GUI thread: intent ──────────────────────────────────────────────────────

    void MirrorChannel::setRequiredComponent(const QString& componentName)
    {
        std::lock_guard<std::mutex> lk(_m);
        _required = componentName;
    }

    void MirrorChannel::setInterest(qulonglong entity, const QString& componentName, const QStringList& leafPaths)
    {
        std::lock_guard<std::mutex> lk(_m);
        _inEntity = entity;
        _inComponent = componentName;
        _inPaths = leafPaths;
    }

    void MirrorChannel::clearInterest()
    {
        std::lock_guard<std::mutex> lk(_m);
        _inEntity = 0;
        _inComponent.clear();
        _inPaths.clear();
    }

    void MirrorChannel::queueEdit(const QString& path, rttr::variant value)
    {
        std::lock_guard<std::mutex> lk(_m);
        _edits.emplace_back(path, std::move(value));
    }

    // ── GUI thread: results ─────────────────────────────────────────────────────

    bool MirrorChannel::pollEntities(QVector<EntityEntry>& out)
    {
        std::lock_guard<std::mutex> lk(_m);
        if (!_outEntitiesDirty)
        {
            return false;
        }
        out = _outEntities;
        _outEntitiesDirty = false;
        return true;
    }

    bool MirrorChannel::pollComponents(QStringList& out)
    {
        std::lock_guard<std::mutex> lk(_m);
        if (!_outComponentsDirty)
        {
            return false;
        }
        out = _outComponents;
        _outComponentsDirty = false;
        return true;
    }

    std::vector<MirrorChannel::ValueUpdate> MirrorChannel::pollValues()
    {
        std::lock_guard<std::mutex> lk(_m);
        std::vector<ValueUpdate> v;
        v.reserve(static_cast<size_t>(_outValues.size()));
        for (auto it = _outValues.cbegin(); it != _outValues.cend(); ++it)
        {
            v.push_back({ it.key(), it.value() });
        }
        _outValues.clear();
        return v;
    }

    // ── simulation thread: producer ─────────────────────────────────────────────

    MirrorChannel::Intent MirrorChannel::takeIntent()
    {
        Intent in;
        std::lock_guard<std::mutex> lk(_m);
        in.entity = _inEntity;
        in.component = _inComponent;
        in.required = _required;
        in.paths = _inPaths;
        in.edits.swap(_edits);
        return in;
    }

    void MirrorChannel::publishEntities(const QVector<EntityEntry>& entities)
    {
        std::lock_guard<std::mutex> lk(_m);
        _outEntities = entities;
        _outEntitiesDirty = true;
    }

    void MirrorChannel::publishComponents(const QStringList& components)
    {
        std::lock_guard<std::mutex> lk(_m);
        _outComponents = components;
        _outComponentsDirty = true;
    }

    void MirrorChannel::publishValues(std::vector<ValueUpdate>&& values)
    {
        std::lock_guard<std::mutex> lk(_m);
        for (auto& v : values)
        {
            _outValues.insert(v.path, std::move(v.value)); // coalesce: latest per path
        }
    }

} // namespace rpe

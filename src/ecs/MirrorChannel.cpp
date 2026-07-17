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

    void MirrorChannel::requestResync()
    {
        std::lock_guard<std::mutex> lk(_m);
        _resync = true;
    }

    void MirrorChannel::queueStructural(StructuralKind kind, qulonglong entity, const QString& component)
    {
        std::lock_guard<std::mutex> lk(_m);
        _structurals.push_back(StructuralEdit { kind, entity, component, 0 });
    }

    void MirrorChannel::queueStructuralById(StructuralKind kind, qulonglong entity, qulonglong rawId)
    {
        std::lock_guard<std::mutex> lk(_m);
        _structurals.push_back(StructuralEdit { kind, entity, QString(), rawId });
    }

    // ── GUI thread: pinned watches ──────────────────────────────────────────────

    void MirrorChannel::setPins(const QVector<PinKey>& pins)
    {
        std::lock_guard<std::mutex> lk(_m);
        _pins = pins;
    }

    void MirrorChannel::queuePinEdit(const PinKey& key, rttr::variant value)
    {
        std::lock_guard<std::mutex> lk(_m);
        _pinEdits.emplace_back(key, std::move(value));
    }

    std::vector<MirrorChannel::PinValue> MirrorChannel::pollPinValues()
    {
        std::lock_guard<std::mutex> lk(_m);
        std::vector<PinValue> v;
        v.reserve(static_cast<size_t>(_outPinValues.size()));
        for (auto it = _outPinValues.cbegin(); it != _outPinValues.cend(); ++it)
        {
            v.push_back(it.value());
        }
        _outPinValues.clear();
        return v;
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
        out.clear();
        for (const ComponentRow& r : _outComponents)
        {
            if (r.kind == RowKind::Data)
            {
                out.append(r.name);
            }
        }
        _outComponentsDirty = false;
        return true;
    }

    bool MirrorChannel::pollComponentRows(QVector<ComponentRow>& out)
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

    bool MirrorChannel::pollCatalog(QStringList& out)
    {
        std::lock_guard<std::mutex> lk(_m);
        if (!_outCatalogDirty)
        {
            return false;
        }
        out.clear();
        for (const CatalogEntry& e : _outCatalog)
        {
            out.append(e.path);
        }
        _outCatalogDirty = false;
        return true;
    }

    bool MirrorChannel::pollCatalogEntries(QVector<CatalogEntry>& out)
    {
        std::lock_guard<std::mutex> lk(_m);
        if (!_outCatalogDirty)
        {
            return false;
        }
        out = _outCatalog;
        _outCatalogDirty = false;
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
        in.structurals.swap(_structurals);
        in.pins = _pins;
        in.pinEdits.swap(_pinEdits);
        in.resync = _resync;
        _resync = false;
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
        QVector<ComponentRow> rows;
        rows.reserve(components.size());
        for (const QString& c : components)
        {
            rows.append(ComponentRow { c, QString(), RowKind::Data, 0 });
        }
        publishComponentRows(rows);
    }

    void MirrorChannel::publishComponentRows(const QVector<ComponentRow>& rows)
    {
        std::lock_guard<std::mutex> lk(_m);
        _outComponents = rows;
        _outComponentsDirty = true;
    }

    void MirrorChannel::publishCatalog(const QStringList& catalog)
    {
        QVector<CatalogEntry> entries;
        entries.reserve(catalog.size());
        for (const QString& c : catalog)
        {
            entries.append(CatalogEntry { c, false });
        }
        publishCatalogEntries(entries);
    }

    void MirrorChannel::publishCatalogEntries(const QVector<CatalogEntry>& entries)
    {
        std::lock_guard<std::mutex> lk(_m);
        _outCatalog = entries;
        _outCatalogDirty = true;
    }

    void MirrorChannel::publishValues(std::vector<ValueUpdate>&& values)
    {
        std::lock_guard<std::mutex> lk(_m);
        for (auto& v : values)
        {
            _outValues.insert(v.path, std::move(v.value)); // coalesce: latest per path
        }
    }

    void MirrorChannel::publishPinValues(std::vector<PinValue>&& values)
    {
        std::lock_guard<std::mutex> lk(_m);
        for (auto& v : values)
        {
            // Coalesce: keep only the latest value per pin.
            const QString k = QStringLiteral("%1|%2|%3").arg(v.key.entity).arg(v.key.component, v.key.path);
            _outPinValues.insert(k, std::move(v));
        }
    }

} // namespace rpe

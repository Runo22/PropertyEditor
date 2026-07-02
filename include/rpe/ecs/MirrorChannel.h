#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QHash>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  MirrorChannel — the thread-safe, flecs-free data channel between EcsMirror
    //  (simulation-thread producer) and the GUI (EntityComponentBrowser consumer).
    //
    //  It is held by std::shared_ptr from BOTH sides, which decouples their
    //  lifetimes: the producer (EcsMirror, owning the flecs system) may be
    //  destroyed on the sim thread before the GUI tears down. When that happens
    //  the producer calls markProducerGone(); the GUI keeps its shared_ptr, so its
    //  poll*() calls remain valid (they simply return no new data) instead of
    //  dereferencing freed memory. The channel itself owns no flecs resources, so
    //  its final destruction (on whichever thread releases last) is thread-safe.
    // ─────────────────────────────────────────────────────────────────────────────
    class MirrorChannel
    {
    public:
        struct EntityEntry
        {
            qulonglong id = 0;
            QString label;
            bool operator==(const EntityEntry& o) const
            {
                return id == o.id && label == o.label;
            }
            bool operator!=(const EntityEntry& o) const
            {
                return !(*this == o);
            }
        };
        struct ValueUpdate
        {
            QString path;
            rttr::variant value;
        };

        // ── GUI thread: intent ───────────────────────────────────────────────────
        void setRequiredComponent(const QString& componentName);
        void setInterest(qulonglong entity, const QString& componentName, const QStringList& leafPaths);
        void clearInterest();
        void queueEdit(const QString& path, rttr::variant value);
        // The consumer reset its view (e.g. re-selected the same entity/component,
        // or rebound the property tree). The producer dedups publishes against what
        // it last sent, so without this it would NOT resend identical data and the
        // reset view would stay empty. Call this to force one full resend.
        void requestResync();

        // Structural component edits (add/remove a component on an entity). Applied
        // on the simulation thread by the producer — structural world changes are
        // never safe from the GUI thread. `component` is the flecs component name.
        enum class StructuralKind
        {
            AddComponent,
            RemoveComponent
        };
        struct StructuralEdit
        {
            StructuralKind kind;
            qulonglong entity = 0;
            QString component;
        };
        void queueStructural(StructuralKind kind, qulonglong entity, const QString& component);

        // ── GUI thread: results ──────────────────────────────────────────────────
        bool pollEntities(QVector<EntityEntry>& out);
        bool pollComponents(QStringList& out);
        std::vector<ValueUpdate> pollValues();
        // Catalog of all bridged component names present in the world (for the "add
        // component" picker). True if changed since the last poll.
        bool pollCatalog(QStringList& out);

        // True until the producing EcsMirror is destroyed.
        bool producerAlive() const
        {
            return _producerAlive.load(std::memory_order_acquire);
        }

        // ── simulation thread: producer side (called by EcsMirror::pump) ─────────
        struct Intent
        {
            qulonglong entity = 0;
            QString component;
            QString required;
            QStringList paths;
            std::vector<std::pair<QString, rttr::variant>> edits; // drained
            std::vector<StructuralEdit> structurals;              // drained
            bool resync = false; // consumer reset its view → resend everything
        };
        Intent takeIntent();
        void publishEntities(const QVector<EntityEntry>& entities);
        void publishComponents(const QStringList& components);
        void publishValues(std::vector<ValueUpdate>&& values);
        void publishCatalog(const QStringList& catalog);
        void markProducerGone()
        {
            _producerAlive.store(false, std::memory_order_release);
        }

    private:
        mutable std::mutex _m;

        // GUI -> sim
        qulonglong _inEntity = 0;
        QString _inComponent;
        QStringList _inPaths;
        QString _required;
        std::vector<std::pair<QString, rttr::variant>> _edits;
        std::vector<StructuralEdit> _structurals;
        bool _resync = false; // set by requestResync(), drained by takeIntent()

        // sim -> GUI
        QVector<EntityEntry> _outEntities;
        bool _outEntitiesDirty = false;
        QStringList _outComponents;
        bool _outComponentsDirty = false;
        QStringList _outCatalog;
        bool _outCatalogDirty = false;
        // Keyed by path: keeps only the latest value per leaf, so a stalled/hidden
        // consumer can't make this grow unbounded.
        QHash<QString, rttr::variant> _outValues;

        std::atomic<bool> _producerAlive { true };
    };

} // namespace rpe

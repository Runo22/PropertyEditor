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

        // One row of the selected entity's composition. Besides bridged DATA
        // components (inspectable/editable), the list carries zero-size TAGS and
        // PAIRS — pure presence state with nothing to edit, shown as badge rows.
        enum class RowKind : quint8
        {
            Data = 0,
            Tag = 1,
            Pair = 2,     // DATALESS pair: presence only, badge row
            PairData = 3, // pair that CARRIES data (ecs_get_typeid) — selectable/editable
        };
        struct ComponentRow
        {
            QString name;       // full scoped path (pairs: the RELATION's path)
            QString pairTarget; // pairs only: target's display name
            RowKind kind = RowKind::Data;
            qulonglong rawId = 0; // flecs id (pair-encoded for pairs) — removal identity
            // PairData only: the RTTR-resolvable name of the type the pair carries
            // (usually the relation's type). Empty for other kinds.
            QString typeName;

            // Unique selection/interest identity. A relation can pair with several
            // targets carrying the same type ((Damage,Fire), (Damage,Ice)), so the
            // name alone is ambiguous for pairs.
            QString key() const
            {
                return (kind == RowKind::Pair || kind == RowKind::PairData)
                    ? name + QStringLiteral(" (") + pairTarget + QLatin1Char(')')
                    : name;
            }
            bool operator==(const ComponentRow& o) const
            {
                return name == o.name && pairTarget == o.pairTarget && kind == o.kind
                    && rawId == o.rawId && typeName == o.typeName;
            }
            bool operator!=(const ComponentRow& o) const
            {
                return !(*this == o);
            }
        };

        // Add-component catalog entry (tag == zero-size component: addable, no data).
        struct CatalogEntry
        {
            QString path;
            bool tag = false;
            bool operator==(const CatalogEntry& o) const
            {
                return path == o.path && tag == o.tag;
            }
            bool operator!=(const CatalogEntry& o) const
            {
                return !(*this == o);
            }
        };

        // A spawnable prefab for the "add entity" picker: its id (the spawn handle),
        // display name, and the group tag it matched (empty = ungrouped). The group is
        // computed producer-side from the host-provided group tags (setPrefabGroupTags).
        struct PrefabEntry
        {
            qulonglong id = 0;
            QString name;
            QString group;
            bool operator==(const PrefabEntry& o) const
            {
                return id == o.id && name == o.name && group == o.group;
            }
            bool operator!=(const PrefabEntry& o) const
            {
                return !(*this == o);
            }
        };

        // A pinned (watched) property, independent of the current selection: any
        // entity + component + leaf path. Pins are mirrored every pump alongside the
        // selected component, so a watch widget can show values from several
        // entities at once.
        struct PinKey
        {
            qulonglong entity = 0;
            QString component; // full scoped flecs name, or a pair's "Rel (Target)" key
            QString path;      // property dot-path inside the component
            // Data-carrying pairs have no resolvable flecs name; when set, the producer
            // resolves the component by this exact id (pair-encoded) + ecs_get_typeid,
            // instead of by name. 0 for ordinary components (resolved by `component`).
            qulonglong rawId = 0;
            bool operator==(const PinKey& o) const
            {
                // Identity is (entity, component, path) — `component` is already unique
                // per pair ("Rel (Target)"), so rawId is a resolution aid, not identity.
                return entity == o.entity && component == o.component && path == o.path;
            }
        };
        struct PinValue
        {
            PinKey key;
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
            RemoveComponent,
            SpawnPrefab,  // instantiate a new entity from a prefab (rawId = prefab id)
            DestroyEntity // delete an entity outright (entity = the one to destroy)
        };
        struct StructuralEdit
        {
            StructuralKind kind;
            qulonglong entity = 0;
            QString component;    // by-name form (empty when rawId is used)
            qulonglong rawId = 0; // by-id form — required for pairs, exact for tags
        };
        void queueStructural(StructuralKind kind, qulonglong entity, const QString& component);
        // By flecs id — the only way to address a PAIR, and unambiguous for tags.
        void queueStructuralById(StructuralKind kind, qulonglong entity, qulonglong rawId);
        // Spawn a new entity from prefab `prefabId` (producer runs is_a + configurator).
        void queueSpawnPrefab(qulonglong prefabId);
        // Destroy `entity` on the simulation thread.
        void queueDestroyEntity(qulonglong entity);

        // Tag names the producer groups spawnable prefabs by (a prefab is filed under
        // the first of these it carries). Also drives which prefabs the "add entity"
        // picker offers, alongside the existing required-component filter.
        void setPrefabGroupTags(const QStringList& tags);

        // ── GUI thread: pinned watches ───────────────────────────────────────────
        // Replace the full pin set (atomic swap; the producer reads it next pump).
        void setPins(const QVector<PinKey>& pins);
        // Queue a value edit for a pinned property (applied on the sim thread, like
        // queueEdit but addressed to an explicit entity + component).
        void queuePinEdit(const PinKey& key, rttr::variant value);
        // Latest changed pin values (drained; keyed producer-side so a hidden
        // consumer can't make this grow unbounded).
        std::vector<PinValue> pollPinValues();
        // Pins whose target is GONE — the entity was destroyed, or the component was
        // removed from it. Drained; the watch widget removes these rows so a pin can't
        // linger showing a stale value after the thing it watched is deleted. (A pin
        // that is merely temporarily unresolvable — e.g. its bridge hasn't registered
        // yet — is NOT reported here.)
        QVector<PinKey> pollDeadPins();

        // ── GUI thread: results ──────────────────────────────────────────────────
        bool pollEntities(QVector<EntityEntry>& out);
        bool pollComponents(QStringList& out); // legacy: DATA row names only
        bool pollComponentRows(QVector<ComponentRow>& out); // full composition
        std::vector<ValueUpdate> pollValues();
        // Catalog of addable components (bridged data types + zero-size tags) for
        // the "add component" picker. True if changed since the last poll.
        bool pollCatalog(QStringList& out); // legacy: paths only
        bool pollCatalogEntries(QVector<CatalogEntry>& out);
        // Spawnable prefabs for the "add entity" picker. True if changed since last poll.
        bool pollPrefabs(QVector<PrefabEntry>& out);

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
            QStringList prefabGroups;                             // prefab grouping tags
            std::vector<std::pair<QString, rttr::variant>> edits; // drained
            std::vector<StructuralEdit> structurals;              // drained
            QVector<PinKey> pins;                                 // current pin set
            std::vector<std::pair<PinKey, rttr::variant>> pinEdits; // drained
            bool resync = false; // consumer reset its view → resend everything
        };
        Intent takeIntent();
        void publishEntities(const QVector<EntityEntry>& entities);
        void publishComponents(const QStringList& components); // legacy → data rows
        void publishComponentRows(const QVector<ComponentRow>& rows);
        void publishValues(std::vector<ValueUpdate>&& values);
        void publishPinValues(std::vector<PinValue>&& values);
        // Report pins whose target no longer exists (entity destroyed / component
        // removed). Coalesced producer-side by pin identity.
        void publishDeadPins(const QVector<PinKey>& keys);
        void publishCatalog(const QStringList& catalog); // legacy → non-tag entries
        void publishCatalogEntries(const QVector<CatalogEntry>& entries);
        void publishPrefabs(const QVector<PrefabEntry>& prefabs);
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
        QStringList _prefabGroups;
        std::vector<std::pair<QString, rttr::variant>> _edits;
        std::vector<StructuralEdit> _structurals;
        QVector<PinKey> _pins;
        std::vector<std::pair<PinKey, rttr::variant>> _pinEdits;
        bool _resync = false; // set by requestResync(), drained by takeIntent()

        // sim -> GUI
        QVector<EntityEntry> _outEntities;
        bool _outEntitiesDirty = false;
        QVector<ComponentRow> _outComponents;
        bool _outComponentsDirty = false;
        QVector<CatalogEntry> _outCatalog;
        bool _outCatalogDirty = false;
        QVector<PrefabEntry> _outPrefabs;
        bool _outPrefabsDirty = false;
        // Keyed by path: keeps only the latest value per leaf, so a stalled/hidden
        // consumer can't make this grow unbounded.
        QHash<QString, rttr::variant> _outValues;
        // Same idea for pins, keyed by "entity|component|path".
        QHash<QString, PinValue> _outPinValues;
        // Pins whose target vanished, coalesced by the same key (drained by the GUI).
        QHash<QString, PinKey> _outDeadPins;

        std::atomic<bool> _producerAlive { true };
    };

} // namespace rpe

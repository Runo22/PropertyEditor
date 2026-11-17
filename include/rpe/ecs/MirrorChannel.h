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
        MirrorChannel();
        ~MirrorChannel();
        MirrorChannel(const MirrorChannel&) = delete;
        MirrorChannel& operator=(const MirrorChannel&) = delete;

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
            // Which entity + component the value was READ from. A consumer that has
            // switched selection since must drop it: the producer may have read the
            // old target in a pump that raced the switch, and the same leaf path
            // exists on the new one. 0 / empty = unknown (accept).
            qulonglong entity = 0;
            QString component;
        };

        // A value edit, addressed to the entity + component it was MADE on. The target
        // is fixed when the edit is queued, never at apply time: an inline editor
        // commits on focus-out, i.e. in the very click that selects another entity, so
        // "whatever is selected when the sim gets round to it" is often the wrong one.
        struct EditRequest
        {
            qulonglong entity = 0;
            QString component; // the selection key (full path, or "Rel (Target)")
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

        // A suggested target for a relationship in the add picker.
        struct PairTarget
        {
            qulonglong id = 0; // the target entity (generation included)
            QString path;      // its full path ("elements.Fire"), or "#<id>" if unnamed
            bool operator==(const PairTarget& o) const
            {
                return id == o.id && path == o.path;
            }
            bool operator!=(const PairTarget& o) const
            {
                return !(*this == o);
            }
        };

        // How the add picker offers a catalog entry.
        enum class PairUse : quint8
        {
            None, // added by itself only
            Only, // a relationship: added as a pair, never by itself
            Also, // either — by itself, or as a pair
        };

        // Add-component catalog entry (tag == zero-size component or plain entity:
        // addable, no data). A RELATIONSHIP entry (pair != None) also carries the
        // targets to suggest; any other target can still be picked or typed.
        struct CatalogEntry
        {
            QString path;
            bool tag = false;
            qulonglong id = 0; // the component / relationship entity (generation included)
            PairUse pair = PairUse::None;
            bool exclusive = false;      // flecs Exclusive: a new target replaces the old one
            bool targetsFixed = false;   // flecs OneOf: only `targets` are valid
            QVector<PairTarget> targets; // suggestions (OneOf children, or targets in use)
            bool moreTargets = false;    // `targets` was cut at the suggestion limit
            bool operator==(const CatalogEntry& o) const
            {
                return path == o.path && tag == o.tag && id == o.id && pair == o.pair && exclusive == o.exclusive
                    && targetsFixed == o.targetsFixed && targets == o.targets && moreTargets == o.moreTargets;
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

        // ── Search all components ────────────────────────────────────────────────
        // While a search is set, the producer searches every bridged component of
        // `entity` for rows whose name or value text contains `text` (the property
        // filter's rule — see rpe/core/PropertySearch.h) and publishes the hits:
        // a round every ~250 ms, sliced across pumps so no single frame pays for it.
        struct SearchHit
        {
            QString component; // the component's selection key
            QString path;      // row path inside it
            rttr::variant value;
        };
        struct SearchResults
        {
            qulonglong entity = 0;
            QString text;
            std::vector<SearchHit> hits;
        };
        // Empty text (or entity 0) stops searching.
        void setSearch(qulonglong entity, const QString& text);
        // Start a round now (e.g. right after an edit, to echo it promptly).
        void requestSearchRefresh();
        // The latest complete round, if one finished since the last poll.
        bool pollSearchResults(SearchResults& out);

        // ── GUI thread: intent ───────────────────────────────────────────────────
        void setRequiredComponent(const QString& componentName);
        void setInterest(qulonglong entity, const QString& componentName, const QStringList& leafPaths);
        void clearInterest();
        // Edit the leaf `path` of the current interest (entity + component as set by the
        // last setInterest) — captured NOW, so a later selection change can't redirect it.
        void queueEdit(const QString& path, rttr::variant value);
        // Edit the leaf `path` of an explicit entity + component.
        void queueEdit(qulonglong entity, const QString& component, const QString& path, rttr::variant value);
        // The consumer reset its view (e.g. re-selected the same entity/component,
        // or rebound the property tree). The producer dedups publishes against what
        // it last sent, so without this it would NOT resend identical data and the
        // reset view would stay empty. Call this to force one full resend.
        void requestResync();
        // Ask the producer to rescan the add-component catalog and the spawnable
        // prefabs on its next pump — e.g. because an Add menu is opening. Both are
        // otherwise rescanned only on change (component count / bridge registry) and
        // on a slow safety interval, which can't see every change (a renamed
        // component, or one unloaded and another loaded in the same pump).
        void requestCatalogRefresh();

        // Structural component edits (add/remove a component on an entity). Applied
        // on the simulation thread by the producer — structural world changes are
        // never safe from the GUI thread. `component` is the flecs component name.
        enum class StructuralKind
        {
            AddComponent,
            RemoveComponent,
            SpawnPrefab,   // instantiate a new entity from a prefab (rawId = prefab id)
            DestroyEntity, // delete an entity outright (entity = the one to destroy)
            AddPair        // add (rawId, target) — target by id, else by path (component)
        };
        struct StructuralEdit
        {
            StructuralKind kind;
            qulonglong entity = 0;
            QString component;    // by-name form (empty when rawId is used); AddPair: target path
            qulonglong rawId = 0; // by-id form — required for pairs, exact for tags; AddPair: relation
            qulonglong target = 0; // AddPair: the target entity (0 = look `component` up)
        };
        void queueStructural(StructuralKind kind, qulonglong entity, const QString& component);
        // By flecs id — the only way to address a PAIR, and unambiguous for tags.
        void queueStructuralById(StructuralKind kind, qulonglong entity, qulonglong rawId);
        // Add the pair (relation, target) to `entity`. Ids carry their generation, so
        // a target deleted (and its id recycled) since the GUI saw it is refused, not
        // swapped for another entity. With target 0, `targetPath` is looked up instead
        // ("elements.Fire", "elements::Fire" or "#<id>"). The producer checks flecs'
        // rules (Relationship / Target / OneOf) first and skips a pair that breaks
        // them — a debug flecs build would abort on it.
        void queueAddPair(qulonglong entity, qulonglong relation, qulonglong target, const QString& targetPath = QString());
        // Spawn a new entity from prefab `prefabId` (producer runs is_a + configurator).
        void queueSpawnPrefab(qulonglong prefabId);
        // Destroy `entity` on the simulation thread.
        void queueDestroyEntity(qulonglong entity);

        // Tag names the producer groups spawnable prefabs by (a prefab is filed under
        // the first of these it carries). Also drives which prefabs the "add entity"
        // picker offers, alongside the existing required-component filter.
        void setPrefabGroupTags(const QStringList& tags);

        // Relationships the add picker offers as pairs besides those it finds itself
        // (see EntityComponentBrowser::Settings::pairRelations).
        void setPairRelations(const QStringList& names);

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
        // Named entities, by path — what a relationship's "Other target…" offers.
        // Published with the catalog. True if changed since the last poll.
        bool pollTargetCandidates(QVector<EntityEntry>& out);

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
            QStringList pairRelations;                            // declared relationships
            std::vector<EditRequest> edits;                      // drained
            std::vector<StructuralEdit> structurals;              // drained
            QVector<PinKey> pins;                                 // current pin set
            std::vector<std::pair<PinKey, rttr::variant>> pinEdits; // drained
            bool resync = false; // consumer reset its view → resend everything
            bool catalogRefresh = false; // rescan catalog + prefabs now
            qulonglong searchEntity = 0; // search all components (see setSearch)
            QString searchText;
            bool searchNow = false;
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
        void publishTargetCandidates(const QVector<EntityEntry>& entities);
        void publishSearchResults(SearchResults&& results);
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
        QStringList _pairRelations;
        std::vector<EditRequest> _edits;
        std::vector<StructuralEdit> _structurals;
        QVector<PinKey> _pins;
        std::vector<std::pair<PinKey, rttr::variant>> _pinEdits;
        bool _resync = false; // set by requestResync(), drained by takeIntent()
        qulonglong _searchEntity = 0;
        QString _searchText;
        bool _searchNow = false;
        bool _catalogRefresh = false;

        // sim -> GUI
        QVector<EntityEntry> _outEntities;
        bool _outEntitiesDirty = false;
        QVector<ComponentRow> _outComponents;
        bool _outComponentsDirty = false;
        QVector<CatalogEntry> _outCatalog;
        bool _outCatalogDirty = false;
        QVector<PrefabEntry> _outPrefabs;
        bool _outPrefabsDirty = false;
        QVector<EntityEntry> _outTargets;
        bool _outTargetsDirty = false;
        // Keyed by path: keeps only the latest value per leaf, so a stalled/hidden
        // consumer can't make this grow unbounded.
        QHash<QString, ValueUpdate> _outValues; // coalesced by path; carries its target
        // Same idea for pins, keyed by "entity|component|path".
        QHash<QString, PinValue> _outPinValues;
        // Pins whose target vanished, coalesced by the same key (drained by the GUI).
        QHash<QString, PinKey> _outDeadPins;
        SearchResults _outSearch; // latest complete search round
        bool _outSearchDirty = false;

        std::atomic<bool> _producerAlive { true };

        // Type release (rpe/core/TypeRelease.h): drop every queued value — a value
        // read through a plugin's property calls into the plugin when destroyed.
        int _releaseHandle = 0;
        void _onTypesReleased();
    };

} // namespace rpe

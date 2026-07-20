#pragma once

#include "rpe/core/rttr_prelude.h"

#include "rpe/core/AccessGuard.h"
#include "rpe/core/PropertyNode.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QSet>
#include <QMutex>

#include <atomic>
#include <functional>
#include <memory>

Q_DECLARE_METATYPE(rttr::variant)

namespace rpe
{

    // Custom item-data roles exposed to the delegate.
    // (rttr::property is not default-constructible, so it cannot be a QVariant;
    //  the metadata the delegate needs is surfaced through plain roles instead.)
    enum PropertyRole
    {
        HasLocalEditRole = Qt::UserRole + 1,
        PropertyPathRole, // QString dot-path
        RttrVariantRole,  // current effective rttr::variant
        IsLeafRole,       // bool — true if directly editable leaf
        EditorHintRole,   // QString — rpe::editor::* (empty if unset)
        MinRole,          // double  — invalid QVariant if unset
        MaxRole,          // double  — invalid QVariant if unset
        StepRole,         // double  — invalid QVariant if unset
        DecimalsRole,     // int     — invalid QVariant if unset
        IsArrayRole,      // bool    — true for a sequential/array parent node
        DeclaredTypeRole, // rttr::variant wrapping the node's declared rttr::type
    };

    // How committed edits are applied.
    enum class EditPolicy
    {
        // Keep the edit as a LOCAL DRAFT: the row shows your value and stops
        // following live updates until reset. The bound object/world is never
        // touched. (Formerly named "Override" — nothing is ever written.)
        LocalEdit,
        WriteBack, // write the value straight into the bound object (true "set data")
    };

    // ─────────────────────────────────────────────────────────────────────────────
    //  PropertyModel — QAbstractItemModel over an RTTR type's property graph.
    //
    //  Design goals:
    //   • Schema is built once (bindType). The hot path (refresh) only re-reads
    //     values and emits tight dataChanged ranges — no tree rebuilds, no per-row
    //     allocations — so dozens of these can update at 50Hz+.
    //   • setPropertyValue() is thread-safe: values pushed from a worker/sim thread
    //     are coalesced and flushed on the GUI thread.
    //   • Two edit policies (local-edit vs. write-back), selectable per instance.
    // ─────────────────────────────────────────────────────────────────────────────
    class PropertyModel : public QAbstractItemModel
    {
        Q_OBJECT

    public:
        explicit PropertyModel(QObject* parent = nullptr);
        ~PropertyModel() override;

        // ── Schema (GUI thread) ──────────────────────────────────────────────────
        void bindType(rttr::type type);
        void unbind();
        rttr::type boundType() const
        {
            return _boundType;
        }

        // ── Data ─────────────────────────────────────────────────────────────────
        // Re-read every property from a live instance (GUI thread).
        void refresh(const rttr::instance& obj);

        // Inject one value by dot-path from any thread (coalesced, flushed on GUI thread).
        void setPropertyValue(const QString& path, rttr::variant val);

        // ── Editing behaviour ────────────────────────────────────────────────────
        void setReadOnly(bool ro);
        bool isReadOnly() const
        {
            return _readOnly;
        }

        void setEditPolicy(EditPolicy p)
        {
            _editPolicy = p;
        }
        EditPolicy editPolicy() const
        {
            return _editPolicy;
        }

        // Provider that returns the object writes should target (for WriteBack and
        // for refreshing after a write). May be empty.
        void setInstanceProvider(std::function<rttr::instance()> provider);

        // Optional guard wrapped around WriteBack writes (instance provider + set),
        // for objects owned by another thread. See rpe/core/AccessGuard.h.
        void setWriteGuard(AccessGuard guard);

        // When set, committed edits are routed here instead of being written or
        // pinned, and the row resumes live updates immediately (used by mirror mode:
        // the edit is queued to the sim thread, which echoes the new value back).
        void setEditSink(std::function<void(const QString&, const rttr::variant&)> sink);

        // Dot-paths of every directly-editable leaf in the current schema.
        QStringList allLeafPaths() const;

        // Paths pinned to a watch list (PinnedPropertiesWidget). Purely visual
        // here: pinned rows are tinted so they are recognisable in the main tree.
        void setPinnedPaths(const QSet<QString>& paths);

        // View → model expansion state. A COLLAPSED struct row with few fields
        // shows a compact "[a, b]" summary of its children in the value column;
        // expanding it hides the summary (the children now show the values), so
        // the same data is never on screen twice. The view reports its expand/
        // collapse transitions here (PropertyEditor wires QTreeView::expanded/
        // collapsed). Arrays are unaffected — they always show "[N]".
        void setPathExpanded(const QString& path, bool expanded);
        bool isPinnedPath(const QString& path) const
        {
            return _pinnedPaths.contains(path);
        }

        // ── Local edit / reset ──────────────────────────────────────────────────────
        void beginLocalEdit(const QString& path);
        void resetNode(const QString& path);
        void resetAll();
        bool hasAnyLocalEdit() const;

        // ── QAbstractItemModel ───────────────────────────────────────────────────
        QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
        QModelIndex parent(const QModelIndex& child) const override;
        int rowCount(const QModelIndex& parent = {}) const override;
        int columnCount(const QModelIndex& parent = {}) const override;
        QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
        bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
        Qt::ItemFlags flags(const QModelIndex& index) const override;
        QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    signals:
        void propertyEdited(const QString& path, const rttr::variant& newValue);

    private slots:
        void _flushPending();

    private:
        QModelIndex _indexFromNode(PropertyNode* node, int column = 0) const;
        void _resetRoot();
        void _buildTree(PropertyNode* parent, rttr::type type, const QString& prefix);
        void _refreshNode(PropertyNode* node, const rttr::variant& val);
        void _refreshSequential(PropertyNode* node, const rttr::variant& val);
        void _rebuildArrayChildren(PropertyNode* node, const rttr::variant& arrayVal);
        static bool _anyDescendantLocallyEdited(const PropertyNode* node);
        void _applyBatch(const QHash<QString, rttr::variant>& batch);
        bool _emitDirtyRanges(PropertyNode* parent);
        QString _structSummary(PropertyNode* node) const;
        void _collectNodes(PropertyNode* node, QHash<QString, PropertyNode*>& out) const;
        PropertyNode* _findNode(const QString& path) const;
        bool _applyEdit(PropertyNode* node, const rttr::variant& newVal);

        std::unique_ptr<PropertyNode> _root;
        QHash<QString, PropertyNode*> _nodeByPath;
        rttr::type _boundType = rttr::type::get<void>();

        mutable QMutex _pendingMutex;
        QHash<QString, rttr::variant> _pendingUpdates;
        std::atomic<bool> _flushScheduled { false };

        bool _readOnly = false;
        QSet<QString> _pinnedPaths;   // watch-list tint (see setPinnedPaths)
        QSet<QString> _expandedPaths; // rows currently expanded in the view (see setPathExpanded)
        EditPolicy _editPolicy = EditPolicy::LocalEdit;
        std::function<rttr::instance()> _instanceProvider;
        AccessGuard _writeGuard;
        std::function<void(const QString&, const rttr::variant&)> _editSink;
    };

} // namespace rpe

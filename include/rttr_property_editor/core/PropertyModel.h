#pragma once

#include "PropertyNode.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QMutex>
#include <QTimer>
#include <QVector>

#include <atomic>
#include <memory>

#include <rttr/instance>
#include <rttr/type>
#include <rttr/variant>

Q_DECLARE_METATYPE(rttr::variant)

namespace rpe {

// Custom roles
enum PropertyRole {
    IsOverriddenRole = Qt::UserRole + 1,
    PropertyPathRole,
    RttrTypeRole,    // stores rttr::type as quint64 (type_id)
    RttrVariantRole, // stores rttr::variant wrapped in QVariant via Q_DECLARE_METATYPE
};

class PropertyModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    explicit PropertyModel(QObject* parent = nullptr);
    ~PropertyModel() override;

    // ── Schema API (main thread) ──────────────────────────────────────────────
    void bindType(rttr::type type);
    void unbind();

    // ── Data API ─────────────────────────────────────────────────────────────
    // Main-thread: reads all properties from a live rttr::instance and updates model.
    void refresh(const rttr::instance& obj);

    // Thread-safe: inject a single value by dot-path. Can be called from any thread.
    void setPropertyValue(const QString& path, rttr::variant val);

    // ── Override / reset ─────────────────────────────────────────────────────
    void overrideNode(const QString& path);
    void resetNode(const QString& path);
    void resetAll();

    bool hasAnyOverride() const;

    // ── QAbstractItemModel ────────────────────────────────────────────────────
    QModelIndex index(int row, int column,
                      const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int         rowCount   (const QModelIndex& parent = {}) const override;
    int         columnCount(const QModelIndex& parent = {}) const override;
    QVariant    data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool        setData(const QModelIndex& index, const QVariant& value,
                        int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QVariant    headerData(int section, Qt::Orientation orientation,
                           int role = Qt::DisplayRole) const override;

signals:
    // Emitted when the user commits an edit through the delegate.
    void propertyEdited(const QString& path, const rttr::variant& newValue);

private slots:
    void _flushPending();

private:
    // Tree helpers
    PropertyNode* _nodeFromIndex(const QModelIndex& idx) const;
    QModelIndex   _indexFromNode(PropertyNode* node, int column = 0) const;
    void          _buildTree(PropertyNode* parent,
                             rttr::type    type,
                             const QString& pathPrefix);
    void _refreshNode(PropertyNode* node, const rttr::variant& parentVal);
    void _rebuildArrayChildren(PropertyNode* node, const rttr::variant& arrayVal);
    void _applyBatch(const QHash<QString, rttr::variant>& batch);
    void _emitDirtyRanges(PropertyNode* parent);
    PropertyNode* _findNode(const QString& path) const;
    void _collectNodes(PropertyNode* node,
                       QHash<QString, PropertyNode*>& out) const;

    // Root of the property tree (invisible root, holds top-level nodes as children)
    std::unique_ptr<PropertyNode> _root;

    // Flat path → node lookup (rebuilt when schema changes)
    QHash<QString, PropertyNode*> _nodeByPath;

    // Thread-safe pending updates
    mutable QMutex                _pendingMutex;
    QHash<QString, rttr::variant> _pendingUpdates;
    std::atomic<bool>             _flushScheduled{false};

    // Display mode (read by flags())
    bool _readOnly = false;

    // Set of paths with open delegate editors (implicit override)
    QHash<QString, bool> _editorOpen;

    friend class PropertyDelegate; // delegate notifies editor open/close
};

} // namespace rpe

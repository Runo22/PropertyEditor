#include "rttr_property_editor/core/PropertyModel.h"
#include "rttr_property_editor/core/TypeRenderer.h"

#include <QMetaObject>

#include <rttr/property>
#include <rttr/enumeration>
#include <rttr/variant_sequential_view>

namespace rpe {

// ── Construction ─────────────────────────────────────────────────────────────

PropertyModel::PropertyModel(QObject* parent)
    : QAbstractItemModel(parent)
    , _root(std::make_unique<PropertyNode>(
          QStringLiteral("root"), QString(), rttr::type::get<void>(),
          rttr::property(), nullptr))
{
    // Register so rttr::variant can be passed via QVariant across threads
    qRegisterMetaType<rttr::variant>("rttr::variant");
}

PropertyModel::~PropertyModel() = default;

// ── Schema API ────────────────────────────────────────────────────────────────

void PropertyModel::bindType(rttr::type type)
{
    beginResetModel();
    _nodeByPath.clear();
    _root = std::make_unique<PropertyNode>(
        QStringLiteral("root"), QString(), rttr::type::get<void>(),
        rttr::property(), nullptr);
    _buildTree(_root.get(), type, QString());
    _collectNodes(_root.get(), _nodeByPath);
    endResetModel();
}

void PropertyModel::unbind()
{
    beginResetModel();
    _nodeByPath.clear();
    _root = std::make_unique<PropertyNode>(
        QStringLiteral("root"), QString(), rttr::type::get<void>(),
        rttr::property(), nullptr);
    endResetModel();
}

// ── Tree building ─────────────────────────────────────────────────────────────

void PropertyModel::_buildTree(PropertyNode* parent, rttr::type type, const QString& pathPrefix)
{
    for (auto& prop : type.get_properties()) {
        const QString name     = QString::fromStdString(prop.get_name().to_string());
        const QString path     = pathPrefix.isEmpty() ? name : pathPrefix + u'.' + name;
        const rttr::type pType = prop.get_type();

        auto* node = new PropertyNode(name, path, pType, prop, parent);
        parent->children().append(node);

        // Recurse into nested structs at schema time.
        // Sequential containers are expanded lazily on first refresh.
        if (!TypeRenderer::isSequential(pType) && TypeRenderer::isExpandable(pType)) {
            _buildTree(node, pType, path);
        }
    }
}

void PropertyModel::_collectNodes(PropertyNode* node, QHash<QString, PropertyNode*>& out) const
{
    if (!node->path().isEmpty())
        out[node->path()] = node;
    for (auto* child : node->children())
        _collectNodes(child, out);
}

// ── Data API ─────────────────────────────────────────────────────────────────

void PropertyModel::refresh(const rttr::instance& obj)
{
    if (!obj.is_valid() || !_root) return;

    for (auto* child : _root->children()) {
        const rttr::variant val = child->prop().get_value(obj);
        _refreshNode(child, val);
    }
    _emitDirtyRanges(_root.get());
}

void PropertyModel::_refreshNode(PropertyNode* node, const rttr::variant& val)
{
    if (node->isOverridden()) return;

    const rttr::type t = node->type();

    if (TypeRenderer::isSequential(t)) {
        // Update the parent node value (shows "[n]" summary)
        node->setLiveValue(val);
        node->setCachedDisplay(TypeRenderer::toDisplayString(val));

        auto view     = val.create_sequential_view();
        const int sz  = static_cast<int>(view.get_size());

        if (sz != node->arraySize())
            _rebuildArrayChildren(node, val);
        else {
            for (int i = 0; i < sz; ++i) {
                auto* child = node->children()[i];
                if (child->isOverridden()) continue;
                rttr::variant elemVal = view.get_value(static_cast<size_t>(i));
                // extract_wrapped_value unwraps reference wrappers
                if (elemVal.get_type().is_wrapper())
                    elemVal = elemVal.extract_wrapped_value();
                child->setLiveValue(elemVal);
                child->setCachedDisplay(TypeRenderer::toDisplayString(elemVal));
            }
        }
    } else if (TypeRenderer::isExpandable(t)) {
        // Nested struct: update summary and recurse into children
        node->setLiveValue(val);
        node->setCachedDisplay(TypeRenderer::toDisplayString(val));

        for (auto* child : node->children()) {
            if (!val.is_valid()) continue;
            // Create a mutable copy to satisfy rttr::instance(variant&) constructor
            rttr::variant mutableParent = val;
            rttr::instance nestedInst(mutableParent);
            const rttr::variant childVal = child->prop().get_value(nestedInst);
            _refreshNode(child, childVal);
        }
    } else {
        // Leaf: just update the value
        node->setLiveValue(val);
        node->setCachedDisplay(TypeRenderer::toDisplayString(val));
    }
}

void PropertyModel::_rebuildArrayChildren(PropertyNode* node, const rttr::variant& arrayVal)
{
    auto view        = arrayVal.create_sequential_view();
    const int newSz  = static_cast<int>(view.get_size());
    const int oldSz  = node->arraySize();

    const QModelIndex nodeIdx = _indexFromNode(node);

    if (oldSz > 0) {
        beginRemoveRows(nodeIdx, 0, oldSz - 1);
        // Remove from path map
        for (auto* ch : node->children())
            _nodeByPath.remove(ch->path());
        qDeleteAll(node->children());
        node->children().clear();
        endRemoveRows();
    }

    if (newSz > 0) {
        beginInsertRows(nodeIdx, 0, newSz - 1);
        const rttr::type elemType = view.get_value_type();
        for (int i = 0; i < newSz; ++i) {
            const QString elemName = QStringLiteral("[%1]").arg(i);
            const QString elemPath = node->path() + u'.' + elemName;

            auto* elemNode = new PropertyNode(elemName, elemPath, elemType,
                                               rttr::property(), node);
            rttr::variant elemVal = view.get_value(static_cast<size_t>(i));
            if (elemVal.get_type().is_wrapper())
                elemVal = elemVal.extract_wrapped_value();
            elemNode->setLiveValue(elemVal);
            elemNode->setCachedDisplay(TypeRenderer::toDisplayString(elemVal));

            node->children().append(elemNode);
            _nodeByPath[elemPath] = elemNode;
        }
        endInsertRows();
    }

    node->setArraySize(newSz);
}

// ── Thread-safe value injection ───────────────────────────────────────────────

void PropertyModel::setPropertyValue(const QString& path, rttr::variant val)
{
    {
        QMutexLocker lk(&_pendingMutex);
        _pendingUpdates[path] = std::move(val);
    }
    if (!_flushScheduled.exchange(true, std::memory_order_relaxed)) {
        QMetaObject::invokeMethod(this, &PropertyModel::_flushPending,
                                  Qt::QueuedConnection);
    }
}

void PropertyModel::_flushPending()
{
    _flushScheduled.store(false, std::memory_order_relaxed);

    QHash<QString, rttr::variant> batch;
    {
        QMutexLocker lk(&_pendingMutex);
        batch = std::move(_pendingUpdates);
    }
    _applyBatch(batch);
}

void PropertyModel::_applyBatch(const QHash<QString, rttr::variant>& batch)
{
    for (auto it = batch.cbegin(); it != batch.cend(); ++it) {
        auto* node = _findNode(it.key());
        if (!node || node->isOverridden()) continue;
        node->setLiveValue(it.value());
        node->setCachedDisplay(TypeRenderer::toDisplayString(it.value()));
    }
    _emitDirtyRanges(_root.get());
}

void PropertyModel::_emitDirtyRanges(PropertyNode* parent)
{
    auto& ch        = parent->children();
    int   rangeStart= -1;

    auto flush = [&](int end) {
        if (rangeStart < 0) return;
        const QModelIndex parentIdx =
            (parent == _root.get()) ? QModelIndex() : _indexFromNode(parent);
        auto tl = index(rangeStart, 0, parentIdx);
        auto br = index(end - 1, columnCount() - 1, parentIdx);
        emit dataChanged(tl, br);
        rangeStart = -1;
    };

    for (int i = 0; i < ch.size(); ++i) {
        auto* node = ch[i];
        if (node->isDirty()) {
            node->clearDirty();
            if (rangeStart < 0) rangeStart = i;
        } else {
            flush(i);
        }
        if (!node->children().isEmpty())
            _emitDirtyRanges(node);
    }
    flush(static_cast<int>(ch.size()));
}

// ── Override / reset ──────────────────────────────────────────────────────────

void PropertyModel::overrideNode(const QString& path)
{
    auto* node = _findNode(path);
    if (!node || node->isOverridden()) return;
    node->setOverridden(true);
    node->setOverrideValue(node->liveValue());
    const auto idx = _indexFromNode(node);
    emit dataChanged(idx, idx.siblingAtColumn(columnCount() - 1));
}

void PropertyModel::resetNode(const QString& path)
{
    auto* node = _findNode(path);
    if (!node || !node->isOverridden()) return;
    node->setOverridden(false);
    const auto idx = _indexFromNode(node);
    emit dataChanged(idx, idx.siblingAtColumn(columnCount() - 1));
}

void PropertyModel::resetAll()
{
    bool any = false;
    for (auto* node : std::as_const(_nodeByPath)) {
        if (node->isOverridden()) {
            node->setOverridden(false);
            any = true;
        }
    }
    if (any && !_root->children().isEmpty()) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
    }
}

bool PropertyModel::hasAnyOverride() const
{
    for (auto* node : std::as_const(_nodeByPath)) {
        if (node->isOverridden()) return true;
    }
    return false;
}

// ── QAbstractItemModel ────────────────────────────────────────────────────────

QModelIndex PropertyModel::index(int row, int column, const QModelIndex& parent) const
{
    if (!hasIndex(row, column, parent)) return {};

    const PropertyNode* parentNode = parent.isValid()
        ? static_cast<const PropertyNode*>(parent.internalPointer())
        : _root.get();

    if (!parentNode || row >= parentNode->children().size()) return {};
    return createIndex(row, column, parentNode->children()[row]);
}

QModelIndex PropertyModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) return {};
    auto* node       = static_cast<PropertyNode*>(child.internalPointer());
    auto* parentNode = node->parent();
    if (!parentNode || parentNode == _root.get()) return {};
    return createIndex(parentNode->row(), 0, parentNode);
}

int PropertyModel::rowCount(const QModelIndex& parent) const
{
    const PropertyNode* parentNode = parent.isValid()
        ? static_cast<const PropertyNode*>(parent.internalPointer())
        : _root.get();
    return parentNode ? static_cast<int>(parentNode->children().size()) : 0;
}

int PropertyModel::columnCount(const QModelIndex&) const { return 2; }

QVariant PropertyModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) return {};
    auto* node = static_cast<PropertyNode*>(index.internalPointer());

    switch (role) {
    case Qt::DisplayRole:
        if (index.column() == 0) return node->name();
        if (index.column() == 1) {
            if (node->isOverridden())
                return TypeRenderer::toDisplayString(node->overrideValue());
            if (node->cachedDisplay().isEmpty())
                node->setCachedDisplay(TypeRenderer::toDisplayString(node->liveValue()));
            return node->cachedDisplay();
        }
        break;

    case Qt::EditRole:
        if (index.column() == 1)
            return QVariant::fromValue(
                node->isOverridden() ? node->overrideValue() : node->liveValue());
        break;

    case Qt::BackgroundRole:
        if (node->isOverridden())
            return QColor(255, 240, 180);
        break;

    case Qt::ToolTipRole:
        if (node->isOverridden())
            return QStringLiteral("Overridden — right-click to reset");
        break;

    case IsOverriddenRole:  return node->isOverridden();
    case PropertyPathRole:  return node->path();
    case RttrTypeRole:      return QVariant::fromValue(static_cast<quint32>(node->type().get_id()));
    case RttrVariantRole:
        return QVariant::fromValue(
            node->isOverridden() ? node->overrideValue() : node->liveValue());
    }
    return {};
}

bool PropertyModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (!index.isValid() || role != Qt::EditRole || index.column() != 1) return false;
    if (!value.canConvert<rttr::variant>()) return false;

    auto* node = static_cast<PropertyNode*>(index.internalPointer());
    const rttr::variant newVal = value.value<rttr::variant>();

    node->setOverrideValue(newVal);
    node->setOverridden(true);
    node->setCachedDisplay(TypeRenderer::toDisplayString(newVal));
    node->clearDirty();

    emit dataChanged(index, index.siblingAtColumn(columnCount() - 1));
    emit propertyEdited(node->path(), newVal);
    return true;
}

Qt::ItemFlags PropertyModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    auto* node = static_cast<PropertyNode*>(index.internalPointer());

    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!_readOnly && index.column() == 1 && node->children().isEmpty())
        f |= Qt::ItemIsEditable;
    return f;
}

QVariant PropertyModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return section == 0 ? QStringLiteral("Property") : QStringLiteral("Value");
}

// ── Internal helpers ──────────────────────────────────────────────────────────

PropertyNode* PropertyModel::_nodeFromIndex(const QModelIndex& idx) const
{
    return idx.isValid() ? static_cast<PropertyNode*>(idx.internalPointer()) : _root.get();
}

QModelIndex PropertyModel::_indexFromNode(PropertyNode* node, int column) const
{
    if (!node || node == _root.get()) return {};
    return createIndex(node->row(), column, node);
}

PropertyNode* PropertyModel::_findNode(const QString& path) const
{
    return _nodeByPath.value(path, nullptr);
}

} // namespace rpe

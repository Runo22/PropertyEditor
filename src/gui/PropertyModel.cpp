#include "rpe/gui/PropertyModel.h"

#include "rpe/core/EditorHints.h"
#include "rpe/core/RttrBridge.h"
#include "rpe/core/TypeRenderer.h"

#include <QBrush>
#include <QColor>
#include <QMetaObject>

#include <rttr/variant_sequential_view.h>

namespace rpe {

// ── metadata helpers ──────────────────────────────────────────────────────────
namespace {

// RTTR has no public default/invalid property constructor; querying a missing
// property on `void` yields a copyable invalid property we use as a sentinel.
rttr::property invalidProperty()
{
    return rttr::type::get<void>().get_property(std::string());
}

QString metaString(const rttr::property& p, const char* key)
{
    if (!p.is_valid()) return {};
    const rttr::variant m = p.get_metadata(key);
    if (!m.is_valid()) return {};
    bool ok = false;
    const std::string s = m.to_string(&ok);
    return ok ? QString::fromStdString(s) : QString();
}

bool metaBool(const rttr::property& p, const char* key, bool def)
{
    if (!p.is_valid()) return def;
    const rttr::variant m = p.get_metadata(key);
    if (!m.is_valid()) return def;
    return m.can_convert<bool>() ? m.to_bool() : def;
}

// Returns the metadata value as a QVariant(double) if present & numeric, else
// an invalid QVariant (so the delegate can apply its own default).
QVariant metaNumber(const rttr::property& p, const char* key)
{
    if (!p.is_valid()) return {};
    const rttr::variant m = p.get_metadata(key);
    if (!m.is_valid()) return {};
    bool ok = false;
    const double v = m.to_double(&ok);
    return ok ? QVariant(v) : QVariant();
}

} // namespace

// ── construction ───────────────────────────────────────────────────────────────

PropertyModel::PropertyModel(QObject* parent)
    : QAbstractItemModel(parent)
{
    qRegisterMetaType<rttr::variant>("rttr::variant");
    _resetRoot();
}

PropertyModel::~PropertyModel() = default;

void PropertyModel::_resetRoot()
{
    _root = std::make_unique<PropertyNode>(
        QStringLiteral("<root>"), QString(), rttr::type::get<void>(),
        invalidProperty(), nullptr);
    _nodeByPath.clear();
}

// ── schema ──────────────────────────────────────────────────────────────────────

void PropertyModel::bindType(rttr::type type)
{
    beginResetModel();
    _resetRoot();
    _boundType = type;
    if (type.is_valid())
        _buildTree(_root.get(), type, QString());
    _collectNodes(_root.get(), _nodeByPath);
    endResetModel();
}

void PropertyModel::unbind()
{
    beginResetModel();
    _resetRoot();
    _boundType = rttr::type::get<void>();
    endResetModel();
}

void PropertyModel::_buildTree(PropertyNode* parent, rttr::type type, const QString& prefix)
{
    for (auto& prop : TypeRenderer::rawType(type).get_properties()) {
        const QString name  = QString::fromStdString(prop.get_name().to_string());
        const QString path  = prefix.isEmpty() ? name : prefix + QLatin1Char('.') + name;
        const rttr::type pt = prop.get_type();

        auto* node = new PropertyNode(name, path, pt, prop, parent);
        node->setExpandable(TypeRenderer::isExpandable(pt));

        if (const QString label = metaString(prop, hint::Label); !label.isEmpty())
            node->setDisplayName(label);
        node->setTooltip(metaString(prop, hint::Tooltip));

        parent->children().append(node);

        // Nested structs are expanded at schema time; sequential containers are
        // expanded lazily on first refresh (size is data-dependent).
        if (!TypeRenderer::isSequential(pt) && TypeRenderer::isExpandable(pt))
            _buildTree(node, pt, path);
    }
}

void PropertyModel::_collectNodes(PropertyNode* node, QHash<QString, PropertyNode*>& out) const
{
    if (!node->path().isEmpty())
        out.insert(node->path(), node);
    for (auto* child : node->children())
        _collectNodes(child, out);
}

// ── refresh (hot path) ───────────────────────────────────────────────────────────

void PropertyModel::refresh(const rttr::instance& obj)
{
    if (!obj.is_valid() || !_root) return;
    for (auto* child : _root->children()) {
        const rttr::variant val = child->prop().get_value(obj);
        _refreshNode(child, val);
    }
    _emitDirtyRanges(_root.get());
}

void PropertyModel::_refreshNode(PropertyNode* node, const rttr::variant& valIn)
{
    if (node->isOverridden()) return;

    const rttr::variant val = TypeRenderer::unwrap(valIn);
    const rttr::type    t   = node->type();

    if (TypeRenderer::isSequential(t)) {
        node->setLiveValue(val);
        auto view    = val.create_sequential_view();
        const int sz = static_cast<int>(view.get_size());
        if (sz != node->arraySize()) {
            _rebuildArrayChildren(node, val);
        } else {
            for (int i = 0; i < sz; ++i) {
                auto* child = node->children()[i];
                if (child->isOverridden()) continue;
                _refreshNode(child, view.get_value(static_cast<size_t>(i)));
            }
        }
    } else if (TypeRenderer::isExpandable(t)) {
        node->setLiveValue(val);
        if (!val.is_valid()) return;
        rttr::variant  mutableParent = val;
        rttr::instance nested(mutableParent);
        for (auto* child : node->children())
            _refreshNode(child, child->prop().get_value(nested));
    } else {
        node->setLiveValue(val);
    }
}

void PropertyModel::_rebuildArrayChildren(PropertyNode* node, const rttr::variant& arrayVal)
{
    auto view       = arrayVal.create_sequential_view();
    const int newSz = static_cast<int>(view.get_size());
    const int oldSz = node->arraySize() < 0 ? node->children().size() : node->arraySize();

    const QModelIndex nodeIdx = _indexFromNode(node);

    if (oldSz > 0) {
        beginRemoveRows(nodeIdx, 0, oldSz - 1);
        for (auto* ch : node->children())
            _nodeByPath.remove(ch->path());
        qDeleteAll(node->children());
        node->children().clear();
        endRemoveRows();
    }

    if (newSz > 0) {
        beginInsertRows(nodeIdx, 0, newSz - 1);
        const rttr::type elemType = view.get_value_type();
        const bool       expand   = TypeRenderer::isExpandable(elemType);
        for (int i = 0; i < newSz; ++i) {
            const QString elemName = QStringLiteral("[%1]").arg(i);
            const QString elemPath = node->path() + QLatin1Char('.') + elemName;
            auto* elem = new PropertyNode(elemName, elemPath, elemType, invalidProperty(), node);
            elem->setArrayElement(true);
            elem->setExpandable(expand);
            node->children().append(elem);
            _nodeByPath.insert(elemPath, elem);
            // Static nested-struct rows belong to the inserted subtree and can be
            // built here. Values (and any nested *sequential* element, which would
            // start its own begin/endInsertRows) are populated below — never while
            // this insert is still open, which would violate the model protocol.
            if (expand && !TypeRenderer::isSequential(elemType))
                _buildTree(elem, elemType, elemPath);
        }
        endInsertRows();
    }
    node->setArraySize(newSz);

    // Populate element values after the insert window has closed.
    for (int i = 0; i < newSz; ++i)
        _refreshNode(node->children()[i], view.get_value(static_cast<size_t>(i)));
}

// ── thread-safe injection ────────────────────────────────────────────────────────

void PropertyModel::setPropertyValue(const QString& path, rttr::variant val)
{
    {
        QMutexLocker lk(&_pendingMutex);
        _pendingUpdates.insert(path, std::move(val));
    }
    if (!_flushScheduled.exchange(true, std::memory_order_acq_rel))
        QMetaObject::invokeMethod(this, &PropertyModel::_flushPending, Qt::QueuedConnection);
}

void PropertyModel::_flushPending()
{
    _flushScheduled.store(false, std::memory_order_release);
    QHash<QString, rttr::variant> batch;
    {
        QMutexLocker lk(&_pendingMutex);
        batch.swap(_pendingUpdates);
    }
    _applyBatch(batch);
}

void PropertyModel::_applyBatch(const QHash<QString, rttr::variant>& batch)
{
    for (auto it = batch.cbegin(); it != batch.cend(); ++it) {
        auto* node = _findNode(it.key());
        if (!node || node->isOverridden()) continue;
        node->setLiveValue(it.value());
    }
    _emitDirtyRanges(_root.get());
}

void PropertyModel::_emitDirtyRanges(PropertyNode* parent)
{
    auto& ch = parent->children();
    int   rangeStart = -1;

    auto flush = [&](int endExclusive) {
        if (rangeStart < 0) return;
        const QModelIndex parentIdx =
            (parent == _root.get()) ? QModelIndex() : _indexFromNode(parent);
        emit dataChanged(index(rangeStart, 0, parentIdx),
                         index(endExclusive - 1, columnCount() - 1, parentIdx));
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

// ── override / reset ───────────────────────────────────────────────────────────

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
        if (node->isOverridden()) { node->setOverridden(false); node->clearDirty(); any = true; }
    }
    if (any && !_root->children().isEmpty())
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
}

bool PropertyModel::hasAnyOverride() const
{
    for (auto* node : std::as_const(_nodeByPath))
        if (node->isOverridden()) return true;
    return false;
}

// ── editing ────────────────────────────────────────────────────────────────────

void PropertyModel::setReadOnly(bool ro)
{
    if (_readOnly == ro) return;
    _readOnly = ro;
    if (!_root->children().isEmpty())
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1),
                         {Qt::DisplayRole});
}

void PropertyModel::setInstanceProvider(std::function<rttr::instance()> provider)
{
    _instanceProvider = std::move(provider);
}

void PropertyModel::setWriteGuard(AccessGuard guard)
{
    _writeGuard = std::move(guard);
}

void PropertyModel::setEditSink(std::function<void(const QString&, const rttr::variant&)> sink)
{
    _editSink = std::move(sink);
}

QStringList PropertyModel::allLeafPaths() const
{
    QStringList out;
    for (auto it = _nodeByPath.cbegin(); it != _nodeByPath.cend(); ++it)
        if (it.value()->isLeaf())
            out.append(it.key());
    return out;
}

bool PropertyModel::_applyEdit(PropertyNode* node, const rttr::variant& newVal)
{
    if (_editSink) {
        // Mirror mode: hand the edit off, show it optimistically, and resume
        // live updates (the sim thread will echo the applied value back).
        node->setOverridden(false);
        node->setLiveValue(newVal);
        _editSink(node->path(), newVal);
        emit propertyEdited(node->path(), newVal);
        return true;
    }


    if (_editPolicy == EditPolicy::WriteBack && _instanceProvider) {
        bool          wrote = false;
        rttr::variant actual;
        // The provider may hand back a pointer into data owned by another
        // thread; the whole provider+write+read-back sequence runs under the
        // guard so it can't race the owner.
        withGuard(_writeGuard, [&] {
            rttr::instance inst = _instanceProvider();
            if (inst.is_valid() && bridge::setValueByPath(inst, node->path(), newVal)) {
                // Read back so the display reflects any coercion the object applied.
                actual = bridge::getValueByPath(inst, node->path());
                wrote  = true;
            }
        });
        if (wrote) {
            node->setOverridden(false);
            node->setLiveValue(actual.is_valid() ? actual : newVal);
            emit propertyEdited(node->path(), node->liveValue());
            return true;
        }
        // Write failed — fall through to override so the edit is not lost.
    }

    node->setOverrideValue(newVal);
    node->setOverridden(true);
    emit propertyEdited(node->path(), newVal);
    return true;
}

// ── QAbstractItemModel ───────────────────────────────────────────────────────────

QModelIndex PropertyModel::index(int row, int column, const QModelIndex& parent) const
{
    if (!hasIndex(row, column, parent)) return {};
    const PropertyNode* p = parent.isValid()
        ? static_cast<const PropertyNode*>(parent.internalPointer()) : _root.get();
    if (!p || row >= p->children().size()) return {};
    return createIndex(row, column, p->children()[row]);
}

QModelIndex PropertyModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) return {};
    auto* node = static_cast<PropertyNode*>(child.internalPointer());
    auto* p    = node ? node->parent() : nullptr;
    if (!p || p == _root.get()) return {};
    return createIndex(p->row(), 0, p);
}

int PropertyModel::rowCount(const QModelIndex& parent) const
{
    const PropertyNode* p = parent.isValid()
        ? static_cast<const PropertyNode*>(parent.internalPointer()) : _root.get();
    return p ? static_cast<int>(p->children().size()) : 0;
}

int PropertyModel::columnCount(const QModelIndex&) const { return 2; }

QVariant PropertyModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) return {};
    auto* node = static_cast<PropertyNode*>(index.internalPointer());

    switch (role) {
    case Qt::DisplayRole:
        if (index.column() == 0) return node->displayName();
        if (index.column() == 1) {
            if (node->cachedDisplay().isEmpty())
                node->setCachedDisplay(TypeRenderer::toDisplayString(node->effectiveValue()));
            return node->cachedDisplay();
        }
        break;

    case Qt::EditRole:
        if (index.column() == 1)
            return QVariant::fromValue(node->effectiveValue());
        break;

    case Qt::DecorationRole:
        // QColor leaves get a swatch via Qt's standard decoration handling.
        // Use the raw/unwrapped type so wrapped QColor properties match too.
        if (index.column() == 1
            && TypeRenderer::rawType(node->type()) == rttr::type::get<QColor>()) {
            const rttr::variant v = TypeRenderer::unwrap(node->effectiveValue());
            if (v.is_valid() && v.get_type() == rttr::type::get<QColor>())
                return v.get_value<QColor>();
        }
        break;

    case Qt::ToolTipRole:
        if (node->isOverridden())
            return QStringLiteral("Overridden — right-click to reset to live");
        if (!node->tooltip().isEmpty())
            return node->tooltip();
        break;

    case Qt::ForegroundRole:
        if (node->isOverridden())
            return QBrush(QColor(0xE5, 0x9A, 0x2E));   // amber for pinned values
        break;

    case IsOverriddenRole: return node->isOverridden();
    case PropertyPathRole: return node->path();
    case IsLeafRole:       return node->isLeaf();
    case RttrVariantRole:  return QVariant::fromValue(node->effectiveValue());
    case EditorHintRole:   return metaString(node->prop(), hint::Editor);
    case MinRole:          return metaNumber(node->prop(), hint::Min);
    case MaxRole:          return metaNumber(node->prop(), hint::Max);
    case StepRole:         return metaNumber(node->prop(), hint::Step);
    case DecimalsRole: {
        const QVariant d = metaNumber(node->prop(), hint::Decimals);
        return d.isValid() ? QVariant(static_cast<int>(d.toDouble())) : QVariant();
    }
    }
    return {};
}

bool PropertyModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (!index.isValid() || role != Qt::EditRole || index.column() != 1) return false;
    if (!value.canConvert<rttr::variant>()) return false;

    auto* node = static_cast<PropertyNode*>(index.internalPointer());
    const rttr::variant newVal = value.value<rttr::variant>();
    if (!newVal.is_valid()) return false;

    if (!_applyEdit(node, newVal)) return false;

    node->setCachedDisplay(TypeRenderer::toDisplayString(node->effectiveValue()));
    node->clearDirty();
    emit dataChanged(index.siblingAtColumn(0), index.siblingAtColumn(columnCount() - 1));
    return true;
}

Qt::ItemFlags PropertyModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    auto* node = static_cast<PropertyNode*>(index.internalPointer());

    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    const bool forcedRO = metaBool(node->prop(), hint::ReadOnly, false);
    if (!_readOnly && !forcedRO && index.column() == 1 && node->isLeaf()
        && TypeRenderer::isInlineEditable(node->type()))
        f |= Qt::ItemIsEditable;
    return f;
}

QVariant PropertyModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return section == 0 ? QStringLiteral("Property") : QStringLiteral("Value");
}

// ── helpers ──────────────────────────────────────────────────────────────────────

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

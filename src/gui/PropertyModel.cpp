#include "rpe/gui/PropertyModel.h"

#include "rpe/core/EditorHints.h"
#include "rpe/core/RttrBridge.h"
#include "rpe/core/TypeRenderer.h"

#include <QBrush>
#include <QColor>
#include <QMetaObject>

#include <rttr/variant_sequential_view.h>

namespace rpe
{

    // ── metadata helpers ──────────────────────────────────────────────────────────
    namespace
    {

        // RTTR has no public default/invalid property constructor; querying a missing
        // property on `void` yields a copyable invalid property we use as a sentinel.
        rttr::property invalidProperty()
        {
            return rttr::type::get<void>().get_property(std::string());
        }

        QString metaString(const rttr::property& p, const char* key)
        {
            if (!p.is_valid())
            {
                return {};
            }
            const rttr::variant m = p.get_metadata(key);
            if (!m.is_valid())
            {
                return {};
            }
            bool ok = false;
            const std::string s = m.to_string(&ok);
            return ok ? QString::fromStdString(s) : QString();
        }

        bool metaBool(const rttr::property& p, const char* key, bool def)
        {
            if (!p.is_valid())
            {
                return def;
            }
            const rttr::variant m = p.get_metadata(key);
            if (!m.is_valid())
            {
                return def;
            }
            return m.can_convert<bool>() ? m.to_bool() : def;
        }

        // Returns the metadata value as a QVariant(double) if present & numeric, else
        // an invalid QVariant (so the delegate can apply its own default).
        QVariant metaNumber(const rttr::property& p, const char* key)
        {
            if (!p.is_valid())
            {
                return {};
            }
            const rttr::variant m = p.get_metadata(key);
            if (!m.is_valid())
            {
                return {};
            }
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
            QStringLiteral("<root>"), QString(), rttr::type::get<void>(), invalidProperty(), nullptr);
        _nodeByPath.clear();
    }

    // ── schema ──────────────────────────────────────────────────────────────────────

    void PropertyModel::bindType(rttr::type type)
    {
        beginResetModel();
        _resetRoot();
        _boundType = type;
        if (type.is_valid())
        {
            _buildTree(_root.get(), type, QString());
        }
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
        for (auto& prop : TypeRenderer::rawType(type).get_properties())
        {
            const QString name = QString::fromStdString(prop.get_name().to_string());
            const QString path = prefix.isEmpty() ? name : prefix + QLatin1Char('.') + name;
            const rttr::type pt = prop.get_type();

            auto* node = new PropertyNode(name, path, pt, prop, parent);
            node->setExpandable(TypeRenderer::isExpandable(pt));

            if (const QString label = metaString(prop, hint::Label); !label.isEmpty())
            {
                node->setDisplayName(label);
            }
            node->setTooltip(metaString(prop, hint::Tooltip));

            parent->children().append(node);

            // Nested structs are expanded at schema time; sequential containers are
            // expanded lazily on first refresh (size is data-dependent).
            if (!TypeRenderer::isSequential(pt) && TypeRenderer::isExpandable(pt))
            {
                _buildTree(node, pt, path);
            }
        }
    }

    void PropertyModel::_collectNodes(PropertyNode* node, QHash<QString, PropertyNode*>& out) const
    {
        if (!node->path().isEmpty())
        {
            out.insert(node->path(), node);
        }
        for (auto* child : node->children())
        {
            _collectNodes(child, out);
        }
    }

    // ── refresh (hot path) ───────────────────────────────────────────────────────────

    void PropertyModel::refresh(const rttr::instance& obj)
    {
        if (!obj.is_valid() || !_root)
        {
            return;
        }
        for (auto* child : _root->children())
        {
            const rttr::variant val = child->prop().get_value(obj);
            _refreshNode(child, val);
        }
        _emitDirtyRanges(_root.get());
    }

    void PropertyModel::_refreshNode(PropertyNode* node, const rttr::variant& valIn)
    {
        if (node->hasLocalEdit())
        {
            return;
        }

        const rttr::variant val = TypeRenderer::unwrap(valIn);
        const rttr::type t = node->type();

        if (TypeRenderer::isSequential(t))
        {
            _refreshSequential(node, val);
        }
        else if (TypeRenderer::isExpandable(t))
        {
            node->setLiveValue(val);
            if (!val.is_valid())
            {
                return;
            }
            rttr::variant mutableParent = val;
            rttr::instance nested(mutableParent);
            for (auto* child : node->children())
            {
                _refreshNode(child, child->prop().get_value(nested));
            }
        }
        else
        {
            node->setLiveValue(val);
        }
    }

    // Sync an array node's element rows to a sequential value: rebuild the child
    // rows when the size changed, otherwise refresh each element in place. Shared by
    // the schema refresh() path AND the mirror injection path (setPropertyValue),
    // so arrays display and expand in mirror mode too — where refresh() never runs.
    void PropertyModel::_refreshSequential(PropertyNode* node, const rttr::variant& val)
    {
        node->setLiveValue(val);
        if (!val.is_valid() || !val.is_sequential_container())
        {
            return;
        }
        auto view = val.create_sequential_view();
        const int sz = static_cast<int>(view.get_size());
        if (sz != node->arraySize())
        {
            // A resize tears down and rebuilds the element rows (qDeleteAll). If any
            // element (or a field of one) currently has an open inline editor, its
            // node is pinned and about to be freed — deleting it would destroy the
            // live editor and dangle its QModelIndex. Defer the rebuild until the
            // edit finishes; a later refresh/injection (the size still differs) redoes
            // it. The stale row count is transient and harmless.
            if (_anyDescendantLocallyEdited(node))
            {
                return;
            }
            _rebuildArrayChildren(node, val);
        }
        else
        {
            for (int i = 0; i < sz; ++i)
            {
                auto* child = node->children()[i];
                if (child->hasLocalEdit())
                {
                    continue;
                }
                _refreshNode(child, view.get_value(static_cast<size_t>(i)));
            }
        }
    }

    bool PropertyModel::_anyDescendantLocallyEdited(const PropertyNode* node)
    {
        for (const auto* child : node->children())
        {
            if (child->hasLocalEdit() || _anyDescendantLocallyEdited(child))
            {
                return true;
            }
        }
        return false;
    }

    void PropertyModel::_rebuildArrayChildren(PropertyNode* node, const rttr::variant& arrayVal)
    {
        auto view = arrayVal.create_sequential_view();
        const int newSz = static_cast<int>(view.get_size());
        const int oldSz = node->arraySize() < 0 ? node->children().size() : node->arraySize();

        const QModelIndex nodeIdx = _indexFromNode(node);

        if (oldSz > 0)
        {
            beginRemoveRows(nodeIdx, 0, oldSz - 1);
            for (auto* ch : node->children())
            {
                _nodeByPath.remove(ch->path());
            }
            qDeleteAll(node->children());
            node->children().clear();
            endRemoveRows();
        }

        if (newSz > 0)
        {
            beginInsertRows(nodeIdx, 0, newSz - 1);
            const rttr::type elemType = view.get_value_type();
            const bool expand = TypeRenderer::isExpandable(elemType);
            for (int i = 0; i < newSz; ++i)
            {
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
                {
                    _buildTree(elem, elemType, elemPath);
                }
            }
            endInsertRows();
        }
        node->setArraySize(newSz);

        // Populate element values after the insert window has closed.
        for (int i = 0; i < newSz; ++i)
        {
            _refreshNode(node->children()[i], view.get_value(static_cast<size_t>(i)));
        }
    }

    // ── thread-safe injection ────────────────────────────────────────────────────────

    void PropertyModel::setPropertyValue(const QString& path, rttr::variant val)
    {
        {
            QMutexLocker lk(&_pendingMutex);
            _pendingUpdates.insert(path, std::move(val));
        }
        if (!_flushScheduled.exchange(true, std::memory_order_acq_rel))
        {
            QMetaObject::invokeMethod(this, &PropertyModel::_flushPending, Qt::QueuedConnection);
        }
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
        for (auto it = batch.cbegin(); it != batch.cend(); ++it)
        {
            auto* node = _findNode(it.key());
            if (!node || node->hasLocalEdit())
            {
                continue;
            }
            // Route mirror injections through the same dispatch refresh() uses, so an
            // injected array builds its element rows and an injected struct refreshes
            // its children — identical behaviour on both paths (and no divergence as
            // new node kinds are added). Scalars just cache their (unwrapped) value.
            _refreshNode(node, it.value());
        }
        _emitDirtyRanges(_root.get());
    }

    void PropertyModel::_emitDirtyRanges(PropertyNode* parent)
    {
        auto& ch = parent->children();
        int rangeStart = -1;

        auto flush = [&](int endExclusive) {
            if (rangeStart < 0)
            {
                return;
            }
            const QModelIndex parentIdx = (parent == _root.get()) ? QModelIndex() : _indexFromNode(parent);
            emit dataChanged(index(rangeStart, 0, parentIdx), index(endExclusive - 1, columnCount() - 1, parentIdx));
            rangeStart = -1;
        };

        for (int i = 0; i < ch.size(); ++i)
        {
            auto* node = ch[i];
            if (node->isDirty())
            {
                node->clearDirty();
                if (rangeStart < 0)
                {
                    rangeStart = i;
                }
            }
            else
            {
                flush(i);
            }
            if (!node->children().isEmpty())
            {
                _emitDirtyRanges(node);
            }
        }
        flush(static_cast<int>(ch.size()));
    }

    // ── local edit / reset ────────────────────────────────────────────────────────────

    void PropertyModel::beginLocalEdit(const QString& path)
    {
        auto* node = _findNode(path);
        if (!node || node->hasLocalEdit())
        {
            return;
        }
        node->setLocalEdit(true);
        node->setLocalEditValue(node->liveValue());
        const auto idx = _indexFromNode(node);
        emit dataChanged(idx, idx.siblingAtColumn(columnCount() - 1));
    }

    void PropertyModel::resetNode(const QString& path)
    {
        auto* node = _findNode(path);
        if (!node || !node->hasLocalEdit())
        {
            return;
        }
        node->setLocalEdit(false);
        const auto idx = _indexFromNode(node);
        emit dataChanged(idx, idx.siblingAtColumn(columnCount() - 1));
    }

    void PropertyModel::resetAll()
    {
        bool any = false;
        for (auto* node : std::as_const(_nodeByPath))
        {
            if (node->hasLocalEdit())
            {
                node->setLocalEdit(false);
                node->clearDirty();
                any = true;
            }
        }
        if (any && !_root->children().isEmpty())
        {
            emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
        }
    }

    bool PropertyModel::hasAnyLocalEdit() const
    {
        for (auto* node : std::as_const(_nodeByPath))
        {
            if (node->hasLocalEdit())
            {
                return true;
            }
        }
        return false;
    }

    // ── editing ────────────────────────────────────────────────────────────────────

    void PropertyModel::setReadOnly(bool ro)
    {
        if (_readOnly == ro)
        {
            return;
        }
        _readOnly = ro;
        if (!_root->children().isEmpty())
        {
            emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), { Qt::DisplayRole });
        }
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
        {
            if (it.value()->isLeaf())
            {
                out.append(it.key());
            }
        }
        return out;
    }

    bool PropertyModel::_applyEdit(PropertyNode* node, const rttr::variant& newVal)
    {
        if (_editSink)
        {
            // Mirror mode: hand the edit off, show it optimistically, and resume
            // live updates (the sim thread will echo the applied value back).
            node->setLocalEdit(false);
            node->setLiveValue(newVal);
            _editSink(node->path(), newVal);
            emit propertyEdited(node->path(), newVal);
            return true;
        }

        if (_editPolicy == EditPolicy::WriteBack && _instanceProvider)
        {
            bool wrote = false;
            rttr::variant actual;
            // The provider may hand back a pointer into data owned by another
            // thread; the whole provider+write+read-back sequence runs under the
            // guard so it can't race the owner.
            withGuard(_writeGuard, [&] {
                rttr::instance inst = _instanceProvider();
                if (inst.is_valid() && bridge::setValueByPath(inst, node->path(), newVal))
                {
                    // Read back so the display reflects any coercion the object applied.
                    actual = bridge::getValueByPath(inst, node->path());
                    wrote = true;
                }
            });
            if (wrote)
            {
                node->setLocalEdit(false);
                node->setLiveValue(actual.is_valid() ? actual : newVal);
                emit propertyEdited(node->path(), node->liveValue());
                return true;
            }
            // Write failed — fall through to a local draft so the edit is not lost.
        }

        node->setLocalEditValue(newVal);
        node->setLocalEdit(true);
        emit propertyEdited(node->path(), newVal);
        return true;
    }

    // ── QAbstractItemModel ───────────────────────────────────────────────────────────

    QModelIndex PropertyModel::index(int row, int column, const QModelIndex& parent) const
    {
        if (!hasIndex(row, column, parent))
        {
            return {};
        }
        const PropertyNode* p = parent.isValid()
            ? static_cast<const PropertyNode*>(parent.internalPointer())
            : _root.get();
        if (!p || row >= p->children().size())
        {
            return {};
        }
        return createIndex(row, column, p->children()[row]);
    }

    QModelIndex PropertyModel::parent(const QModelIndex& child) const
    {
        if (!child.isValid())
        {
            return {};
        }
        auto* node = static_cast<PropertyNode*>(child.internalPointer());
        auto* p = node ? node->parent() : nullptr;
        if (!p || p == _root.get())
        {
            return {};
        }
        return createIndex(p->row(), 0, p);
    }

    int PropertyModel::rowCount(const QModelIndex& parent) const
    {
        const PropertyNode* p = parent.isValid()
            ? static_cast<const PropertyNode*>(parent.internalPointer())
            : _root.get();
        return p ? static_cast<int>(p->children().size()) : 0;
    }

    int PropertyModel::columnCount(const QModelIndex&) const
    {
        return 2;
    }

    QVariant PropertyModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid())
        {
            return {};
        }
        auto* node = static_cast<PropertyNode*>(index.internalPointer());

        switch (role)
        {
        case Qt::DisplayRole:
            if (index.column() == 0)
            {
                return node->displayName();
            }
            if (index.column() == 1)
            {
                // Expandable rows (structs, arrays) show no scalar value of their own
                // — the children carry the values. Show an element count for sized
                // arrays, else nothing. Avoids a bare "<invalid>" on the parent,
                // especially in mirror mode where only leaf values are mirrored.
                if (node->isExpandable())
                {
                    return node->arraySize() >= 0 ? QStringLiteral("[%1]").arg(node->arraySize()) : QString();
                }
                // Leaf with no value yet (e.g. not mirrored): blank, not "<invalid>".
                const rttr::variant v = node->effectiveValue();
                if (!v.is_valid())
                {
                    return QString();
                }
                if (node->cachedDisplay().isEmpty())
                {
                    node->setCachedDisplay(TypeRenderer::toDisplayString(v));
                }
                return node->cachedDisplay();
            }
            break;

        case Qt::EditRole:
            if (index.column() == 1)
            {
                return QVariant::fromValue(node->effectiveValue());
            }
            break;

        case Qt::DecorationRole:
            // QColor leaves get a swatch via Qt's standard decoration handling.
            // Use the raw/unwrapped type so wrapped QColor properties match too.
            if (index.column() == 1
                && TypeRenderer::rawType(node->type()) == rttr::type::get<QColor>())
            {
                const rttr::variant v = TypeRenderer::unwrap(node->effectiveValue());
                if (v.is_valid() && v.get_type() == rttr::type::get<QColor>())
                {
                    return v.get_value<QColor>();
                }
            }
            break;

        case Qt::ToolTipRole:
            if (node->hasLocalEdit())
            {
                return QStringLiteral("Local edit (draft, not applied) — right-click to reset to live");
            }
            if (!node->tooltip().isEmpty())
            {
                return node->tooltip();
            }
            break;

        case Qt::ForegroundRole:
            if (node->hasLocalEdit())
            {
                return QBrush(QColor(0xE5, 0x9A, 0x2E)); // amber for pinned values
            }
            break;

        case HasLocalEditRole:
            return node->hasLocalEdit();
        case PropertyPathRole:
            return node->path();
        case IsLeafRole:
            return node->isLeaf();
        case RttrVariantRole:
            return QVariant::fromValue(node->effectiveValue());
        case EditorHintRole:
            return metaString(node->prop(), hint::Editor);
        case MinRole:
            return metaNumber(node->prop(), hint::Min);
        case MaxRole:
            return metaNumber(node->prop(), hint::Max);
        case StepRole:
            return metaNumber(node->prop(), hint::Step);
        case DecimalsRole:
        {
            const QVariant d = metaNumber(node->prop(), hint::Decimals);
            return d.isValid() ? QVariant(static_cast<int>(d.toDouble())) : QVariant();
        }
        case IsArrayRole:
            return node->arraySize() >= 0;
        case DeclaredTypeRole:
            // The node's schema type, always valid — unlike the live value, which may
            // be absent (not yet mirrored) or a transient editor type mid-edit. The
            // delegate picks the editor from this so a path/enum/number cell always
            // gets the right editor.
            return QVariant::fromValue(rttr::variant(node->type()));
        }
        return {};
    }

    bool PropertyModel::setData(const QModelIndex& index, const QVariant& value, int role)
    {
        if (!index.isValid() || role != Qt::EditRole || index.column() != 1)
        {
            return false;
        }
        if (!value.canConvert<rttr::variant>())
        {
            return false;
        }

        auto* node = static_cast<PropertyNode*>(index.internalPointer());
        const rttr::variant newVal = value.value<rttr::variant>();
        if (!newVal.is_valid())
        {
            return false;
        }

        if (!_applyEdit(node, newVal))
        {
            return false;
        }

        node->setCachedDisplay(TypeRenderer::toDisplayString(node->effectiveValue()));
        node->clearDirty();
        emit dataChanged(index.siblingAtColumn(0), index.siblingAtColumn(columnCount() - 1));
        return true;
    }

    Qt::ItemFlags PropertyModel::flags(const QModelIndex& index) const
    {
        if (!index.isValid())
        {
            return Qt::NoItemFlags;
        }
        auto* node = static_cast<PropertyNode*>(index.internalPointer());

        Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        const bool forcedRO = metaBool(node->prop(), hint::ReadOnly, false);
        if (!_readOnly && !forcedRO && index.column() == 1 && node->isLeaf()
            && TypeRenderer::isInlineEditable(node->type()))
        {
            f |= Qt::ItemIsEditable;
        }
        return f;
    }

    QVariant PropertyModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        {
            return {};
        }
        return section == 0 ? QStringLiteral("Property") : QStringLiteral("Value");
    }

    // ── helpers ──────────────────────────────────────────────────────────────────────

    QModelIndex PropertyModel::_indexFromNode(PropertyNode* node, int column) const
    {
        if (!node || node == _root.get())
        {
            return {};
        }
        return createIndex(node->row(), column, node);
    }

    PropertyNode* PropertyModel::_findNode(const QString& path) const
    {
        return _nodeByPath.value(path, nullptr);
    }

} // namespace rpe

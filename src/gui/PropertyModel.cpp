#include "rpe/gui/PropertyModel.h"
#include "rpe/core/NamedElement.h"

#include "rpe/core/ReadOnly.h"
#include "rpe/core/TypeBridge.h"

#include "rpe/core/EditorHints.h"
#include "rpe/core/RttrBridge.h"
#include "rpe/core/TypeRelease.h"
#include "rpe/core/TypeRenderer.h"

#include <QBrush>
#include <QColor>
#include <QMetaObject>

#include <algorithm>

#include <rttr/variant_associative_view.h>
#include <rttr/variant_sequential_view.h>

namespace rpe
{

    // ── metadata helpers ──────────────────────────────────────────────────────────
    namespace
    {

        // RTTR has no public default/invalid property constructor; querying a missing
        // property on `void` yields a copyable invalid property we use as a sentinel.
        // Joins a group key to a row path (grouped view). A control character: it can't
        // occur in a component key or a property path.
        constexpr QChar kGroupSep(0x1F);

        rttr::property invalidProperty()
        {
            return rttr::type::get<void>().get_property(std::string());
        }

        QString metaString(const rttr::property& p, hint::Key key)
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
            // A `const char*` VARIABLE — every rpe::editor::* constant — is stored as
            // a const char*, which RTTR's to_string() can't convert (a string LITERAL
            // is stored as std::string and converts fine). Read it directly, or
            // metadata(hint::Editor, rpe::editor::Color) silently means nothing.
            if (m.get_type() == rttr::type::get<const char*>())
            {
                const char* c = m.get_value<const char*>();
                return c ? QString::fromUtf8(c) : QString();
            }
            bool ok = false;
            const std::string s = m.to_string(&ok);
            return ok ? QString::fromStdString(s) : QString();
        }

        bool metaBool(const rttr::property& p, hint::Key key, bool def)
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
        QVariant metaNumber(const rttr::property& p, hint::Key key)
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
        _namedGen = TypeBridge::namedElementGeneration();
        _resetRoot();
        _releaseHandle = addTypeReleaseHandler(
            [this](const std::vector<rttr::type>& released) { _onTypesReleased(released); });
    }

    PropertyModel::~PropertyModel()
    {
        removeTypeReleaseHandler(_releaseHandle);
        TypeUseScope scope; // the tree's destructor may run a plugin's variant code
        _root.reset();
    }

    namespace
    {
        bool treeUses(const PropertyNode* node, const std::vector<rttr::type>& released)
        {
            if (isReleased(node->type(), released))
            {
                return true;
            }
            for (const PropertyNode* c : node->children())
            {
                if (treeUses(c, released))
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    void PropertyModel::_onTypesReleased(const std::vector<rttr::type>& released)
    {
        // Any thread; no other thread is inside a TypeUseScope. The released module
        // is still loaded, so reading its types (and destroying its values) is fine
        // HERE — and never again after this returns.
        if (!isReleased(_boundType, released) && !treeUses(_root.get(), released))
        {
            return;
        }
        // A grouped view has no single bound type (its _boundType is void): no name.
        _releasedTypeName = _grouped ? QString() : QString::fromStdString(_boundType.get_name().to_string());
        _root->releaseReflection(); // keeps the rows (views hold indexes), empties them
        _boundType = rttr::type::get<void>();
        {
            QMutexLocker lk(&_pendingMutex);
            _pendingUpdates.clear();
        }
        _released.store(true, std::memory_order_release);
        // Resetting the model (signals, views) belongs on its own thread.
        QMetaObject::invokeMethod(this, [this] { _finishRelease(); }, Qt::QueuedConnection);
    }

    void PropertyModel::_finishRelease()
    {
        if (!_released.exchange(false))
        {
            return;
        }
        unbind();
        emit boundTypeReleased(_releasedTypeName);
    }

    void PropertyModel::_resetRoot()
    {
        _root = std::make_unique<PropertyNode>(
            QStringLiteral("<root>"), QString(), rttr::type::get<void>(), invalidProperty(), nullptr);
        _nodeByPath.clear();
    }

    // ── schema ──────────────────────────────────────────────────────────────────────

    void PropertyModel::bindType(rttr::type type)
    {
        TypeUseScope scope;
        _released.store(false); // a fresh tree supersedes a pending release
        beginResetModel();
        _roCache.clear();
        _resetRoot();
        _grouped = false;
        _groupKeys.clear();
        _groupCounts.clear();
        _editorPinPath.clear();
        _boundType = type;
        _expandedPaths.clear();
        if (type.is_valid())
        {
            _buildTree(_root.get(), type, QString());
        }
        _collectNodes(_root.get(), _nodeByPath);
        endResetModel();
    }

    void PropertyModel::unbind()
    {
        TypeUseScope scope;
        beginResetModel();
        _roCache.clear();
        _resetRoot();
        _grouped = false;
        _groupKeys.clear();
        _groupCounts.clear();
        _editorPinPath.clear();
        _boundType = rttr::type::get<void>();
        _expandedPaths.clear();
        endResetModel();
    }

    // ── grouped view ────────────────────────────────────────────────────────────────

    QString PropertyModel::groupPath(const QString& key, const QString& relPath)
    {
        return key + kGroupSep + relPath;
    }

    bool PropertyModel::splitGroupPath(const QString& path, QString* key, QString* relPath)
    {
        const int at = path.indexOf(kGroupSep);
        if (at < 0)
        {
            if (key)
                key->clear();
            if (relPath)
                *relPath = path;
            return false;
        }
        if (key)
            *key = path.left(at);
        if (relPath)
            *relPath = path.mid(at + 1);
        return true;
    }

    void PropertyModel::bindGroups(const QVector<Group>& groups)
    {
        TypeUseScope scope;
        _released.store(false);
        beginResetModel();
        _roCache.clear();
        _resetRoot();
        _boundType = rttr::type::get<void>();
        _expandedPaths.clear();
        _grouped = true;
        _groupKeys.clear();
        _groupCounts.clear();
        _editorPinPath.clear();
        for (const Group& g : groups)
        {
            // The header's path is "key<sep>", so its rows' paths come out as
            // groupPath(key, "position.x").
            auto* header = new PropertyNode(g.label, g.key + kGroupSep, g.type, invalidProperty(), _root.get());
            header->setExpandable(true);
            _root->children().append(header);
            _groupKeys.insert(header, g.key);
            if (g.type.is_valid() && g.type != rttr::type::get<void>())
            {
                _buildTree(header, g.type, header->path());
            }
        }
        _collectNodes(_root.get(), _nodeByPath);
        endResetModel();
    }

    void PropertyModel::setGroupInstanceProvider(std::function<rttr::instance(const QString&)> provider)
    {
        _groupInstanceProvider = std::move(provider);
    }

    void PropertyModel::setGroupCounts(const QHash<QString, int>& counts)
    {
        if (counts == _groupCounts)
        {
            return;
        }
        _groupCounts = counts;
        for (int r = 0; _grouped && r < _root->children().size(); ++r)
        {
            const QModelIndex i = createIndex(r, 0, _root->children()[r]);
            emit dataChanged(i, i, { Qt::DisplayRole, GroupCountRole });
        }
    }

    void PropertyModel::clearValues(const QString& path)
    {
        TypeUseScope scope;
        PropertyNode* node = _findNode(path);
        if (!node)
        {
            return;
        }
        std::function<void(PropertyNode*)> clear = [&](PropertyNode* n) {
            if (!n->hasLocalEdit())
            {
                n->setLiveValue(rttr::variant());
            }
            for (PropertyNode* c : n->children())
            {
                clear(c);
            }
        };
        clear(node);
        _emitDirtyRanges(_root.get());
    }

    QModelIndex PropertyModel::groupIndex(const QString& key) const
    {
        for (int r = 0; _grouped && r < _root->children().size(); ++r)
        {
            PropertyNode* n = _root->children()[r];
            if (_groupKeys.value(n) == key)
            {
                return createIndex(r, 0, n);
            }
        }
        return {};
    }

    QModelIndex PropertyModel::indexForPath(const QString& path) const
    {
        PropertyNode* n = _findNode(path);
        return n ? _indexFromNode(n, 0) : QModelIndex();
    }

    const PropertyNode* PropertyModel::_groupOf(const PropertyNode* node) const
    {
        if (!_grouped || !node || node == _root.get())
        {
            return nullptr;
        }
        while (node->parent() && node->parent() != _root.get())
        {
            node = node->parent();
        }
        return node;
    }

    void PropertyModel::_buildTree(PropertyNode* parent, rttr::type type, const QString& prefix)
    {
        for (auto& prop : TypeRenderer::rawType(type).get_properties())
        {
            const QString name = QString::fromStdString(prop.get_name().to_string());
            // A group header's path ends in the separator: its rows join without a dot.
            const QString path = (prefix.isEmpty() || prefix.endsWith(kGroupSep)) ? prefix + name
                                                                                   : prefix + QLatin1Char('.') + name;
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
        for (auto* field : { node->rowLabel(), node->rowValue() })
        {
            if (field)
            {
                _collectNodes(field, out);
            }
        }
    }

    void PropertyModel::_forgetNodes(PropertyNode* node)
    {
        for (auto* child : node->children())
        {
            _forgetNodes(child);
        }
        for (auto* field : { node->rowLabel(), node->rowValue() })
        {
            if (field)
            {
                _forgetNodes(field);
            }
        }
        _nodeByPath.remove(node->path());
    }

    // ── refresh (hot path) ───────────────────────────────────────────────────────────

    void PropertyModel::refresh(const rttr::instance& obj)
    {
        TypeUseScope scope; // see rpe/core/TypeRelease.h
        syncNamedElements();
        if (!obj.is_valid() || !_root || _grouped) // a grouped view is fed by path

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
        else if (TypeRenderer::isAssociative(t))
        {
            _refreshAssociative(node, val);
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
            // A named element's own fields. Its name comes from the source as text
            // when the source gave it (the mirror does: a string_view / const char*
            // name points into simulation memory) — otherwise it's read here.
            if (PropertyNode* value = node->rowValue())
            {
                _refreshNode(value, value->prop().get_value(nested));
            }
            if (PropertyNode* label = node->rowLabel(); label && !label->hasLocalEdit())
            {
                const QStringList given = node->parent() ? _givenLabels.value(node->parent()->path()) : QStringList();
                const int i = node->row();
                label->setLiveValue(i >= 0 && i < given.size() ? rttr::variant(given[i]) : label->prop().get_value(nested));
                _syncNamedRow(node);
            }
        }
        else
        {
            node->setLiveValue(val);
            if (node->isRowField() && node == node->parent()->rowLabel())
            {
                _syncNamedRow(node->parent()); // the name alone came in (a reorder)
            }
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

    // Sync an associative node's rows to a map value: one child row per key,
    // named "[key]" (displayed as the bare key), value refreshed/edited exactly
    // like an array element. Rows are sorted by key display string so
    // unordered_map doesn't reshuffle the UI between refreshes. Key-only
    // containers (std::set) show just their count. Same local-edit guard and
    // rebuild-vs-in-place split as _refreshSequential.
    void PropertyModel::_refreshAssociative(PropertyNode* node, const rttr::variant& val)
    {
        node->setLiveValue(val);
        if (!val.is_valid() || !val.is_associative_container())
        {
            return;
        }
        auto view = val.create_associative_view();
        if (view.is_key_only_type())
        {
            node->setArraySize(static_cast<int>(view.get_size()));
            return;
        }

        QVector<QPair<QString, rttr::variant>> entries; // (key display, value)
        for (auto it = view.begin(); it != view.end(); ++it)
        {
            entries.append({ TypeRenderer::toDisplayString(TypeRenderer::unwrap(it.get_key())),
                             TypeRenderer::unwrap(it.get_value()) });
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        bool sameKeys = entries.size() == node->children().size();
        for (int i = 0; sameKeys && i < entries.size(); ++i)
        {
            sameKeys = node->children()[i]->name() == QLatin1Char('[') + entries[i].first + QLatin1Char(']');
        }

        if (!sameKeys)
        {
            // Same deferral rule as arrays: never tear down rows under an open editor.
            if (_anyDescendantLocallyEdited(node))
            {
                return;
            }
            _rebuildAssocChildren(node, entries, view.get_value_type());
        }
        else
        {
            for (int i = 0; i < entries.size(); ++i)
            {
                auto* child = node->children()[i];
                if (!child->hasLocalEdit())
                {
                    _refreshNode(child, entries[i].second);
                }
            }
        }
    }

    void PropertyModel::_rebuildAssocChildren(PropertyNode* node,
                                              const QVector<QPair<QString, rttr::variant>>& entries,
                                              rttr::type valueType)
    {
        const QModelIndex nodeIdx = _indexFromNode(node);

        if (!node->children().isEmpty())
        {
            beginRemoveRows(nodeIdx, 0, node->children().size() - 1);
            for (auto* ch : node->children())
            {
                _forgetNodes(ch); // the element AND its struct sub-fields
            }
            qDeleteAll(node->children());
            node->children().clear();
            endRemoveRows();
        }

        const int newSz = entries.size();
        if (newSz > 0)
        {
            beginInsertRows(nodeIdx, 0, newSz - 1);
            const bool expand = TypeRenderer::isExpandable(valueType);
            for (int i = 0; i < newSz; ++i)
            {
                const QString elemName = QLatin1Char('[') + entries[i].first + QLatin1Char(']');
                const QString elemPath = node->path() + QLatin1Char('.') + elemName;
                auto* elem = new PropertyNode(elemName, elemPath, valueType, invalidProperty(), node);
                elem->setArrayElement(true);
                elem->setExpandable(expand);
                elem->setDisplayName(entries[i].first); // bare key in the name column
                node->children().append(elem);
                _nodeByPath.insert(elemPath, elem);
                if (expand && !TypeRenderer::isSequential(valueType))
                {
                    _buildTree(elem, valueType, elemPath);
                    _collectNodes(elem, _nodeByPath); // register sub-fields by path (see arrays)
                }
            }
            endInsertRows();
        }
        node->setArraySize(newSz);

        // Populate values only after the insert window has closed (model protocol).
        for (int i = 0; i < newSz; ++i)
        {
            _refreshNode(node->children()[i], entries[i].second);
        }
    }

    void PropertyModel::_shapeNamedElement(PropertyNode* elem, const NamedElement& named)
    {
        // The name field and — when it's a plain value — the value field leave the
        // child rows and become the row's own fields (see PropertyNode::rowLabel).
        PropertyNode* label = nullptr;
        PropertyNode* value = nullptr;
        auto& ch = elem->children();
        for (int i = ch.size() - 1; i >= 0; --i)
        {
            PropertyNode* c = ch[i];
            if (c->prop() == named.label)
            {
                label = c;
                ch.removeAt(i);
            }
            else if (named.value.is_valid() && c->prop() == named.value && !c->isExpandable())
            {
                value = c;
                ch.removeAt(i);
            }
        }
        elem->setRowFields(label, value);
        elem->setExpandable(!ch.isEmpty()); // no other fields: a plain "name   value" row
    }

    void PropertyModel::_syncNamedRow(PropertyNode* elem)
    {
        // The row's name: its label field's text — "[3]" until there is one.
        const PropertyNode* label = elem->rowLabel();
        const rttr::variant v = label ? label->liveValue() : rttr::variant();
        const QString text = v.is_valid() ? TypeRenderer::toDisplayString(v) : QString();
        const QString shown = text.isEmpty() ? elem->name() : text;
        if (elem->displayName() != shown)
        {
            elem->setDisplayName(shown);
            elem->setCachedDisplay(QString());
            const QModelIndex idx = _indexFromNode(elem, 0);
            if (idx.isValid())
            {
                emit dataChanged(idx, idx, { Qt::DisplayRole });
            }
        }
    }

    void PropertyModel::_scheduleCatchUp(const QString& path)
    {
        // Queued: this runs from inside an editor's commit / close, and the catch-up
        // may delete that editor's row.
        QMetaObject::invokeMethod(this, [this, path] { _catchUpResizes(path); }, Qt::QueuedConnection);
    }

    void PropertyModel::_catchUpResizes(const QString& path)
    {
        // A resize _refreshSequential/_refreshAssociative held back while an
        // element was being edited / frozen is applied from the array's latest
        // value (kept on its node meanwhile) — a mirror sends an unchanged array
        // only once, so waiting for the next value could wait forever.
        TypeUseScope scope; // see rpe/core/TypeRelease.h
        const auto isContainer = [](const PropertyNode* n) {
            return TypeRenderer::isSequential(n->type()) || TypeRenderer::isAssociative(n->type());
        };
        // Only when a resize is really pending below: refreshing from the array's
        // value otherwise would briefly undo an edit the mirror hasn't echoed yet.
        std::function<bool(const PropertyNode*)> pending = [&](const PropertyNode* n) {
            if (isContainer(n))
            {
                const rttr::variant v = TypeRenderer::unwrap(n->liveValue());
                int size = -1;
                if (v.is_valid() && v.is_sequential_container())
                {
                    size = static_cast<int>(v.create_sequential_view().get_size());
                }
                else if (v.is_valid() && v.is_associative_container())
                {
                    size = static_cast<int>(v.create_associative_view().get_size());
                }
                if (size >= 0 && size != n->arraySize())
                {
                    return true;
                }
            }
            for (const PropertyNode* c : n->children())
            {
                if (pending(c))
                {
                    return true;
                }
            }
            return false;
        };
        const auto catchUp = [&](PropertyNode* top) {
            const rttr::variant live = top->liveValue();
            if (live.is_valid() && !_anyDescendantLocallyEdited(top) && pending(top))
            {
                _refreshNode(top, live);
            }
        };
        const auto outermost = [&](PropertyNode* n) {
            PropertyNode* top = nullptr;
            for (; n && n != _root.get(); n = n->parent())
            {
                if (isContainer(n))
                {
                    top = n;
                }
            }
            return top;
        };
        if (path.isEmpty())
        {
            // Outermost arrays never nest, so one's rebuild can't free another.
            QVector<PropertyNode*> tops;
            for (auto* node : std::as_const(_nodeByPath))
            {
                if (isContainer(node) && outermost(node) == node)
                {
                    tops.append(node);
                }
            }
            for (PropertyNode* top : std::as_const(tops))
            {
                catchUp(top);
            }
            return;
        }
        // The nearest node still there (the path itself may be gone with its row).
        QString p = path;
        PropertyNode* node = _findNode(p);
        while (!node && !p.isEmpty())
        {
            const int cut = p.lastIndexOf(QLatin1Char('.'));
            p = cut > 0 ? p.left(cut) : QString();
            node = p.isEmpty() ? nullptr : _findNode(p);
        }
        // The outermost array / map above it: refreshing that re-syncs everything below.
        if (PropertyNode* top = outermost(node))
        {
            catchUp(top);
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
        for (const auto* field : { node->rowLabel(), node->rowValue() })
        {
            if (field && field->hasLocalEdit())
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
                _forgetNodes(ch); // the element AND its struct sub-fields (see rebuild)
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
            const NamedElement named = namedElement(elemType); // one row per element, by name
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
                    if (named.isValid())
                    {
                        _shapeNamedElement(elem, named);
                    }
                    // Register the struct's sub-fields by path too — otherwise a leaf
                    // like "items.[0].v" is unreachable by _findNode, so a mirrored
                    // per-leaf update (setPropertyValue) for it is silently dropped and
                    // the row only refreshes on a full-vector resend (e.g. re-select).
                    _collectNodes(elem, _nodeByPath);
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

    void PropertyModel::setPropertyValue(const QString& path, rttr::variant val, const QHash<QString, QStringList>& names)
    {
        {
            QMutexLocker lk(&_pendingMutex);
            _pendingUpdates.insert(path, std::move(val));
            for (auto it = names.cbegin(); it != names.cend(); ++it)
            {
                _pendingLabels.insert(it.key(), it.value());
            }
        }
        if (!_flushScheduled.exchange(true, std::memory_order_acq_rel))
        {
            QMetaObject::invokeMethod(this, &PropertyModel::_flushPending, Qt::QueuedConnection);
        }
    }

    void PropertyModel::_flushPending()
    {
        TypeUseScope scope; // see rpe/core/TypeRelease.h
        _flushScheduled.store(false, std::memory_order_release);
        QHash<QString, rttr::variant> batch;
        QHash<QString, QStringList> names;
        {
            QMutexLocker lk(&_pendingMutex);
            batch.swap(_pendingUpdates);
            names.swap(_pendingLabels);
        }
        for (auto it = names.cbegin(); it != names.cend(); ++it)
        {
            _givenLabels.insert(it.key(), it.value()); // before the values that use them
        }
        syncNamedElements();
        _applyBatch(batch);
    }

    void PropertyModel::syncNamedElements()
    {
        const quint64 gen = TypeBridge::namedElementGeneration();
        if (gen == _namedGen)
        {
            return;
        }
        _namedGen = gen;
        TypeUseScope scope; // see rpe/core/TypeRelease.h
        // A type was (un)marked: every array lays its rows out again — from its latest
        // value, as a mirror won't resend one that hasn't changed. A mismatched size
        // is what makes _refreshSequential rebuild (deferred, as ever, while one of
        // its elements is being edited).
        for (PropertyNode* node : std::as_const(_nodeByPath))
        {
            if (TypeRenderer::isSequential(node->type()) && node->arraySize() >= 0)
            {
                node->setArraySize(-2);
            }
        }
        _catchUpResizes(QString());
    }

    QString PropertyModel::_stablePath(const PropertyNode* node) const
    {
        // Each named element above (or at) the row addressed by its NAME — "[1]"
        // becomes "{armor}" — so a pin follows the element, not the index. Only for
        // a name that is unique in its array and addressable (no '.', no '}').
        QString path = node ? node->path() : QString();
        for (const PropertyNode* n = node; n && n != _root.get(); n = n->parent())
        {
            if (!n->rowLabel() || !n->parent())
            {
                continue;
            }
            const QString name = n->displayName();
            if (name == n->name() || name.contains(QLatin1Char('.')) || name.contains(QLatin1Char('}')))
            {
                continue; // no name (yet), or one a path can't carry
            }
            int same = 0;
            for (const PropertyNode* sib : n->parent()->children())
            {
                same += sib->rowLabel() && sib->displayName() == name ? 1 : 0;
            }
            if (same != 1)
            {
                continue;
            }
            const QString base = n->path();
            const QString parentPath = n->parent()->path();
            path = parentPath + QStringLiteral(".{") + name + QLatin1Char('}') + path.mid(base.size());
        }
        return path;
    }

    void PropertyModel::_applyBatch(const QHash<QString, rttr::variant>& batch)
    {
        for (auto it = batch.cbegin(); it != batch.cend(); ++it)
        {
            auto* node = _findNode(it.key());
            if (!node)
            {
                continue;
            }
            if (node->hasLocalEdit())
            {
                // Frozen leaf: don't disturb the shown draft, but keep the underlying
                // live value current so a later reset snaps to the REAL live value.
                // Mirror mode dedups, so it may never resend an unchanged value — and
                // in direct mode the 50 Hz refresh would keep it fresh, which is why
                // this staleness only bit mirror mode. (Expandable frozen nodes are
                // left as-is; their structure isn't rebuilt on this path.)
                if (!node->isExpandable())
                {
                    node->setLiveValueQuiet(TypeRenderer::unwrap(it.value()));
                }
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

    bool PropertyModel::_emitDirtyRanges(PropertyNode* parent)
    {
        auto& ch = parent->children();
        int rangeStart = -1;
        bool subtreeDirty = false;

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
            // A named element's row shows its row fields: they make it dirty too.
            bool fieldsDirty = false;
            for (auto* field : { node->rowLabel(), node->rowValue() })
            {
                if (field && field->isDirty())
                {
                    field->clearDirty();
                    fieldsDirty = true;
                }
            }
            if (node->isDirty() || fieldsDirty)
            {
                node->clearDirty();
                subtreeDirty = true;
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
                subtreeDirty |= _emitDirtyRanges(node);
            }
        }
        flush(static_cast<int>(ch.size()));

        // A collapsed struct row summarises its children in its own value cell —
        // when any descendant changed, that cell is stale too, even though the
        // parent node itself carries no dirty flag. Repaint just that cell.
        if (subtreeDirty && parent != _root.get() && parent->isExpandable()
            && parent->arraySize() < 0 && !_expandedPaths.contains(parent->path()))
        {
            const QModelIndex idx = _indexFromNode(parent, 1);
            emit dataChanged(idx, idx, { Qt::DisplayRole });
        }
        return subtreeDirty;
    }

    // ── local edit / reset ────────────────────────────────────────────────────────────

    void PropertyModel::beginLocalEdit(const QString& path)
    {
        TypeUseScope scope; // see rpe/core/TypeRelease.h
        auto* node = _findNode(path);
        if (!node || node->hasLocalEdit())
        {
            return;
        }
        node->setLocalEdit(true);
        node->setLocalEditValue(node->liveValue());
        const auto idx = _indexFromNode(node);
        emit dataChanged(idx, idx.siblingAtColumn(columnCount() - 1));
        emit localEditsChanged();
    }

    void PropertyModel::beginEditorPin(const QString& path)
    {
        auto* node = _findNode(path);
        if (!node || node->hasLocalEdit())
        {
            return; // already frozen by the user: that freeze is theirs, not the editor's
        }
        _editorPinPath = path;
        beginLocalEdit(path);
    }

    void PropertyModel::resetNode(const QString& path)
    {
        TypeUseScope scope; // see rpe/core/TypeRelease.h
        if (path == _editorPinPath)
        {
            _editorPinPath.clear();
        }
        auto* node = _findNode(path);
        if (!node || !node->hasLocalEdit())
        {
            return;
        }
        node->setLocalEdit(false);
        const auto idx = _indexFromNode(node);
        emit dataChanged(idx, idx.siblingAtColumn(columnCount() - 1));
        emit localEditsChanged();
        _scheduleCatchUp(path); // an array resize it was holding back
    }

    void PropertyModel::resetAll()
    {
        TypeUseScope scope; // see rpe/core/TypeRelease.h
        _editorPinPath.clear();
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
            emit localEditsChanged();
            _scheduleCatchUp(QString()); // every array
        }
    }

    bool PropertyModel::hasAnyLocalEdit() const
    {
        for (auto* node : std::as_const(_nodeByPath))
        {
            if (node->hasLocalEdit() && node->path() != _editorPinPath)
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

    void PropertyModel::setPinnedPaths(const QSet<QString>& paths)
    {
        if (_pinnedPaths == paths)
        {
            return;
        }
        _pinnedPaths = paths;
        // Rare (pin/unpin/selection change) — a whole-tree repaint is fine.
        if (!_root->children().isEmpty())
        {
            emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), { Qt::ForegroundRole });
        }
    }

    void PropertyModel::setPathExpanded(const QString& path, bool expanded)
    {
        if (expanded == _expandedPaths.contains(path))
        {
            return;
        }
        if (expanded)
        {
            _expandedPaths.insert(path);
        }
        else
        {
            _expandedPaths.remove(path);
        }
        // Only a non-array expandable row renders differently (summary ↔ blank).
        auto* node = _findNode(path);
        if (node && node->isExpandable() && node->arraySize() < 0)
        {
            const QModelIndex idx = _indexFromNode(node, 1);
            emit dataChanged(idx, idx, { Qt::DisplayRole });
        }
    }

    // Compact one-line summary of a small struct's fields: "[3, 1.5]". Nested
    // non-leaf fields show as "…". Empty when the struct is too wide (>4 fields —
    // a summary that long stops being scannable) or when nothing has a value yet
    // (e.g. mirror mode before the first snapshot arrives): blank, as before.
    QString PropertyModel::_structSummary(PropertyNode* node) const
    {
        const auto& ch = node->children();
        if (ch.isEmpty() || ch.size() > 4)
        {
            return {};
        }
        QStringList parts;
        bool anyValue = false;
        for (auto* c : ch)
        {
            const rttr::variant v = c->effectiveValue();
            if (c->isLeaf() && v.is_valid())
            {
                parts << TypeRenderer::toDisplayString(v);
                anyValue = true;
            }
            else
            {
                parts << QStringLiteral("…");
            }
        }
        if (!anyValue)
        {
            return {};
        }
        QString s = QLatin1Char('[') + parts.join(QStringLiteral(", ")) + QLatin1Char(']');
        if (s.size() > 44)
        {
            s = s.left(42) + QStringLiteral("…]");
        }
        return s;
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

    QString PropertyModel::_readOnlyReason(const PropertyNode* node) const
    {
        if (!node || node == _root.get())
        {
            return {};
        }
        const quint64 regGen = TypeBridge::registryGeneration();
        const quint64 lockGen = TypeBridge::readOnlyGeneration();
        if (regGen != _roRegistryGen || lockGen != _roLockGen)
        {
            _roCache.clear();
            _roRegistryGen = regGen;
            _roLockGen = lockGen;
        }
        const QString path = node->path();
        const auto it = _roCache.constFind(path);
        if (it != _roCache.constEnd())
        {
            return it.value();
        }
        QString why;
        if (const PropertyNode* g = _groupOf(node))
        {
            // Grouped: the rule asks about the row's own component, by its own path.
            why = (g == node) ? QString() : readOnlyReason(g->type(), path.mid(g->path().size()));
        }
        else
        {
            why = readOnlyReason(_boundType, path);
        }
        _roCache.insert(path, why);
        return why;
    }

    bool PropertyModel::_applyEdit(PropertyNode* node, const rttr::variant& newVal)
    {
        if (node->path() == _editorPinPath)
        {
            _editorPinPath.clear(); // committed: from here on it's an ordinary edit
        }
        if (_editSink)
        {
            // Mirror mode: hand the edit off, show it optimistically, and resume
            // live updates (the sim thread will echo the applied value back).
            // Shown as the property's own type (an enum edit arrives as its integer —
            // see PropertyDelegate::setModelData); the sink gets the portable value.
            const rttr::variant shown = bridge::coerce(newVal, node->type());
            node->setLocalEdit(false);
            node->setLiveValue(shown.is_valid() ? shown : newVal);
            _editSink(node->path(), newVal);
            emit propertyEdited(node->path(), node->liveValue());
            emit localEditsChanged();
            _scheduleCatchUp(node->path()); // an array resize the open editor held back
            return true;
        }

        // Grouped: the row's own component, addressed by its key, at its own path.
        QString groupKey, relPath = node->path();
        const bool grouped = _grouped && splitGroupPath(node->path(), &groupKey, &relPath);
        if (_editPolicy == EditPolicy::WriteBack && (grouped ? bool(_groupInstanceProvider) : bool(_instanceProvider)))
        {
            bool wrote = false;
            bool released = false;
            bool gone = false;
            {
                // The provider may hand back a pointer into data owned by another
                // thread; the whole provider+write+read-back sequence runs under the
                // guard so it can't race the owner. Leave the read section while
                // waiting for the guard — its owner may be releasing types (a plugin
                // unloading) and waiting for us — and take it again inside.
                TypeUseGap gap;
                withGuard(_writeGuard, [&] {
                    TypeUseScope scope;
                    if (_released.load(std::memory_order_acquire))
                    {
                        released = true; // the tree was emptied meanwhile
                        return;
                    }
                    rttr::instance inst = grouped ? _groupInstanceProvider(groupKey) : _instanceProvider();
                    if (inst.is_valid() && bridge::setValueByPath(inst, relPath, newVal))
                    {
                        // Read back so the display reflects any coercion the object
                        // applied — stored in the node HERE, inside the read section:
                        // the read-back value is made by the property's (maybe a
                        // plugin's) code and must not outlive it on this stack.
                        const rttr::variant actual = bridge::getValueByPath(inst, relPath);
                        node->setLocalEdit(false);
                        node->setLiveValue(actual.is_valid() ? actual : bridge::coerce(newVal, node->type()));
                        wrote = true;
                    }
                    else if (inst.is_valid() && !bridge::getValueByPath(inst, relPath).is_valid())
                    {
                        // The target is gone — an array element whose index went
                        // away while its editor was open, a map key removed. There
                        // is nothing to keep a draft OF: drop the edit.
                        gone = true;
                    }
                });
            }
            if (released || _released.load(std::memory_order_acquire))
            {
                return false;
            }
            if (wrote)
            {
                emit propertyEdited(node->path(), node->liveValue());
                emit localEditsChanged();
                _scheduleCatchUp(node->path());
                return true;
            }
            if (gone)
            {
                if (node->hasLocalEdit())
                {
                    node->setLocalEdit(false);
                    emit localEditsChanged();
                }
                _scheduleCatchUp(node->path()); // the rows can follow the data now
                return false;
            }
            // Write failed — fall through to a local draft so the edit is not lost.
        }

        const rttr::variant draft = bridge::coerce(newVal, node->type());
        node->setLocalEditValue(draft.is_valid() ? draft : newVal);
        node->setLocalEdit(true);
        emit propertyEdited(node->path(), node->localEditValue());
        emit localEditsChanged();
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
        TypeUseScope scope; // see rpe/core/TypeRelease.h
        if (!index.isValid())
        {
            return {};
        }
        auto* node = static_cast<PropertyNode*>(index.internalPointer());

        // Grouped view: a header row is a label (+ its match count), nothing else.
        if (_isGroupNode(node))
        {
            switch (role)
            {
            case Qt::DisplayRole:
            case Qt::ToolTipRole:
                return index.column() == 0 ? node->displayName() : QString();
            case IsGroupRole: // (bold: the view's delegate — a FontRole QFont would be the app default font)
                return true;
            case GroupKeyRole:
                return _groupKeys.value(node);
            case GroupCountRole:
                return _groupCounts.value(_groupKeys.value(node), 0);
            case PropertyPathRole:
                return node->path();
            case IsLeafRole:
                return false;
            default:
                return {};
            }
        }

        // A named element's row (rpe/core/NamedElement.h): the name column shows the
        // element's name; every value-side role is its value field's (the field the
        // row edits) — path, value, editor hints, read-only, local edit, filtering.
        if (node->rowLabel() || node->rowValue())
        {
            switch (role)
            {
            case Qt::DisplayRole:
                if (index.column() == 0)
                {
                    return node->displayName();
                }
                if (!node->rowValue() && !node->isExpandable())
                {
                    return QString(); // a name with nothing else: no value to show
                }
                break;
            case Qt::ToolTipRole:
                if (index.column() == 0)
                {
                    return QStringLiteral("%1 \u2014 %2").arg(node->displayName(), node->name());
                }
                break;
            case RowLabelPathRole:
                return node->rowLabel() ? node->rowLabel()->path() : QString();
            case StablePathRole:
                return _stablePath(node->rowValue() ? node->rowValue() : node);
            case IsLeafRole:
                return node->isLeaf();
            case IsArrayRole:
                return false;
            default:
                break;
            }
            if (node->rowValue())
            {
                node = node->rowValue();
            }
        }

        switch (role)
        {
        case IsGroupRole:
            return false;
        case StablePathRole:
            return _stablePath(node);
        case RowLabelPathRole:
            return QString();
        case GroupKeyRole:
            if (const PropertyNode* g = _groupOf(node))
            {
                return _groupKeys.value(g);
            }
            return QString();
        case Qt::DisplayRole:
            if (index.column() == 0)
            {
                return node->displayName();
            }
            if (index.column() == 1)
            {
                // Expandable rows carry no scalar value of their own — the children
                // do. Arrays show their element count. A COLLAPSED small struct
                // shows a compact "[a, b]" summary of its fields; expanding it
                // blanks the cell (the children now display the values), so the
                // same data is never on screen twice. Wide structs stay blank —
                // never a bare "<invalid>".
                if (node->isExpandable())
                {
                    if (node->arraySize() >= 0)
                    {
                        return QStringLiteral("[%1]").arg(node->arraySize());
                    }
                    return _expandedPaths.contains(node->path()) ? QString() : _structSummary(node);
                }
                // Leaf with no value yet (e.g. not mirrored): blank, not "<invalid>".
                const rttr::variant v = node->effectiveValue();
                if (!v.is_valid())
                {
                    return QString();
                }
                if (node->cachedDisplay().isEmpty())
                {
                    const bool asFlags = v.get_type().is_enumeration()
                        && metaBool(node->prop(), hint::Flags, false);
                    node->setCachedDisplay(asFlags ? TypeRenderer::flagsToDisplayString(v)
                                                   : TypeRenderer::toDisplayString(v));
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
            // …and so do STRINGS that hold a colour (the rpe::editor::Color hint):
            // "#AARRGGBB", "#RRGGBB" or a named colour. Not a colour → no swatch.
            if (index.column() == 1 && node->isLeaf()
                && metaString(node->prop(), hint::Editor) == QLatin1String(editor::Color))
            {
                const rttr::variant v = TypeRenderer::unwrap(node->effectiveValue());
                const QColor c(v.is_valid() ? TypeRenderer::toDisplayString(v) : QString());
                if (c.isValid())
                {
                    return c;
                }
            }
            break;

        case InlineVectorRole:
            return _isInlineVector(node);

        case ReadOnlyRole:
            return _readOnlyReason(node);

        case Qt::ToolTipRole:
            if (index.column() == 1)
            {
                const QString why = _readOnlyReason(node);
                if (!why.isEmpty())
                {
                    return node->tooltip().isEmpty() ? why : node->tooltip() + QStringLiteral("\n\n") + why;
                }
            }
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
                return QBrush(QColor(0xE5, 0x9A, 0x2E)); // amber: local draft edit
            }
            if (!_pinnedPaths.isEmpty() && (_pinnedPaths.contains(node->path()) || _pinnedPaths.contains(_stablePath(node))))
            {
                return QBrush(QColor(0x4D, 0xB6, 0xAC)); // teal: pinned to the watch list
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
        case FlagsRole:
            return metaBool(node->prop(), hint::Flags, false);
        case FilterValueRole:
        {
            // Value text for the property filter — the SAME string the value cell
            // would show when collapsed, but computed regardless of the row's current
            // expansion (so a struct's "[a, b]" summary or an array's "[N]" stays
            // matchable even while the tree is force-expanded during filtering).
            // Cheap: leaves reuse the cached display; struct summaries scan ≤4 fields.
            if (node->isExpandable())
            {
                if (node->arraySize() >= 0)
                {
                    return QStringLiteral("[%1]").arg(node->arraySize());
                }
                return _structSummary(node);
            }
            const rttr::variant v = node->effectiveValue();
            if (!v.is_valid())
            {
                return QString();
            }
            if (node->cachedDisplay().isEmpty())
            {
                const bool asFlags = v.get_type().is_enumeration() && metaBool(node->prop(), hint::Flags, false);
                node->setCachedDisplay(asFlags ? TypeRenderer::flagsToDisplayString(v)
                                               : TypeRenderer::toDisplayString(v));
            }
            return node->cachedDisplay();
        }
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
        TypeUseScope scope; // see rpe/core/TypeRelease.h
        if (!index.isValid() || role != Qt::EditRole || index.column() != 1)
        {
            return false;
        }
        if (!value.canConvert<rttr::variant>())
        {
            return false;
        }

        auto* node = static_cast<PropertyNode*>(index.internalPointer());
        if (node->rowValue())
        {
            node = node->rowValue(); // a named element's row edits its value field
        }
        const rttr::variant newVal = value.value<rttr::variant>();
        if (!newVal.is_valid())
        {
            return false;
        }
        // The UI never opens an editor on a read-only row; this is for every other
        // caller. Refusing here — rather than letting the write fail further down —
        // is what keeps a getter-only value from being shown as a frozen "edit" that
        // never reached the object.
        if (!_readOnlyReason(node).isEmpty())
        {
            return false;
        }

        if (!_applyEdit(node, newVal))
        {
            return false;
        }

        // Invalidate the display cache; the DisplayRole read triggered by the
        // dataChanged below recomputes it with the correct formatter (e.g. the
        // flags decomposition, which plain toDisplayString would not apply).
        node->setCachedDisplay(QString());
        node->clearDirty();
        emit dataChanged(index.siblingAtColumn(0), index.siblingAtColumn(columnCount() - 1));
        return true;
    }

    Qt::ItemFlags PropertyModel::flags(const QModelIndex& index) const
    {
        TypeUseScope scope; // see rpe/core/TypeRelease.h
        if (!index.isValid())
        {
            return Qt::NoItemFlags;
        }
        auto* node = static_cast<PropertyNode*>(index.internalPointer());

        Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (_isGroupNode(node))
        {
            return f; // a header: never edited
        }
        if (node->rowValue())
        {
            node = node->rowValue(); // a named element's row edits its value field
        }
        // Read-only rows stay ENABLED (normal text, selectable, copyable) — they just
        // never get ItemIsEditable, so no editor opens. The reason is shown on hover.
        if (!_readOnly && index.column() == 1 && node->isLeaf() && TypeRenderer::isInlineEditable(node->type())
            && _readOnlyReason(node).isEmpty())
        {
            f |= Qt::ItemIsEditable;
        }
        // A small numeric struct (Vec3, a colour as floats, a 2D size…) also edits
        // inline on its OWN row — one box per field — without expanding it.
        if (!_readOnly && index.column() == 1 && _isInlineVector(node))
        {
            f |= Qt::ItemIsEditable;
        }
        return f;
    }

    bool PropertyModel::_isInlineVector(const PropertyNode* node) const
    {
        if (!node || node == _root.get() || _isGroupNode(node) || node->isLeaf() || node->arraySize() >= 0)
        {
            return false;
        }
        const auto& ch = node->children();
        if (ch.size() < 2 || ch.size() > 4 || !_readOnlyReason(node).isEmpty())
        {
            return false;
        }
        for (const PropertyNode* c : ch)
        {
            const rttr::type t = TypeRenderer::rawType(c->type());
            const bool wide = t == rttr::type::get<long>() || t == rttr::type::get<unsigned long>()
                || t == rttr::type::get<long long>() || t == rttr::type::get<unsigned long long>()
                || t == rttr::type::get<unsigned int>();
            if (!c->isLeaf() || !t.is_arithmetic() || t == rttr::type::get<bool>() || wide
                || !_readOnlyReason(c).isEmpty())
            {
                return false;
            }
        }
        return true;
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
        if (node->isRowField())
        {
            node = node->parent(); // shown on its element's row
        }
        return createIndex(node->row(), column, node);
    }

    PropertyNode* PropertyModel::_findNode(const QString& path) const
    {
        return _nodeByPath.value(path, nullptr);
    }

} // namespace rpe

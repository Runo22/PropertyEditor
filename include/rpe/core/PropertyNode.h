#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QString>
#include <QVector>

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  PropertyNode — one row in the property tree.
    //
    //  The tree is built once from rttr::type metadata (the *schema*) and is never
    //  rebuilt during value updates, except for sequential containers whose element
    //  count changed. This keeps the hot live-update path allocation-free.
    // ─────────────────────────────────────────────────────────────────────────────
    class PropertyNode
    {
    public:
        PropertyNode(QString name, QString path, rttr::type type,
                     rttr::property prop, // invalid for synthetic nodes (array elements)
                     PropertyNode* parent = nullptr);
        ~PropertyNode();

        PropertyNode(const PropertyNode&) = delete;
        PropertyNode& operator=(const PropertyNode&) = delete;

        // ── Tree structure (non-owning parent, owning children) ──────────────────
        PropertyNode* parent() const
        {
            return _parent;
        }
        QVector<PropertyNode*>& children()
        {
            return _children;
        }
        const QVector<PropertyNode*>& children() const
        {
            return _children;
        }
        int row() const;

        // ── Identity / schema ────────────────────────────────────────────────────
        const QString& name() const
        {
            return _name;
        }
        const QString& displayName() const
        {
            return _displayName.isEmpty() ? _name : _displayName;
        }
        const QString& path() const
        {
            return _path;
        }
        const QString& tooltip() const
        {
            return _tooltip;
        }
        rttr::type type() const
        {
            return _type;
        }
        rttr::property prop() const
        {
            return _prop;
        }
        bool isArrayElement() const
        {
            return _arrayElement;
        }
        bool isLeaf() const
        {
            return _children.isEmpty() && !_expandable;
        }
        bool isExpandable() const
        {
            return _expandable;
        }

        void setDisplayName(QString s)
        {
            _displayName = std::move(s);
        }
        void setTooltip(QString s)
        {
            _tooltip = std::move(s);
        }
        void setArrayElement(bool v)
        {
            _arrayElement = v;
        }
        void setExpandable(bool v)
        {
            _expandable = v;
        }

        // ── Value state ──────────────────────────────────────────────────────────
        const rttr::variant& liveValue() const
        {
            return _liveValue;
        }
        const rttr::variant& overrideValue() const
        {
            return _overrideValue;
        }
        const rttr::variant& effectiveValue() const
        {
            return _isOverridden ? _overrideValue : _liveValue;
        }
        bool isOverridden() const
        {
            return _isOverridden;
        }
        bool isDirty() const
        {
            return _isDirty;
        }
        const QString& cachedDisplay() const
        {
            return _cachedDisplay;
        }

        void setLiveValue(const rttr::variant& v);
        void setOverrideValue(const rttr::variant& v);
        void setOverridden(bool v)
        {
            _isOverridden = v;
            _isDirty = true;
        }
        void clearDirty()
        {
            _isDirty = false;
        }
        void setCachedDisplay(const QString& s) const
        {
            _cachedDisplay = s;
        }

        // ── Sequential-container bookkeeping ─────────────────────────────────────
        int arraySize() const
        {
            return _arraySize;
        }
        void setArraySize(int s)
        {
            _arraySize = s;
        }

    private:
        QString _name;
        QString _displayName;
        QString _path;
        QString _tooltip;
        rttr::type _type;
        rttr::property _prop;
        PropertyNode* _parent = nullptr;
        QVector<PropertyNode*> _children;

        rttr::variant _liveValue;
        rttr::variant _overrideValue;
        bool _isOverridden = false;
        bool _isDirty = false;
        bool _arrayElement = false;
        bool _expandable = false;
        mutable QString _cachedDisplay;
        int _arraySize = -1; // -1 = not a sequential node
    };

} // namespace rpe

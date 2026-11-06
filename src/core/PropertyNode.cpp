#include "rpe/core/PropertyNode.h"

namespace rpe
{

    PropertyNode::PropertyNode(QString name, QString path, rttr::type type, rttr::property prop, PropertyNode* parent)
        : _name(std::move(name))
        , _path(std::move(path))
        , _type(type)
        , _prop(prop)
        , _parent(parent)
    {
    }

    PropertyNode::~PropertyNode()
    {
        qDeleteAll(_children);
    }

    void PropertyNode::releaseReflection()
    {
        _type = rttr::type::get<void>();
        // RTTR has no public invalid-property constructor; a missing property on
        // `void` is one (and lives in rpe_core, not in a plugin).
        _prop = rttr::type::get<void>().get_property(std::string());
        _liveValue = rttr::variant();
        _localEditValue = rttr::variant();
        _hasLocalEdit = false;
        for (PropertyNode* c : _children)
        {
            c->releaseReflection();
        }
    }

    int PropertyNode::row() const
    {
        if (!_parent)
        {
            return 0;
        }
        return static_cast<int>(_parent->_children.indexOf(const_cast<PropertyNode*>(this)));
    }

    void PropertyNode::setLiveValue(const rttr::variant& v)
    {
        _liveValue = v;
        _isDirty = true;
        _cachedDisplay = QString(); // invalidate; recomputed lazily by data()
    }

    void PropertyNode::setLocalEditValue(const rttr::variant& v)
    {
        _localEditValue = v;
        _isDirty = true;
        _cachedDisplay = QString();
    }

} // namespace rpe

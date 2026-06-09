#include "rpe/core/PropertyNode.h"

namespace rpe {

PropertyNode::PropertyNode(QString        name,
                           QString        path,
                           rttr::type     type,
                           rttr::property prop,
                           PropertyNode*  parent)
    : _name(std::move(name))
    , _path(std::move(path))
    , _type(type)
    , _prop(prop)
    , _parent(parent)
{}

PropertyNode::~PropertyNode()
{
    qDeleteAll(_children);
}

int PropertyNode::row() const
{
    if (!_parent)
        return 0;
    return static_cast<int>(_parent->_children.indexOf(const_cast<PropertyNode*>(this)));
}

void PropertyNode::setLiveValue(const rttr::variant& v)
{
    _liveValue     = v;
    _isDirty       = true;
    _cachedDisplay = QString();   // invalidate; recomputed lazily by data()
}

void PropertyNode::setOverrideValue(const rttr::variant& v)
{
    _overrideValue = v;
    _isDirty       = true;
    _cachedDisplay = QString();
}

} // namespace rpe

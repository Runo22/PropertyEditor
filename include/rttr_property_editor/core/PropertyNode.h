#pragma once

#include <QString>
#include <QVector>
#include <rttr/type>
#include <rttr/variant>
#include <rttr/property>

namespace rpe {

// One row in the property tree.
// The tree is built once from rttr::type metadata and never rebuilt on value updates.
class PropertyNode
{
public:
    explicit PropertyNode(
        QString         name,
        QString         path,
        rttr::type      type,
        rttr::property  prop,        // invalid for synthetic nodes (array elements)
        PropertyNode*   parent = nullptr);

    ~PropertyNode();

    // Tree structure (non-owning parent, owning children)
    PropertyNode*              parent()   const { return _parent; }
    QVector<PropertyNode*>&    children()       { return _children; }
    const QVector<PropertyNode*>& children() const { return _children; }
    int                        row()      const;

    // Identity
    const QString&      name() const { return _name; }
    const QString&      path() const { return _path; }
    rttr::type          type() const { return _type; }
    rttr::property      prop() const { return _prop; }

    // Value state
    const rttr::variant& liveValue()     const { return _liveValue; }
    const rttr::variant& overrideValue() const { return _overrideValue; }
    bool                 isOverridden()  const { return _isOverridden; }
    bool                 isDirty()       const { return _isDirty; }
    const QString&       cachedDisplay() const { return _cachedDisplay; }

    void setLiveValue(const rttr::variant& v);
    void setOverrideValue(const rttr::variant& v);
    void setOverridden(bool v)      { _isOverridden = v; }
    void clearDirty()               { _isDirty = false; }
    // Cache is mutable so data() can update it without breaking const contract
    void setCachedDisplay(const QString& s) const { _cachedDisplay = s; }

    // For vector<T> nodes: current child count matches last-known array size.
    // Caller rebuilds children when size changes.
    int  arraySize() const  { return _arraySize; }
    void setArraySize(int s){ _arraySize = s; }

private:
    QString               _name;
    QString               _path;
    rttr::type            _type;
    rttr::property        _prop;
    PropertyNode*         _parent     = nullptr;
    QVector<PropertyNode*> _children;

    rttr::variant          _liveValue;
    rttr::variant          _overrideValue;
    bool                   _isOverridden = false;
    bool                   _isDirty      = false;
    mutable QString        _cachedDisplay;
    int                    _arraySize    = -1;   // -1 = not an array node
};

} // namespace rpe

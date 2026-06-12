#include "rpe/core/RttrVariantWrapper.h"

#include "rpe/core/RttrBridge.h"
#include "rpe/core/TypeBridge.h"
#include "rpe/core/TypeRenderer.h"

namespace rpe
{

    RttrVariantWrapper::RttrVariantWrapper(rttr::variant v)
        : _owned(std::move(v))
    {
        _type = TypeRenderer::rawType(_owned.get_type());
    }

    RttrVariantWrapper RttrVariantWrapper::makeLinked(rttr::type type, void* obj)
    {
        RttrVariantWrapper w;
        w._linked = true;
        w._type = type;
        w._access = TypeBridge::wrap(type, obj);
        return w;
    }

    void RttrVariantWrapper::relink(void* obj)
    {
        if (_linked)
        {
            _access = TypeBridge::wrap(_type, obj);
        }
    }

    bool RttrVariantWrapper::isValid() const
    {
        return _linked ? _access.is_valid() : _owned.is_valid();
    }

    rttr::type RttrVariantWrapper::type() const
    {
        return _linked ? _type : TypeRenderer::rawType(_owned.get_type());
    }

    QString RttrVariantWrapper::typeName() const
    {
        return QString::fromStdString(type().get_name().to_string());
    }

    rttr::instance RttrVariantWrapper::instance()
    {
        if (_linked)
        {
            return _access.is_valid() ? rttr::instance(_access) : rttr::instance();
        }
        return rttr::instance(_owned);
    }

    void RttrVariantWrapper::clear()
    {
        _owned = rttr::variant();
        _access = rttr::variant();
        _linked = false;
        _type = rttr::type::get<void>();
    }

    QStringList RttrVariantWrapper::topLevelPropertyNames() const
    {
        QStringList names;
        for (auto& p : type().get_properties())
        {
            names.append(QString::fromStdString(p.get_name().to_string()));
        }
        return names;
    }

    rttr::variant RttrVariantWrapper::get(const QString& path) const
    {
        return bridge::getValueByPath(const_cast<RttrVariantWrapper*>(this)->instance(), path);
    }

    bool RttrVariantWrapper::set(const QString& path, const rttr::variant& value)
    {
        return bridge::setValueByPath(instance(), path, value);
    }

    QString RttrVariantWrapper::displayString() const
    {
        return _linked ? QStringLiteral("{%1}").arg(typeName())
                       : TypeRenderer::toDisplayString(_owned);
    }

} // namespace rpe

#include "rpe/core/TypeRenderer.h"

#include <QColor>
#include <QStringList>

#include <rttr/enumeration.h>
#include <rttr/variant_sequential_view.h>

namespace rpe {

rttr::type TypeRenderer::rawType(rttr::type t)
{
    return t.is_wrapper() ? t.get_wrapped_type() : t;
}

rttr::variant TypeRenderer::unwrap(const rttr::variant& v)
{
    return v.get_type().is_wrapper() ? v.extract_wrapped_value() : v;
}

bool TypeRenderer::isSequential(rttr::type t)
{
    return rawType(t).is_sequential_container();
}

bool TypeRenderer::isExpandable(rttr::type t)
{
    const rttr::type r = rawType(t);
    return r.is_sequential_container() || !r.get_properties().empty();
}

bool TypeRenderer::isInlineEditable(rttr::type t)
{
    const rttr::type r = rawType(t);
    if (r.is_arithmetic())   return true;
    if (r.is_enumeration())  return true;
    if (r == rttr::type::get<std::string>()) return true;
    if (r == rttr::type::get<QString>())     return true;
    if (r == rttr::type::get<bool>())        return true;
    if (r == rttr::type::get<QColor>())      return true;
    return false;
}

QString TypeRenderer::toDisplayString(const rttr::variant& vIn)
{
    if (!vIn.is_valid())
        return QStringLiteral("<invalid>");

    const rttr::variant v = unwrap(vIn);
    const rttr::type    t = v.get_type();

    if (t == rttr::type::get<bool>())
        return v.get_value<bool>() ? QStringLiteral("true") : QStringLiteral("false");
    if (t == rttr::type::get<float>())
        return QString::number(static_cast<double>(v.get_value<float>()), 'g', 6);
    if (t == rttr::type::get<double>())
        return QString::number(v.get_value<double>(), 'g', 8);
    if (t == rttr::type::get<std::string>())
        return QString::fromStdString(v.get_value<std::string>());
    if (t == rttr::type::get<QString>())
        return v.get_value<QString>();
    if (t == rttr::type::get<QColor>())
        return v.get_value<QColor>().name(QColor::HexArgb);

    if (t.is_arithmetic()) {
        // Signed / unsigned integers of any width go through string conversion.
        rttr::variant conv = v;
        if (conv.convert(rttr::type::get<int64_t>()))
            return QString::number(conv.get_value<int64_t>());
        if (conv.convert(rttr::type::get<uint64_t>()))
            return QString::number(conv.get_value<uint64_t>());
    }

    if (t.is_enumeration()) {
        const rttr::string_view name = t.get_enumeration().value_to_name(v);
        if (!name.empty())
            return QString::fromUtf8(name.data(), static_cast<int>(name.size()));
    }

    if (t.is_sequential_container()) {
        auto view = v.create_sequential_view();
        return QStringLiteral("[%1 %2]")
            .arg(view.get_size())
            .arg(view.get_size() == 1 ? QStringLiteral("item") : QStringLiteral("items"));
    }

    if (!t.get_properties().empty())
        return QStringLiteral("{%1}").arg(QString::fromStdString(t.get_name().to_string()));

    // Best-effort textual conversion, then fall back to the type name.
    bool ok = false;
    const std::string s = v.to_string(&ok);
    if (ok && !s.empty())
        return QString::fromStdString(s);

    return QString::fromStdString(t.get_name().to_string());
}

} // namespace rpe

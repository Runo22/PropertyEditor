#include "rttr_property_editor/core/TypeRenderer.h"

#include <rttr/type>
#include <rttr/enumeration>

#include <QString>
#include <QStringList>

namespace rpe {

// ── helpers ──────────────────────────────────────────────────────────────────

static QString variantToString(const rttr::variant& v)
{
    const rttr::type t = v.get_type();

    if (t == rttr::type::get<double>())
        return QString::number(v.get_value<double>(), 'g', 6);
    if (t == rttr::type::get<float>())
        return QString::number(static_cast<double>(v.get_value<float>()), 'g', 6);
    if (t == rttr::type::get<int>())
        return QString::number(v.get_value<int>());
    if (t == rttr::type::get<unsigned int>())
        return QString::number(v.get_value<unsigned int>());
    if (t == rttr::type::get<long long>())
        return QString::number(v.get_value<long long>());
    if (t == rttr::type::get<unsigned long long>())
        return QString::number(v.get_value<unsigned long long>());
    if (t == rttr::type::get<bool>())
        return v.get_value<bool>() ? QStringLiteral("true") : QStringLiteral("false");
    if (t == rttr::type::get<std::string>())
        return QString::fromStdString(v.get_value<std::string>());
    if (t == rttr::type::get<QString>())
        return v.get_value<QString>();

    // Enum: convert to enumerator name
    if (t.is_enumeration()) {
        auto en  = t.get_enumeration();
        bool ok  = false;
        auto str = en.value_to_name(v, ok);
        if (ok)
            return QString::fromStdString(str.to_string());
    }

    // Sequential container (vector, array, etc.)
    if (t.is_sequential_container()) {
        auto view = v.create_sequential_view();
        return QStringLiteral("[%1]").arg(view.get_size());
    }

    // Nested struct: show type name
    if (!t.get_properties().empty())
        return QStringLiteral("{%1}").arg(QString::fromStdString(t.get_name().to_string()));

    // Fallback
    auto converted = v;
    if (converted.convert(rttr::type::get<std::string>()))
        return QString::fromStdString(converted.get_value<std::string>());

    return QString::fromStdString(t.get_name().to_string());
}

// ── TypeRenderer ─────────────────────────────────────────────────────────────

QString TypeRenderer::toDisplayString(const rttr::variant& v)
{
    if (!v.is_valid())
        return QStringLiteral("<invalid>");
    return variantToString(v);
}

bool TypeRenderer::isExpandable(rttr::type t)
{
    return !t.get_properties().empty() || t.is_sequential_container();
}

bool TypeRenderer::isSequential(rttr::type t)
{
    return t.is_sequential_container();
}

} // namespace rpe

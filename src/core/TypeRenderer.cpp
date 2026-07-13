#include "rpe/core/TypeRenderer.h"

#include <QColor>
#include <QStringList>

#include <atomic>
#include <cmath>
#include <filesystem>

#include <rttr/enumeration.h>
#include <rttr/variant_sequential_view.h>

namespace rpe
{

    namespace
    {
        std::atomic<int> g_floatDecimals { 3 };

        // Fixed-point float display: no scientific notation for ordinary magnitudes.
        // Trailing zeros (and a bare trailing dot) are trimmed, so 0.5 stays "0.5" and
        // anything smaller than the last shown digit collapses to "0" (not "2.5e-05",
        // not "-0"). Values >= 1e15 are the one exception — printing them fixed-point
        // would be an unreadable 15+ digit run, so they fall back to compact form.
        QString formatFloating(double v)
        {
            if (std::isnan(v) || std::isinf(v))
            {
                return QString::number(v); // "nan" / "inf"
            }
            // Magnitudes this large are unreadable as fixed-point anyway; fall back
            // to the compact form rather than printing a 15+ digit run.
            if (std::abs(v) >= 1e15)
            {
                return QString::number(v, 'g', 8);
            }
            QString s = QString::number(v, 'f', g_floatDecimals.load(std::memory_order_relaxed));
            if (s.contains(QLatin1Char('.')))
            {
                while (s.endsWith(QLatin1Char('0')))
                {
                    s.chop(1);
                }
                if (s.endsWith(QLatin1Char('.')))
                {
                    s.chop(1);
                }
            }
            if (s == QLatin1String("-0"))
            {
                s = QStringLiteral("0");
            }
            return s;
        }
    } // namespace

    void TypeRenderer::setFloatDecimals(int decimals)
    {
        g_floatDecimals.store(decimals < 0 ? 0 : decimals, std::memory_order_relaxed);
    }

    int TypeRenderer::floatDecimals()
    {
        return g_floatDecimals.load(std::memory_order_relaxed);
    }

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
        if (r.is_arithmetic())
        {
            return true;
        }
        if (r.is_enumeration())
        {
            return true;
        }
        if (r == rttr::type::get<std::string>())
        {
            return true;
        }
        if (r == rttr::type::get<QString>())
        {
            return true;
        }
        if (r == rttr::type::get<bool>())
        {
            return true;
        }
        if (r == rttr::type::get<QColor>())
        {
            return true;
        }
        if (isFilePath(r))
        {
            return true;
        }
        return false;
    }

    bool TypeRenderer::isFilePath(rttr::type t)
    {
        return rawType(t) == rttr::type::get<std::filesystem::path>();
    }

    QString TypeRenderer::toDisplayString(const rttr::variant& vIn)
    {
        if (!vIn.is_valid())
        {
            return QStringLiteral("<invalid>");
        }

        const rttr::variant v = unwrap(vIn);
        const rttr::type t = v.get_type();

        if (t == rttr::type::get<bool>())
        {
            return v.get_value<bool>() ? QStringLiteral("true") : QStringLiteral("false");
        }
        if (t == rttr::type::get<float>())
        {
            return formatFloating(static_cast<double>(v.get_value<float>()));
        }
        if (t == rttr::type::get<double>())
        {
            return formatFloating(v.get_value<double>());
        }
        if (t == rttr::type::get<std::string>())
        {
            return QString::fromStdString(v.get_value<std::string>());
        }
        if (t == rttr::type::get<QString>())
        {
            return v.get_value<QString>();
        }
        if (t == rttr::type::get<QColor>())
        {
            return v.get_value<QColor>().name(QColor::HexArgb);
        }
        if (t == rttr::type::get<std::filesystem::path>())
        {
            // RTTR can't stringify a path; use its UTF-16 form (path::string() is
            // lossy — and can throw — for non-ASCII paths on Windows).
            return QString::fromStdU16String(v.get_value<std::filesystem::path>().u16string());
        }

        if (t.is_arithmetic())
        {
            // Signed / unsigned integers of any width go through string conversion.
            rttr::variant conv = v;
            if (conv.convert(rttr::type::get<int64_t>()))
            {
                return QString::number(conv.get_value<int64_t>());
            }
            if (conv.convert(rttr::type::get<uint64_t>()))
            {
                return QString::number(conv.get_value<uint64_t>());
            }
        }

        if (t.is_enumeration())
        {
            const rttr::string_view name = t.get_enumeration().value_to_name(v);
            if (!name.empty())
            {
                return QString::fromUtf8(name.data(), static_cast<int>(name.size()));
            }
        }

        if (t.is_sequential_container())
        {
            auto view = v.create_sequential_view();
            return QStringLiteral("[%1 %2]")
                .arg(view.get_size())
                .arg(view.get_size() == 1 ? QStringLiteral("item") : QStringLiteral("items"));
        }

        if (!t.get_properties().empty())
        {
            return QStringLiteral("{%1}").arg(QString::fromStdString(t.get_name().to_string()));
        }

        // Best-effort textual conversion, then fall back to the type name.
        bool ok = false;
        const std::string s = v.to_string(&ok);
        if (ok && !s.empty())
        {
            return QString::fromStdString(s);
        }

        return QString::fromStdString(t.get_name().to_string());
    }

} // namespace rpe

#include "rpe/core/TypeRenderer.h"

#include "rpe/core/OptionalSupport.h"

#include <QColor>
#include <QDateTime>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string_view>

#include <rttr/enumeration.h>
#include <rttr/variant_associative_view.h>
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
        rttr::type r = t.is_wrapper() ? t.get_wrapped_type() : t;
        // A wrapper may unwrap to a POINTER (std::shared_ptr<T> → T*): resolve on
        // to the pointee class so schema/editor decisions see T, not T*.
        if (r.is_pointer())
        {
            r = r.get_raw_type();
        }
        return r;
    }

    rttr::variant TypeRenderer::unwrap(const rttr::variant& v)
    {
        if (!v.get_type().is_wrapper())
        {
            return v;
        }
        // Registered std::optional<T> is a wrapper too, but we keep it INTACT so its
        // engaged/empty state survives to the display/editor layer (extracting would
        // silently turn an empty optional into a default value). Everything else
        // (smart pointers, reference_wrapper) unwraps to the pointee.
        if (OptionalBridge::isOptional(v.get_type()))
        {
            return v;
        }
        return v.extract_wrapped_value();
    }

    bool TypeRenderer::isSequential(rttr::type t)
    {
        return rawType(t).is_sequential_container();
    }

    bool TypeRenderer::isAssociative(rttr::type t)
    {
        return rawType(t).is_associative_container();
    }

    bool TypeRenderer::isExpandable(rttr::type t)
    {
        const rttr::type r = rawType(t);
        return r.is_sequential_container() || r.is_associative_container()
            || !r.get_properties().empty();
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
        if (r == rttr::type::get<std::wstring>())
        {
            return true;
        }
        if (r == rttr::type::get<std::u16string>() || r == rttr::type::get<std::u32string>())
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
        if (r == rttr::type::get<QDateTime>())
        {
            return true;
        }
        if (isChronoDuration(r))
        {
            return true;
        }
        if (isFilePath(r))
        {
            return true;
        }
        return false;
    }

    // ── std::chrono durations ────────────────────────────────────────────────────

    bool TypeRenderer::isChronoDuration(rttr::type tIn)
    {
        const rttr::type t = rawType(tIn);
        return t == rttr::type::get<std::chrono::nanoseconds>()
            || t == rttr::type::get<std::chrono::microseconds>()
            || t == rttr::type::get<std::chrono::milliseconds>()
            || t == rttr::type::get<std::chrono::seconds>()
            || t == rttr::type::get<std::chrono::minutes>()
            || t == rttr::type::get<std::chrono::hours>();
    }

    QString TypeRenderer::chronoSuffix(rttr::type tIn)
    {
        const rttr::type t = rawType(tIn);
        if (t == rttr::type::get<std::chrono::nanoseconds>())
        {
            return QStringLiteral("ns");
        }
        if (t == rttr::type::get<std::chrono::microseconds>())
        {
            return QStringLiteral("us");
        }
        if (t == rttr::type::get<std::chrono::milliseconds>())
        {
            return QStringLiteral("ms");
        }
        if (t == rttr::type::get<std::chrono::seconds>())
        {
            return QStringLiteral("s");
        }
        if (t == rttr::type::get<std::chrono::minutes>())
        {
            return QStringLiteral("min");
        }
        if (t == rttr::type::get<std::chrono::hours>())
        {
            return QStringLiteral("h");
        }
        return {};
    }

    qint64 TypeRenderer::chronoCount(const rttr::variant& vIn)
    {
        const rttr::variant v = unwrap(vIn);
        const rttr::type t = v.get_type();
        if (t == rttr::type::get<std::chrono::nanoseconds>())
        {
            return static_cast<qint64>(v.get_value<std::chrono::nanoseconds>().count());
        }
        if (t == rttr::type::get<std::chrono::microseconds>())
        {
            return static_cast<qint64>(v.get_value<std::chrono::microseconds>().count());
        }
        if (t == rttr::type::get<std::chrono::milliseconds>())
        {
            return static_cast<qint64>(v.get_value<std::chrono::milliseconds>().count());
        }
        if (t == rttr::type::get<std::chrono::seconds>())
        {
            return static_cast<qint64>(v.get_value<std::chrono::seconds>().count());
        }
        if (t == rttr::type::get<std::chrono::minutes>())
        {
            return static_cast<qint64>(v.get_value<std::chrono::minutes>().count());
        }
        if (t == rttr::type::get<std::chrono::hours>())
        {
            return static_cast<qint64>(v.get_value<std::chrono::hours>().count());
        }
        return 0;
    }

    rttr::variant TypeRenderer::makeChronoDuration(rttr::type tIn, qint64 count)
    {
        const rttr::type t = rawType(tIn);
        if (t == rttr::type::get<std::chrono::nanoseconds>())
        {
            return rttr::variant(std::chrono::nanoseconds(count));
        }
        if (t == rttr::type::get<std::chrono::microseconds>())
        {
            return rttr::variant(std::chrono::microseconds(count));
        }
        if (t == rttr::type::get<std::chrono::milliseconds>())
        {
            return rttr::variant(std::chrono::milliseconds(count));
        }
        if (t == rttr::type::get<std::chrono::seconds>())
        {
            return rttr::variant(std::chrono::seconds(count));
        }
        if (t == rttr::type::get<std::chrono::minutes>())
        {
            return rttr::variant(std::chrono::minutes(static_cast<std::chrono::minutes::rep>(count)));
        }
        if (t == rttr::type::get<std::chrono::hours>())
        {
            return rttr::variant(std::chrono::hours(static_cast<std::chrono::hours::rep>(count)));
        }
        return {};
    }

    bool TypeRenderer::isFilePath(rttr::type t)
    {
        return rawType(t) == rttr::type::get<std::filesystem::path>();
    }

    qint64 TypeRenderer::enumBits(const rttr::variant& enumValue)
    {
        rttr::variant v = unwrap(enumValue);
        if (!v.get_type().is_enumeration())
        {
            return 0;
        }
        return v.convert(rttr::type::get<int64_t>()) ? v.get_value<int64_t>() : 0;
    }

    QString TypeRenderer::flagsToDisplayString(const rttr::variant& vIn)
    {
        const rttr::variant v = unwrap(vIn);
        const rttr::type t = v.get_type();
        if (!t.is_enumeration())
        {
            return toDisplayString(vIn);
        }
        const rttr::enumeration en = t.get_enumeration();

        // A value with its own name (single flag, or a named combination) shows as-is.
        const rttr::string_view exact = en.value_to_name(v);
        if (!exact.empty())
        {
            return QString::fromUtf8(exact.data(), static_cast<int>(exact.size()));
        }

        const qint64 bits = enumBits(v);

        // Collect (name, bits) for the named values, then greedily match, largest
        // masks first, so an "All = 7" umbrella wins over its individual bits and a
        // value only contributes when it adds bits not already covered.
        struct Flag
        {
            QString name;
            qint64 bits;
        };
        QVector<Flag> flags;
        for (const auto& nm : en.get_names())
        {
            const rttr::variant nv = en.name_to_value(nm);
            const qint64 nb = enumBits(nv);
            flags.append({ QString::fromUtf8(nm.data(), static_cast<int>(nm.size())), nb });
        }

        if (bits == 0)
        {
            // Prefer a name explicitly bound to 0 (e.g. "None"), else literal "0".
            for (const auto& f : flags)
            {
                if (f.bits == 0)
                {
                    return f.name;
                }
            }
            return QStringLiteral("0");
        }

        // Largest masks first (an umbrella like "All" wins over its bits), then by
        // bit value ascending so equal-width flags list in a stable, readable order.
        std::sort(flags.begin(), flags.end(), [](const Flag& a, const Flag& b) {
            const int pa = qPopulationCount(static_cast<quint64>(a.bits));
            const int pb = qPopulationCount(static_cast<quint64>(b.bits));
            return pa != pb ? pa > pb : a.bits < b.bits;
        });

        QStringList parts;
        qint64 covered = 0;
        for (const auto& f : flags)
        {
            if (f.bits != 0 && (bits & f.bits) == f.bits && (f.bits & ~covered) != 0)
            {
                parts << f.name;
                covered |= f.bits;
            }
        }
        const qint64 leftover = bits & ~covered;
        if (leftover != 0)
        {
            parts << (QStringLiteral("0x") + QString::number(static_cast<quint64>(leftover), 16));
        }
        return parts.isEmpty() ? QStringLiteral("0") : parts.join(QStringLiteral(" | "));
    }

    QString TypeRenderer::toDisplayString(const rttr::variant& vIn)
    {
        if (!vIn.is_valid())
        {
            return QStringLiteral("<invalid>");
        }

        // Registered std::optional<T>: show "(none)" when empty, else the inner value.
        if (OptionalBridge::isOptional(vIn.get_type()))
        {
            return OptionalBridge::hasValue(vIn) ? toDisplayString(vIn.extract_wrapped_value())
                                                 : QStringLiteral("(none)");
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
        if (t == rttr::type::get<std::wstring>())
        {
            return QString::fromStdWString(v.get_value<std::wstring>());
        }
        if (t == rttr::type::get<std::u16string>())
        {
            return QString::fromStdU16String(v.get_value<std::u16string>());
        }
        if (t == rttr::type::get<std::u32string>())
        {
            return QString::fromStdU32String(v.get_value<std::u32string>());
        }
        if (t == rttr::type::get<QString>())
        {
            return v.get_value<QString>();
        }
        if (t == rttr::type::get<QColor>())
        {
            return v.get_value<QColor>().name(QColor::HexArgb);
        }
        if (t == rttr::type::get<QDateTime>())
        {
            return v.get_value<QDateTime>().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        }
        if (isChronoDuration(t))
        {
            return QStringLiteral("%1 %2").arg(chronoCount(v)).arg(chronoSuffix(t));
        }
        if (t == rttr::type::get<std::string_view>())
        {
            // Read-only display: a view points into memory the OBJECT owns — showing
            // it is safe, editing it is not (and it is deliberately not editable).
            const std::string_view sv = v.get_value<std::string_view>();
            return sv.data() ? QString::fromUtf8(sv.data(), static_cast<int>(sv.size())) : QString();
        }
        if (t == rttr::type::get<std::wstring_view>())
        {
            // Same read-only rule as string_view.
            const std::wstring_view wv = v.get_value<std::wstring_view>();
            return wv.data() ? QString::fromWCharArray(wv.data(), static_cast<int>(wv.size())) : QString();
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

        if (t.is_associative_container())
        {
            auto view = v.create_associative_view();
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

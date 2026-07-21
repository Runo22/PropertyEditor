#include "rpe/core/RttrBridge.h"
#include "rpe/core/OptionalSupport.h"
#include "rpe/core/TypeRenderer.h"

#include <filesystem>

#include <rttr/variant_associative_view.h>
#include <rttr/variant_sequential_view.h>

namespace rpe::bridge
{

    QStringList splitPath(const QString& path)
    {
        QStringList out;
        for (const QString& seg : path.split(QLatin1Char('.'), Qt::SkipEmptyParts))
        {
            out.append(seg);
        }
        return out;
    }

    // "[…]" path segment: an array index ("[3]") or an associative key ("[alice]").
    static bool isBracketSegment(const QString& seg, QString& outInner)
    {
        if (seg.size() < 3 || !seg.startsWith(QLatin1Char('[')) || !seg.endsWith(QLatin1Char(']')))
        {
            return false;
        }
        outInner = seg.mid(1, seg.size() - 2);
        return true;
    }

    // Build an exact-typed key from its bracket-segment string form. String keys
    // pass through; everything else (arithmetic, enum) goes through RTTR's
    // string conversion. `keyType` is const so variant::convert unambiguously
    // resolves to convert(const type&) rather than the extract-into-T& template.
    static rttr::variant assocKeyFromString(const QString& keyStr, const rttr::type& keyType)
    {
        if (keyType == rttr::type::get<QString>())
        {
            return rttr::variant(keyStr);
        }
        if (keyType == rttr::type::get<std::string>())
        {
            return rttr::variant(keyStr.toStdString());
        }
        rttr::variant v(keyStr.toStdString());
        if (v.convert(keyType))
        {
            return v;
        }
        return {};
    }

    namespace
    {

        // Exact arithmetic re-typing. RTTR 0.9.6's built-in converter table is keyed on
        // the fixed-width typedefs, so e.g. `long` (= int64_t on LP64) ↔ `long long`
        // conversion FAILS even though both are 64-bit — and set_value then silently
        // rejects the write. Route integers through to_int64/to_uint64 and cast to the
        // target's precise C++ type instead.
        rttr::variant coerceArithmetic(const rttr::variant& valueIn, rttr::type raw)
        {
            using rttr::type;
            bool ok = false;

            // Normalize sources the converter table doesn't know either (long long /
            // unsigned long long are distinct from the fixed-width typedefs on LP64).
            rttr::variant value = valueIn;
            if (value.get_type() == type::get<long long>())
            {
                value = static_cast<int64_t>(valueIn.get_value<long long>());
            }
            else if (value.get_type() == type::get<unsigned long long>())
            {
                value = static_cast<uint64_t>(valueIn.get_value<unsigned long long>());
            }

            if (raw == type::get<bool>())
            {
                const auto v = value.to_int64(&ok);
                return ok ? rttr::variant(v != 0) : rttr::variant();
            }
            if (raw == type::get<float>())
            {
                const auto v = value.to_double(&ok);
                return ok ? rttr::variant(static_cast<float>(v)) : rttr::variant();
            }
            if (raw == type::get<double>())
            {
                const auto v = value.to_double(&ok);
                return ok ? rttr::variant(v) : rttr::variant();
            }

            if (raw == type::get<char>())
            {
                const auto v = value.to_int64(&ok);
                return ok ? rttr::variant(static_cast<char>(v)) : rttr::variant();
            }
            if (raw == type::get<signed char>())
            {
                const auto v = value.to_int64(&ok);
                return ok ? rttr::variant(static_cast<signed char>(v)) : rttr::variant();
            }
            if (raw == type::get<short>())
            {
                const auto v = value.to_int64(&ok);
                return ok ? rttr::variant(static_cast<short>(v)) : rttr::variant();
            }
            if (raw == type::get<int>())
            {
                const auto v = value.to_int64(&ok);
                return ok ? rttr::variant(static_cast<int>(v)) : rttr::variant();
            }
            if (raw == type::get<long>())
            {
                const auto v = value.to_int64(&ok);
                return ok ? rttr::variant(static_cast<long>(v)) : rttr::variant();
            }
            if (raw == type::get<long long>())
            {
                const auto v = value.to_int64(&ok);
                return ok ? rttr::variant(static_cast<long long>(v)) : rttr::variant();
            }
            if (raw == type::get<unsigned char>())
            {
                const auto v = value.to_uint64(&ok);
                return ok ? rttr::variant(static_cast<unsigned char>(v)) : rttr::variant();
            }
            if (raw == type::get<unsigned short>())
            {
                const auto v = value.to_uint64(&ok);
                return ok ? rttr::variant(static_cast<unsigned short>(v)) : rttr::variant();
            }
            if (raw == type::get<unsigned int>())
            {
                const auto v = value.to_uint64(&ok);
                return ok ? rttr::variant(static_cast<unsigned int>(v)) : rttr::variant();
            }
            if (raw == type::get<unsigned long>())
            {
                const auto v = value.to_uint64(&ok);
                return ok ? rttr::variant(static_cast<unsigned long>(v)) : rttr::variant();
            }
            if (raw == type::get<unsigned long long>())
            {
                const auto v = value.to_uint64(&ok);
                return ok ? rttr::variant(static_cast<unsigned long long>(v)) : rttr::variant();
            }

            return {};
        }

    } // namespace

    rttr::variant coerce(rttr::variant value, rttr::type target)
    {
        // Optional target: the editor produced an INNER value (target's rawType is
        // the inner type). Coerce it to the inner type, then wrap it into an engaged
        // std::optional<T>. (A value already of the optional type — e.g. an explicit
        // nullopt — is passed through untouched.) Do this before rawType() strips
        // the optional away.
        if (OptionalBridge::isOptional(target))
        {
            if (value.is_valid() && value.get_type() == target)
            {
                return value;
            }
            rttr::variant inner = coerce(value, OptionalBridge::innerType(target));
            return OptionalBridge::engage(target, inner);
        }

        const rttr::type raw = TypeRenderer::rawType(target);
        if (!value.is_valid() || value.get_type() == raw)
        {
            return value;
        }

        // chrono duration target: editors and mirrors hand over a plain number
        // (the tick count) — rebuild the exact duration type from it. RTTR has no
        // built-in arithmetic→duration conversion.
        if (TypeRenderer::isChronoDuration(raw))
        {
            bool ok = false;
            const int64_t n = value.to_int64(&ok);
            if (ok)
            {
                return TypeRenderer::makeChronoDuration(raw, n);
            }
        }

        // std::filesystem::path <- string: RTTR won't auto-convert, so build the
        // path ourselves. Editors produce a QString/std::string.
        if (raw == rttr::type::get<std::filesystem::path>())
        {
            // Build via UTF-16 / UTF-8 so non-ASCII paths survive on Windows (a plain
            // narrow std::string is decoded with the active ANSI codepage there).
            if (value.get_type() == rttr::type::get<QString>())
            {
                return rttr::variant(std::filesystem::path(value.get_value<QString>().toStdU16String()));
            }
            if (value.get_type() == rttr::type::get<std::string>())
            {
                return rttr::variant(std::filesystem::u8path(value.get_value<std::string>()));
            }
        }

        rttr::variant copy = value;
        if (copy.convert(raw))
        {
            return copy;
        }

        if (raw.is_arithmetic() && value.get_type().is_arithmetic())
        {
            rttr::variant exact = coerceArithmetic(value, raw);
            if (exact.is_valid())
            {
                return exact;
            }
        }

        return value; // let set_value attempt its own conversion
    }

    // Recursive set on a *mutable* variant. `i` is the index of the next segment.
    static bool setInVariant(rttr::variant& obj, const QStringList& segs, int i, const rttr::variant& value)
    {
        const QString& seg = segs[i];
        const bool isLast = (i == segs.size() - 1);

        QString bracket;
        if (isBracketSegment(seg, bracket))
        {
            if (obj.is_sequential_container())
            {
                bool okIdx = false;
                const int idx = bracket.toInt(&okIdx);
                auto view = obj.create_sequential_view();
                if (!okIdx || idx < 0 || idx >= static_cast<int>(view.get_size()))
                {
                    return false;
                }
                if (isLast)
                {
                    return view.set_value(idx, coerce(value, view.get_value_type()));
                }

                rttr::variant elem = TypeRenderer::unwrap(view.get_value(idx));
                if (!setInVariant(elem, segs, i + 1, value))
                {
                    return false;
                }
                return view.set_value(idx, elem);
            }
            if (obj.is_associative_container())
            {
                auto view = obj.create_associative_view();
                const rttr::variant key = assocKeyFromString(bracket, view.get_key_type());
                if (!key.is_valid() || view.is_key_only_type())
                {
                    return false;
                }
                auto it = view.find(key);
                if (it == view.end())
                {
                    return false;
                }
                rttr::variant elem = TypeRenderer::unwrap(it.get_value());
                if (isLast)
                {
                    elem = coerce(value, view.get_value_type());
                    if (!elem.is_valid())
                    {
                        return false;
                    }
                }
                else if (!setInVariant(elem, segs, i + 1, value))
                {
                    return false;
                }
                else if (elem.get_type().is_pointer())
                {
                    return true; // edited in place through the pointee — no re-insert
                }
                // The associative view has no set_value: replace = erase + insert.
                view.erase(key);
                return view.insert(key, elem).second;
            }
            return false;
        }

        rttr::type t = TypeRenderer::rawType(obj.get_type());
        rttr::property prop = t.get_property(seg.toStdString());
        if (!prop.is_valid())
        {
            return false;
        }

        rttr::instance inst(obj);
        if (isLast)
        {
            return prop.set_value(inst, coerce(value, prop.get_type()));
        }

        rttr::variant sub = TypeRenderer::unwrap(prop.get_value(inst));
        if (!setInVariant(sub, segs, i + 1, value))
        {
            return false;
        }
        if (sub.get_type().is_pointer())
        {
            // The sub-object was reached through a pointer (e.g. a shared_ptr
            // property): the write above mutated the pointee itself, and writing
            // the raw pointer back into the wrapper property would be wrong.
            return true;
        }
        return prop.set_value(inst, sub);
    }

    bool setValueByPath(rttr::instance root, const QString& path, const rttr::variant& value)
    {
        if (!root.is_valid())
        {
            return false;
        }
        const QStringList segs = splitPath(path);
        if (segs.isEmpty())
        {
            return false;
        }

        rttr::type t = root.get_derived_type();
        rttr::property prop = t.get_property(segs.first().toStdString());
        if (!prop.is_valid())
        {
            return false;
        }

        if (segs.size() == 1)
        {
            return prop.set_value(root, coerce(value, prop.get_type()));
        }

        rttr::variant sub = TypeRenderer::unwrap(prop.get_value(root));
        if (!setInVariant(sub, segs, 1, value))
        {
            return false;
        }
        if (sub.get_type().is_pointer())
        {
            return true; // mutated in place through the pointee (see setInVariant)
        }
        return prop.set_value(root, sub);
    }

    rttr::variant getValueByPath(const rttr::instance& root, const QString& path)
    {
        return getValueByPath(root, splitPath(path));
    }

    rttr::variant getValueByPath(const rttr::instance& root, const QStringList& segs)
    {
        if (!root.is_valid() || segs.isEmpty())
        {
            return {};
        }

        // First hop is against the live instance (no copy of the root).
        rttr::type t = root.get_derived_type();
        rttr::property prop = t.get_property(segs.first().toStdString());
        if (!prop.is_valid())
        {
            return {};
        }
        rttr::variant cur = TypeRenderer::unwrap(prop.get_value(root));

        for (int i = 1; i < segs.size() && cur.is_valid(); ++i)
        {
            const QString& seg = segs[i];
            QString bracket;
            if (isBracketSegment(seg, bracket))
            {
                if (cur.is_sequential_container())
                {
                    bool okIdx = false;
                    const int idx = bracket.toInt(&okIdx);
                    auto view = cur.create_sequential_view();
                    if (!okIdx || idx < 0 || idx >= static_cast<int>(view.get_size()))
                    {
                        return {};
                    }
                    cur = TypeRenderer::unwrap(view.get_value(idx));
                }
                else if (cur.is_associative_container())
                {
                    auto view = cur.create_associative_view();
                    const rttr::variant key = assocKeyFromString(bracket, view.get_key_type());
                    if (!key.is_valid())
                    {
                        return {};
                    }
                    auto it = view.find(key);
                    if (it == view.end())
                    {
                        return {};
                    }
                    cur = TypeRenderer::unwrap(it.get_value());
                }
                else
                {
                    return {};
                }
            }
            else
            {
                rttr::property p = TypeRenderer::rawType(cur.get_type()).get_property(seg.toStdString());
                if (!p.is_valid())
                {
                    return {};
                }
                rttr::instance inst(cur);
                cur = TypeRenderer::unwrap(p.get_value(inst));
            }
        }
        return cur;
    }

} // namespace rpe::bridge

#include "rpe/core/PropertySearch.h"

#include "rpe/core/EditorHints.h"
#include "rpe/core/TypeRenderer.h"

namespace rpe
{

    namespace
    {
        // The name the tree shows: the Label hint, else the property's name.
        QString displayName(const rttr::property& p)
        {
            const rttr::variant m = p.get_metadata(hint::Label);
            if (m.is_valid())
            {
                if (m.get_type() == rttr::type::get<const char*>())
                {
                    const char* c = m.get_value<const char*>();
                    if (c && *c)
                    {
                        return QString::fromUtf8(c);
                    }
                }
                else
                {
                    bool ok = false;
                    const std::string s = m.to_string(&ok);
                    if (ok && !s.empty())
                    {
                        return QString::fromStdString(s);
                    }
                }
            }
            return QString::fromStdString(p.get_name().to_string());
        }

        bool isFlags(const rttr::property& p)
        {
            const rttr::variant m = p.get_metadata(hint::Flags);
            return m.is_valid() && m.can_convert<bool>() && m.to_bool();
        }

        // Could the needle occur in a number's text ("12.500", "-3", "1e-05")? If it
        // has any other character it can't, and rendering the number is skipped — the
        // bulk of the cost of a name search like "speed" over many numeric fields.
        bool couldBeInNumber(const QString& needle)
        {
            for (const QChar c : needle)
            {
                if (!(c.isDigit() || c == QLatin1Char('.') || c == QLatin1Char('-') || c == QLatin1Char('+')
                      || c == QLatin1Char('e') || c == QLatin1Char('E') || c == QLatin1Char(',')))
                {
                    return false;
                }
            }
            return true;
        }

        bool isNumber(rttr::type t)
        {
            const rttr::type r = TypeRenderer::rawType(t);
            return r.is_arithmetic() && r != rttr::type::get<bool>();
        }

        // A container's items come wrapped (a reference to the element); anything
        // else is the value itself — extract_wrapped_value() would make it invalid.
        rttr::variant unwrapped(const rttr::variant& v)
        {
            return v.get_type().is_wrapper() ? v.extract_wrapped_value() : v;
        }

        struct Walker
        {
            const QString& needle;
            std::vector<PropertyHit>& out;
            const PropertySearchLimits& limits;
            const bool numeric = couldBeInNumber(needle);
            int rows = 0;
            int added = 0;

            bool has(const QString& text) const
            {
                return !text.isEmpty() && text.contains(needle, Qt::CaseInsensitive);
            }

            // How many rows INSIDE a container's value match: each element's text,
            // a map's keys, and — for elements that are structs, maps' values and
            // nested containers — their fields' names and values, as the tree shows
            // them once the container is expanded. Bounded by the limits.
            int countInside(const rttr::variant& v, int depth)
            {
                if (depth > limits.maxDepth || rows > limits.maxRows)
                {
                    return 0;
                }
                const rttr::variant inner = TypeRenderer::unwrap(unwrapped(v));
                if (!inner.is_valid())
                {
                    return 0;
                }
                const rttr::type t = TypeRenderer::rawType(inner.get_type());
                int n = 0;
                int seen = 0;
                if (inner.is_sequential_container())
                {
                    const rttr::variant_sequential_view view = inner.create_sequential_view();
                    if (!numeric && isNumber(view.get_value_type()))
                    {
                        return 0; // numbers can't contain this text
                    }
                    for (const rttr::variant& item : view)
                    {
                        if (++seen > limits.maxElements || ++rows > limits.maxRows)
                        {
                            break;
                        }
                        n += countElement(item, depth + 1);
                    }
                    return n;
                }
                if (inner.is_associative_container())
                {
                    const rttr::variant_associative_view view = inner.create_associative_view();
                    for (const auto& kv : view)
                    {
                        if (++seen > limits.maxElements || ++rows > limits.maxRows)
                        {
                            break;
                        }
                        // A row per key ("[key]"). A plain value is that same row; a
                        // struct / container value expands into rows below it.
                        const bool keyHit = has(TypeRenderer::toDisplayString(unwrapped(kv.first)));
                        const rttr::variant value = unwrapped(kv.second);
                        const int below = countElement(value, depth + 1);
                        n += expands(value) ? (keyHit ? 1 : 0) + below : ((keyHit || below > 0) ? 1 : 0);
                    }
                    return n;
                }
                if (TypeRenderer::isExpandable(t))
                {
                    // A struct: its fields' rows.
                    const rttr::instance obj(inner);
                    for (const rttr::property& p : t.get_properties())
                    {
                        if (++rows > limits.maxRows)
                        {
                            break;
                        }
                        const bool nameHit = has(displayName(p));
                        const rttr::type pt = TypeRenderer::rawType(p.get_type());
                        if (TypeRenderer::isExpandable(pt) || pt.is_sequential_container() || pt.is_associative_container())
                        {
                            n += (nameHit ? 1 : 0) + countInside(p.get_value(obj), depth + 1);
                            continue;
                        }
                        if (nameHit)
                        {
                            ++n;
                            continue;
                        }
                        if (!numeric && isNumber(pt))
                        {
                            continue;
                        }
                        n += valueMatches(p, p.get_value(obj)) ? 1 : 0;
                    }
                    return n;
                }
                return 0;
            }

            static bool expands(const rttr::variant& v)
            {
                const rttr::type t = TypeRenderer::rawType(v.get_type());
                return TypeRenderer::isExpandable(t) || t.is_sequential_container() || t.is_associative_container();
            }

            // One element of a container: a row of its own (its text) unless it
            // expands, in which case the rows below it count.
            int countElement(const rttr::variant& item, int depth)
            {
                const rttr::variant v = unwrapped(item);
                const rttr::type t = TypeRenderer::rawType(v.get_type());
                if (expands(v))
                {
                    return countInside(v, depth);
                }
                if (!numeric && isNumber(t))
                {
                    return 0;
                }
                return has(TypeRenderer::toDisplayString(v)) ? 1 : 0;
            }

            bool valueMatches(const rttr::property& p, const rttr::variant& v) const
            {
                if (!v.is_valid())
                {
                    return false;
                }
                return has((v.get_type().is_enumeration() && isFlags(p)) ? TypeRenderer::flagsToDisplayString(v)
                                                                       : TypeRenderer::toDisplayString(v));
            }

            void walk(const rttr::instance& obj, rttr::type type, const QString& prefix)
            {
                for (const rttr::property& p : TypeRenderer::rawType(type).get_properties())
                {
                    if (++rows > limits.maxRows)
                    {
                        return;
                    }
                    const QString name = displayName(p);
                    const QString path = prefix.isEmpty() ? QString::fromStdString(p.get_name().to_string())
                                                          : prefix + QLatin1Char('.') + QString::fromStdString(p.get_name().to_string());
                    const rttr::type pt = p.get_type();
                    const bool nameHit = has(name);
                    const rttr::type raw = TypeRenderer::rawType(pt);

                    if (raw.is_sequential_container() || raw.is_associative_container())
                    {
                        // Reading a container COPIES it — skip that when its text
                        // can't match anyway: numbers only, and a needle that can't
                        // occur in a number.
                        if (!nameHit && !numeric)
                        {
                            const auto args = raw.get_template_arguments();
                            bool allNumbers = args.begin() != args.end();
                            for (const rttr::type& a : args)
                            {
                                allNumbers = allNumbers && isNumber(a);
                            }
                            if (allNumbers)
                            {
                                continue;
                            }
                        }
                        const rttr::variant v = p.get_value(obj);
                        const int inside = countInside(v, 0);
                        if (nameHit || inside > 0)
                        {
                            out.push_back({ path, v, (nameHit ? 1 : 0) + inside });
                            ++added;
                        }
                        continue;
                    }
                    if (TypeRenderer::isExpandable(pt))
                    {
                        const rttr::variant v = p.get_value(obj);
                        if (nameHit)
                        {
                            out.push_back({ path, v, 1 }); // the struct as a whole
                            ++added;
                            continue;
                        }
                        const rttr::variant inner = TypeRenderer::unwrap(v);
                        if (inner.is_valid())
                        {
                            walk(rttr::instance(inner), inner.get_type(), path);
                        }
                        continue;
                    }
                    if (!nameHit && !numeric && isNumber(pt))
                    {
                        continue; // its text can't match — don't even read it
                    }
                    const rttr::variant v = p.get_value(obj);
                    if (nameHit)
                    {
                        out.push_back({ path, v, 1 });
                        ++added;
                        continue;
                    }
                    if (valueMatches(p, v))
                    {
                        out.push_back({ path, v, 1 });
                        ++added;
                    }
                }
            }
        };
    } // namespace

    int searchProperties(const rttr::instance& obj, rttr::type type, const QString& needleIn,
                         std::vector<PropertyHit>& out, const PropertySearchLimits& limits)
    {
        const QString needle = needleIn.trimmed();
        if (needle.isEmpty() || !obj.is_valid() || !type.is_valid())
        {
            return 0;
        }
        Walker w { needle, out, limits };
        w.walk(obj, type, QString());
        return w.added;
    }

} // namespace rpe

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

            // Any element's (or key's) text of a container matches.
            bool containerMatches(const rttr::variant& v)
            {
                int seen = 0;
                if (v.is_sequential_container())
                {
                    const rttr::variant_sequential_view view = v.create_sequential_view();
                    if (!numeric && isNumber(view.get_value_type()))
                    {
                        return false; // numbers can't contain this text
                    }
                    for (const rttr::variant& item : view)
                    {
                        if (++seen > limits.maxElements)
                            break;
                        if (has(TypeRenderer::toDisplayString(item.extract_wrapped_value())))
                            return true;
                    }
                }
                else if (v.is_associative_container())
                {
                    const rttr::variant_associative_view view = v.create_associative_view();
                    for (const auto& kv : view)
                    {
                        if (++seen > limits.maxElements)
                            break;
                        if (has(TypeRenderer::toDisplayString(kv.first.extract_wrapped_value()))
                            || has(TypeRenderer::toDisplayString(kv.second.extract_wrapped_value())))
                            return true;
                    }
                }
                return false;
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
                        const rttr::variant v = p.get_value(obj);
                        if (nameHit || containerMatches(v))
                        {
                            out.push_back({ path, v });
                            ++added;
                        }
                        continue;
                    }
                    if (TypeRenderer::isExpandable(pt))
                    {
                        const rttr::variant v = p.get_value(obj);
                        if (nameHit)
                        {
                            out.push_back({ path, v }); // the struct as a whole
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
                        out.push_back({ path, v });
                        ++added;
                        continue;
                    }
                    if (!v.is_valid())
                    {
                        continue;
                    }
                    const QString text = (v.get_type().is_enumeration() && isFlags(p)) ? TypeRenderer::flagsToDisplayString(v)
                                                                                         : TypeRenderer::toDisplayString(v);
                    if (has(text))
                    {
                        out.push_back({ path, v });
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

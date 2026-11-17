#include "rpe/core/NamedElement.h"

#include "rpe/core/RttrBridge.h"
#include "rpe/core/TypeBridge.h"
#include "rpe/core/TypeRenderer.h"

#include <rttr/variant_sequential_view.h>

namespace rpe
{

    namespace
    {
        // A sequence's element type from its template arguments (invalid if none).
        rttr::type sequenceElementType(rttr::type container)
        {
            const rttr::type r = TypeRenderer::rawType(container);
            if (r.is_sequential_container())
            {
                for (const rttr::type& a : r.get_template_arguments())
                {
                    return a;
                }
            }
            return rttr::type::get_by_name(std::string());
        }
    } // namespace

    NamedElement namedElement(rttr::type elementType)
    {
        NamedElement ne;
        const rttr::type t = TypeRenderer::rawType(elementType);
        if (!t.is_valid() || t.is_sequential_container() || t.is_associative_container())
        {
            return ne;
        }
        const TypeBridge::NamedElementFields f = TypeBridge::namedElementFields(t);
        if (f.label.empty())
        {
            return ne;
        }
        ne.label = t.get_property(f.label);
        if (ne.label.is_valid() && !f.value.empty() && f.value != f.label)
        {
            ne.value = t.get_property(f.value);
        }
        return ne;
    }

    QString namedElementLabel(const NamedElement& ne, const rttr::variant& element)
    {
        if (!ne.isValid())
        {
            return {};
        }
        const rttr::variant v = TypeRenderer::unwrap(element);
        if (!v.is_valid())
        {
            return {};
        }
        const rttr::instance obj(v);
        return TypeRenderer::toDisplayString(ne.label.get_value(obj));
    }

    int findNamedElement(const rttr::variant& containerIn, const QString& label)
    {
        const rttr::variant container = TypeRenderer::unwrap(containerIn);
        if (!container.is_sequential_container())
        {
            return -1;
        }
        const rttr::variant_sequential_view view = container.create_sequential_view();
        const NamedElement ne = namedElement(view.get_value_type());
        if (!ne.isValid())
        {
            return -1;
        }
        int i = 0;
        for (const rttr::variant& item : view)
        {
            if (namedElementLabel(ne, item) == label)
            {
                return i;
            }
            ++i;
        }
        return -1;
    }

    QString displayPath(rttr::type root, const QString& path)
    {
        QStringList out;
        rttr::type t = root;
        NamedElement afterName; // set right after a "{name}" step: its value field is dropped
        for (const QString& seg : bridge::splitPath(path))
        {
            const bool named = seg.size() >= 2 && seg.startsWith(QLatin1Char('{')) && seg.endsWith(QLatin1Char('}'));
            if (named || seg.startsWith(QLatin1Char('[')))
            {
                t = t.is_valid() ? sequenceElementType(t) : t;
                out << (named ? seg.mid(1, seg.size() - 2) : seg);
                afterName = named && t.is_valid() ? namedElement(t) : NamedElement();
                continue;
            }
            const bool skip = afterName.isValid() && afterName.value.is_valid()
                && seg == QString::fromStdString(afterName.value.get_name().to_string());
            afterName = NamedElement();
            const rttr::property p = t.is_valid() ? TypeRenderer::rawType(t).get_property(seg.toStdString())
                                                  : rttr::type::get<void>().get_property(std::string());
            t = p.is_valid() ? p.get_type() : rttr::type::get_by_name(std::string());
            if (!skip)
            {
                out << seg;
            }
        }
        return out.join(QLatin1Char('.'));
    }

    namespace
    {
        void collectLabels(const rttr::variant& in, const QString& path, QHash<QString, QStringList>& out, int depth)
        {
            constexpr int kMaxDepth = 4;
            constexpr int kMaxElements = 4096;
            if (depth > kMaxDepth)
            {
                return;
            }
            const rttr::variant v = TypeRenderer::unwrap(in.get_type().is_wrapper() ? in.extract_wrapped_value() : in);
            if (!v.is_valid())
            {
                return;
            }
            if (v.is_sequential_container())
            {
                const rttr::variant_sequential_view view = v.create_sequential_view();
                const rttr::type et = TypeRenderer::rawType(view.get_value_type());
                if (!TypeRenderer::isExpandable(et))
                {
                    return; // plain values: no names anywhere below
                }
                const NamedElement ne = namedElement(et);
                QStringList names;
                int i = 0;
                for (const rttr::variant& item : view)
                {
                    if (i >= kMaxElements)
                    {
                        break;
                    }
                    if (ne.isValid())
                    {
                        names << namedElementLabel(ne, item);
                    }
                    collectLabels(item, path + QStringLiteral(".[%1]").arg(i), out, depth + 1);
                    ++i;
                }
                if (ne.isValid())
                {
                    out.insert(path, names);
                }
                return;
            }
            const rttr::type t = TypeRenderer::rawType(v.get_type());
            if (v.is_associative_container() || t.get_properties().empty())
            {
                return;
            }
            const rttr::instance obj(v);
            for (const rttr::property& p : t.get_properties())
            {
                const rttr::type pt = TypeRenderer::rawType(p.get_type());
                if (pt.is_sequential_container() || (!pt.is_associative_container() && !pt.get_properties().empty()))
                {
                    collectLabels(p.get_value(obj), path + QLatin1Char('.') + QString::fromStdString(p.get_name().to_string()),
                                  out, depth + 1);
                }
            }
        }
    } // namespace

    void collectNamedLabels(const rttr::variant& value, const QString& path, QHash<QString, QStringList>& out)
    {
        collectLabels(value, path, out, 0);
    }

    bool isNamedLabelPath(rttr::type root, const QString& path)
    {
        const QStringList segs = bridge::splitPath(path);
        if (segs.size() < 2)
        {
            return false;
        }
        rttr::type t = root;
        for (int i = 0; i < segs.size(); ++i)
        {
            const QString& seg = segs[i];
            if (seg.startsWith(QLatin1Char('[')) || seg.startsWith(QLatin1Char('{')))
            {
                t = sequenceElementType(t);
                if (!t.is_valid())
                {
                    return false;
                }
                continue;
            }
            const rttr::property p = TypeRenderer::rawType(t).get_property(seg.toStdString());
            if (!p.is_valid())
            {
                return false;
            }
            if (i == segs.size() - 1)
            {
                const NamedElement ne = namedElement(t);
                return ne.isValid() && ne.label == p;
            }
            t = p.get_type();
        }
        return false;
    }

    rttr::variant ownedText(const rttr::variant& v)
    {
        const rttr::type t = v.get_type();
        if (t == rttr::type::get<std::string_view>() || t == rttr::type::get<std::wstring_view>()
            || t == rttr::type::get<const char*>() || t == rttr::type::get<char*>())
        {
            return rttr::variant(TypeRenderer::toDisplayString(v));
        }
        return v;
    }

} // namespace rpe

#include "rpe/core/ReadOnly.h"

#include "rpe/core/EditorHints.h"
#include "rpe/core/RttrBridge.h"
#include "rpe/core/TypeBridge.h"
#include "rpe/core/TypeRenderer.h"

#include <vector>

namespace rpe
{

    namespace
    {
        QString typeName(rttr::type t)
        {
            return QString::fromStdString(t.get_name().to_string());
        }

        // A locked type, by either route (TypeBridge or RTTR class metadata).
        QString typeLockReason(rttr::type t)
        {
            const rttr::type r = TypeRenderer::rawType(t);
            if (!r.is_valid())
            {
                return {};
            }
            if (TypeBridge::isReadOnly(r))
            {
                return QStringLiteral("Read-only: type %1 is locked").arg(typeName(r));
            }
            const rttr::variant m = r.get_metadata(hint::ReadOnly);
            if (m.is_valid() && m.to_bool())
            {
                return QStringLiteral("Read-only: type %1 is marked read-only").arg(typeName(r));
            }
            return {};
        }

        // Element type of a container step: a sequence's T, an associative
        // container's mapped value (or key, for a set). Invalid when RTTR doesn't
        // expose template arguments for it.
        rttr::type elementType(rttr::type container)
        {
            const rttr::type r = TypeRenderer::rawType(container);
            std::vector<rttr::type> args;
            for (const rttr::type& a : r.get_template_arguments())
            {
                args.push_back(a);
            }
            if (r.is_associative_container())
            {
                return args.size() >= 2 ? args[1] : (args.empty() ? rttr::type::get_by_name(std::string()) : args[0]);
            }
            if (r.is_sequential_container() && !args.empty())
            {
                return args[0];
            }
            return rttr::type::get_by_name(std::string()); // invalid
        }
    } // namespace

    QString readOnlyReason(rttr::type root, const QString& path)
    {
        QString why = typeLockReason(root);
        if (!why.isEmpty())
        {
            return why;
        }
        rttr::type t = root;
        for (const QString& seg : bridge::splitPath(path))
        {
            if (seg.startsWith(QLatin1Char('[')))
            {
                t = elementType(t);
                if (!t.is_valid())
                {
                    return {}; // can't see further — the write validates itself
                }
            }
            else
            {
                const rttr::property p = TypeRenderer::rawType(t).get_property(seg.toStdString());
                if (!p.is_valid())
                {
                    return {};
                }
                if (p.is_readonly())
                {
                    return QStringLiteral("Read-only: %1 has no setter").arg(seg);
                }
                const rttr::variant m = p.get_metadata(hint::ReadOnly);
                if (m.is_valid() && m.to_bool())
                {
                    return QStringLiteral("Read-only: %1 is marked read-only").arg(seg);
                }
                t = p.get_type();
            }
            why = typeLockReason(t);
            if (!why.isEmpty())
            {
                return why;
            }
        }
        return {};
    }

} // namespace rpe

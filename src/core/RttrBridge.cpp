#include "rpe/core/RttrBridge.h"
#include "rpe/core/TypeRenderer.h"

#include <rttr/variant_sequential_view.h>

namespace rpe::bridge {

QStringList splitPath(const QString& path)
{
    QStringList out;
    for (const QString& seg : path.split(QLatin1Char('.'), Qt::SkipEmptyParts))
        out.append(seg);
    return out;
}

static bool isIndexSegment(const QString& seg, int& outIndex)
{
    if (seg.size() < 3 || !seg.startsWith(QLatin1Char('[')) || !seg.endsWith(QLatin1Char(']')))
        return false;
    bool ok = false;
    outIndex = seg.mid(1, seg.size() - 2).toInt(&ok);
    return ok;
}

rttr::variant coerce(rttr::variant value, rttr::type target)
{
    const rttr::type raw = TypeRenderer::rawType(target);
    if (!value.is_valid() || value.get_type() == raw)
        return value;
    rttr::variant copy = value;
    if (copy.convert(raw))
        return copy;
    return value; // let set_value attempt its own conversion
}

// Recursive set on a *mutable* variant. `i` is the index of the next segment.
static bool setInVariant(rttr::variant& obj, const QStringList& segs, int i,
                         const rttr::variant& value)
{
    const QString& seg    = segs[i];
    const bool     isLast = (i == segs.size() - 1);

    int idx = -1;
    if (isIndexSegment(seg, idx)) {
        if (!obj.is_sequential_container())
            return false;
        auto view = obj.create_sequential_view();
        if (idx < 0 || idx >= static_cast<int>(view.get_size()))
            return false;
        if (isLast)
            return view.set_value(idx, coerce(value, view.get_value_type()));

        rttr::variant elem = TypeRenderer::unwrap(view.get_value(idx));
        if (!setInVariant(elem, segs, i + 1, value))
            return false;
        return view.set_value(idx, elem);
    }

    rttr::type    t    = TypeRenderer::rawType(obj.get_type());
    rttr::property prop = t.get_property(seg.toStdString());
    if (!prop.is_valid())
        return false;

    rttr::instance inst(obj);
    if (isLast)
        return prop.set_value(inst, coerce(value, prop.get_type()));

    rttr::variant sub = TypeRenderer::unwrap(prop.get_value(inst));
    if (!setInVariant(sub, segs, i + 1, value))
        return false;
    return prop.set_value(inst, sub);
}

bool setValueByPath(rttr::instance root, const QString& path, const rttr::variant& value)
{
    if (!root.is_valid())
        return false;
    const QStringList segs = splitPath(path);
    if (segs.isEmpty())
        return false;

    rttr::type     t    = root.get_derived_type();
    rttr::property prop = t.get_property(segs.first().toStdString());
    if (!prop.is_valid())
        return false;

    if (segs.size() == 1)
        return prop.set_value(root, coerce(value, prop.get_type()));

    rttr::variant sub = TypeRenderer::unwrap(prop.get_value(root));
    if (!setInVariant(sub, segs, 1, value))
        return false;
    return prop.set_value(root, sub);
}

rttr::variant getValueByPath(const rttr::instance& root, const QString& path)
{
    if (!root.is_valid())
        return {};
    const QStringList segs = splitPath(path);
    if (segs.isEmpty())
        return {};

    // First hop is against the live instance (no copy of the root).
    rttr::type     t    = root.get_derived_type();
    rttr::property prop = t.get_property(segs.first().toStdString());
    if (!prop.is_valid())
        return {};
    rttr::variant cur = TypeRenderer::unwrap(prop.get_value(root));

    for (int i = 1; i < segs.size() && cur.is_valid(); ++i) {
        const QString& seg = segs[i];
        int idx = -1;
        if (isIndexSegment(seg, idx)) {
            if (!cur.is_sequential_container()) return {};
            auto view = cur.create_sequential_view();
            if (idx < 0 || idx >= static_cast<int>(view.get_size())) return {};
            cur = TypeRenderer::unwrap(view.get_value(idx));
        } else {
            rttr::property p = TypeRenderer::rawType(cur.get_type()).get_property(seg.toStdString());
            if (!p.is_valid()) return {};
            rttr::instance inst(cur);
            cur = TypeRenderer::unwrap(p.get_value(inst));
        }
    }
    return cur;
}

} // namespace rpe::bridge

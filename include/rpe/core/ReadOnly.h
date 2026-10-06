#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QString>

namespace rpe
{

    // Why the leaf/subtree at `path` inside a value of type `root` cannot be
    // written — or an empty string when it can. The ONE rule every writer consults
    // (the property tree, the watch list, and the mirror on the sim thread), so a
    // row that looks editable is editable, and one that isn't says why.
    //
    // Walking the path from `root`, it is read-only as soon as any step is:
    //   • a property RTTR knows has no setter (a getter-only property) — editing it,
    //     or anything below it, would write into a temporary copy and vanish;
    //   • a property carrying the rpe::hint::ReadOnly metadata;
    //   • of a type locked with TypeBridge::setReadOnly, or registered with the
    //     rpe::hint::ReadOnly CLASS metadata — wherever that type appears.
    //
    // Array/map element steps ("[3]", "[key]") are followed through the
    // container's element type when RTTR exposes it; past a step it can't see
    // into, the answer is "writable" (the write itself still validates).
    QString readOnlyReason(rttr::type root, const QString& path);

} // namespace rpe

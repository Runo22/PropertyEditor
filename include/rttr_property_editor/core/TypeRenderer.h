#pragma once

#include <QString>
#include <rttr/variant>
#include <rttr/type>

namespace rpe {

// Converts an rttr::variant to a human-readable display string.
// Extracted here so it can be reused by the delegate and tested independently.
class TypeRenderer
{
public:
    // Returns a display string for the given variant.
    // Falls back to the type name for unrecognized types.
    static QString toDisplayString(const rttr::variant& v);

    // Returns true if this type should expand into child rows (struct / sequence).
    static bool isExpandable(rttr::type t);

    // Returns true if this type is a sequential container (e.g. std::vector<T>).
    static bool isSequential(rttr::type t);
};

} // namespace rpe

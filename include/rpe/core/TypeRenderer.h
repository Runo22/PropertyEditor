#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QString>

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  TypeRenderer — converts rttr values to display strings and answers structural
    //  questions about types. Stateless; reused by the model and delegate.
    // ─────────────────────────────────────────────────────────────────────────────
    class TypeRenderer
    {
    public:
        // Human-readable one-line representation of a value.
        static QString toDisplayString(const rttr::variant& v);

        // True if the type should expand into child rows (struct or sequence).
        static bool isExpandable(rttr::type t);

        // True if the type is a sequential container (std::vector<T>, arrays, …).
        static bool isSequential(rttr::type t);

        // True if the type maps to a built-in inline editor (number/bool/string/enum).
        static bool isInlineEditable(rttr::type t);

        // Resolve a possibly-wrapped type to its underlying value type.
        static rttr::type rawType(rttr::type t);

        // Unwrap a possibly reference-wrapped variant to its value.
        static rttr::variant unwrap(const rttr::variant& v);
    };

} // namespace rpe

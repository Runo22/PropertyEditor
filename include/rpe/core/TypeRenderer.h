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

        // How many digits floats/doubles show after the decimal point (default 3).
        // Values are formatted fixed-point — never scientific notation — with
        // trailing zeros trimmed; anything that rounds to zero displays as "0"
        // (e.g. 2.5e-05 → "0", 0.5 → "0.5", 1.2345 → "1.234" at the default).
        static void setFloatDecimals(int decimals);
        static int floatDecimals();

        // True if the type should expand into child rows (struct or sequence).
        static bool isExpandable(rttr::type t);

        // True if the type is a sequential container (std::vector<T>, arrays, …).
        static bool isSequential(rttr::type t);

        // True if the type maps to a built-in inline editor (number/bool/string/enum).
        static bool isInlineEditable(rttr::type t);

        // True if the (raw) type is std::filesystem::path — gets a path line-edit +
        // browse button automatically, no editor hint required.
        static bool isFilePath(rttr::type t);

        // Resolve a possibly-wrapped type to its underlying value type.
        static rttr::type rawType(rttr::type t);

        // Unwrap a possibly reference-wrapped variant to its value.
        static rttr::variant unwrap(const rttr::variant& v);
    };

} // namespace rpe

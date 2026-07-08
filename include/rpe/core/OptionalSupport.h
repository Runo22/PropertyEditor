#pragma once

#include "rpe/core/rttr_prelude.h"

#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
//  std::optional<T> support for the property editor.
//
//  RTTR 0.9.6 knows nothing about std::optional. Two pieces make it work:
//
//   1. A wrapper_mapper specialization (below) so RTTR treats std::optional<T> as
//      a wrapper whose wrapped type is T — the inner value is reachable for
//      reading/editing, and reading an EMPTY optional yields a default T instead
//      of throwing (safe).
//
//   2. A tiny registration, RPE_REGISTER_OPTIONAL(T), that captures the
//      compile-time T so the editor can (a) tell whether an optional is engaged
//      (to show "(none)"), (b) build an engaged std::optional<T> from an edited
//      inner value, and (c) build a disengaged one (nullopt). This mirrors how
//      component types are registered with TypeBridge — put it next to your RTTR
//      registration, where T is complete:
//
//          rttr::registration::class_<Enemy>("Enemy").property("aggro", &Enemy::aggro);
//          RPE_REGISTER_OPTIONAL(float);   // once per inner type you use
//
//  Without the RPE_REGISTER_OPTIONAL call an optional still reads safely (via the
//  wrapper) but cannot be edited or shown as unset.
// ─────────────────────────────────────────────────────────────────────────────

namespace rttr
{
    // Make std::optional<T> an RTTR wrapper over T. get() never throws (default T
    // when empty); create() rebuilds an engaged optional.
    template <typename T>
    struct wrapper_mapper<std::optional<T>>
    {
        using wrapped_type = T;
        using type = std::optional<T>;
        static wrapped_type get(const type& obj)
        {
            return obj ? *obj : T {};
        }
        static type create(const wrapped_type& value)
        {
            return type(value);
        }
    };
} // namespace rttr

namespace rpe
{

    class OptionalBridge
    {
    public:
        using HasValueFn = bool (*)(const rttr::variant&);           // is it engaged?
        using EngageFn = rttr::variant (*)(const rttr::variant&);    // inner value -> optional<T>(value)
        using DisengageFn = rttr::variant (*)();                     // -> optional<T>{} (nullopt)

        static void registerEntry(rttr::type optType, rttr::type innerType,
                                  HasValueFn hasValue, EngageFn engage, DisengageFn disengage);

        template <class T>
        static void registerType()
        {
            registerEntry(
                rttr::type::get<std::optional<T>>(),
                rttr::type::get<T>(),
                +[](const rttr::variant& v) -> bool {
                    return v.get_type() == rttr::type::get<std::optional<T>>()
                        && v.get_value<std::optional<T>>().has_value();
                },
                +[](const rttr::variant& inner) -> rttr::variant {
                    if (inner.get_type() == rttr::type::get<T>())
                    {
                        return rttr::variant(std::optional<T>(inner.get_value<T>()));
                    }
                    rttr::variant c = inner;
                    if (c.convert(rttr::type::get<T>()))
                    {
                        return rttr::variant(std::optional<T>(c.get_value<T>()));
                    }
                    return {};
                },
                +[]() -> rttr::variant { return rttr::variant(std::optional<T>{}); });
        }

        // True if `t` is a std::optional that has been registered (RPE_REGISTER_OPTIONAL).
        static bool isOptional(rttr::type t);
        // The inner value type of a registered optional (invalid if not one).
        static rttr::type innerType(rttr::type optType);
        // Whether the given std::optional<T> variant is engaged (false if empty or
        // not a registered optional).
        static bool hasValue(const rttr::variant& optVariant);
        // Build an engaged std::optional<T> holding `innerValue` (invalid on failure).
        static rttr::variant engage(rttr::type optType, const rttr::variant& innerValue);
        // Build a disengaged std::optional<T> (nullopt); invalid if not registered.
        static rttr::variant disengage(rttr::type optType);
    };

} // namespace rpe

// Register std::optional<T> so its members are editable and can show "(none)".
#define RPE_REGISTER_OPTIONAL(T) ::rpe::OptionalBridge::registerType<T>()

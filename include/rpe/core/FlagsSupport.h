#pragma once

#include "rpe/core/rttr_prelude.h"

#include <cstdint>
#include <type_traits>

// ─────────────────────────────────────────────────────────────────────────────
//  Bitmask (flags) enum support.
//
//  Mark an enumeration property with the hint::Flags metadata to have it shown
//  decomposed ("Fire | Poison") and edited through a multi-check editor:
//
//      .property("damage", &Enemy::damage)(
//          rttr::metadata(rpe::hint::Flags, true))
//
//  DISPLAY works with just that hint. EDITING a combined value additionally needs
//  the enum registered here, once, next to your RTTR registration:
//
//      RPE_REGISTER_FLAGS(Damage);
//
//  Why: RTTR 0.9.6 can turn an enum INTO its integer, but has no way to build an
//  enum FROM an arbitrary integer (a combined mask has no registered name, and
//  variant::convert(int → enum) fails). This registration captures the compile-
//  time enum type so `static_cast<T>(bits)` can produce the value RTTR can't —
//  mirroring how OptionalBridge / registerPair capture their inner types.
// ─────────────────────────────────────────────────────────────────────────────

namespace rpe
{

    class FlagsBridge
    {
    public:
        using BuildFn = rttr::variant (*)(int64_t bits); // -> variant(static_cast<T>(bits))

        static void registerEntry(rttr::type enumType, BuildFn build);

        template <class T>
        static void registerType()
        {
            static_assert(std::is_enum_v<T>, "RPE_REGISTER_FLAGS requires an enum type");
            registerEntry(rttr::type::get<T>(), +[](int64_t bits) -> rttr::variant {
                using U = std::underlying_type_t<T>;
                return rttr::variant(static_cast<T>(static_cast<U>(bits)));
            });
        }

        // True if this enum type has a registered builder (RPE_REGISTER_FLAGS).
        static bool canBuild(rttr::type enumType);
        // Build an enum variant holding `bits` (invalid if the type isn't registered).
        static rttr::variant build(rttr::type enumType, int64_t bits);
    };

} // namespace rpe

// Enable editing of combined flag values for enum type T.
#define RPE_REGISTER_FLAGS(T) ::rpe::FlagsBridge::registerType<T>()

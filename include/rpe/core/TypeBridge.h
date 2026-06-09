#pragma once

#include "rpe/core/rttr_prelude.h"

#include <functional>


namespace rpe {

// ─────────────────────────────────────────────────────────────────────────────
//  TypeBridge — maps an rttr::type to a function that wraps a raw `void*` into
//  an rttr::variant holding a correctly-typed `T*`.
//
//  Why this exists: RTTR has no public "make an instance from (type, void*)"
//  API. But a variant holding a `T*` behaves as an instance of `T` (RTTR
//  dereferences it), and get_value/set_value through it read/write the real
//  object. The only piece that needs the compile-time `T` is producing that
//  typed pointer — so we register a one-line wrapper per type:
//
//      rpe::TypeBridge::registerType<TransformComponent>();
//      rpe::TypeBridge::registerTypes<A, B, C>();
//
//  This is required for editing engine/ECS data referenced by raw pointer.
//  The owned-variant path (VariantEditor::setVariant) does NOT need it.
// ─────────────────────────────────────────────────────────────────────────────
class TypeBridge
{
public:
    using Wrapper = std::function<rttr::variant(void*)>;

    static void registerWrapper(rttr::type t, Wrapper w);

    template <class T>
    static void registerType()
    {
        registerWrapper(rttr::type::get<T>(),
                        [](void* p) { return rttr::variant(static_cast<T*>(p)); });
    }

    template <class... Ts>
    static void registerTypes()
    {
        (registerType<Ts>(), ...);
    }

    // Returns a variant holding a typed pointer to *obj*, or an invalid variant
    // if no wrapper is registered for `t`.
    static rttr::variant wrap(rttr::type t, void* obj);

    static bool has(rttr::type t);
};

} // namespace rpe

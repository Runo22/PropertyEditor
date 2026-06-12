#pragma once

#include "rpe/core/rttr_prelude.h"

#include <functional>
#include <vector>

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  TypeBridge — process-global registry that, for an rttr::type, knows how to:
    //    • wrap a raw `void*` as an rttr::variant holding a correctly-typed `T*`
    //      (so it behaves as an instance of T for get/set), and
    //    • clone the pointee into a self-contained value variant (deep copy, used
    //      by the threading mirror to snapshot a component without touching it on
    //      the GUI thread).
    //
    //  Why a registry exists: RTTR has no public "make an instance from
    //  (type, void*)" API, and no way to copy a runtime-typed object from a void*.
    //  Both need the compile-time `T`, captured by a one-line registration:
    //
    //      rpe::TypeBridge::registerType<TransformComponent>();
    //      rpe::TypeBridge::registerTypes<A, B, C>();
    //      RPE_REGISTER_COMPONENT(TransformComponent);   // equivalent macro
    //
    //  ── Plugin notes ────────────────────────────────────────────────────────────
    //  • The registry is a single process-global, independent of any widget. Put
    //    the registration call next to your RTTR registration (same translation
    //    unit, where T is complete), so the two share one lifetime.
    //  • registerType is idempotent — re-registering a type just overwrites its
    //    entry, so plugin add/remove/add cycles are harmless.
    //  • unregisterType only removes the bridge entry; it does NOT touch RTTR's own
    //    registration. You rarely need it: RTTR has no unregister and its property
    //    accessors point into the defining module, so the safe pattern is to keep
    //    RTTR + bridge registrations alive for the process (host-owned).
    //  • SINGLE-REGISTRY REQUIREMENT: build rpe_core as a SHARED library so the host
    //    and every plugin link the same registry instance. With a static rpe_core
    //    linked separately into each module, each gets its own copy and the browser
    //    won't see types a plugin registered.
    // ─────────────────────────────────────────────────────────────────────────────
    class TypeBridge
    {
    public:
        using Wrapper = std::function<rttr::variant(void*)>; // void* -> variant(T*)
        using Cloner = std::function<rttr::variant(void*)>;  // void* -> variant(T by value)

        static void registerEntry(rttr::type t, Wrapper wrap, Cloner clone);

        template <class T>
        static void registerType()
        {
            registerEntry(rttr::type::get<T>(), [](void* p) { return rttr::variant(static_cast<T*>(p)); }, [](void* p) { return rttr::variant(*static_cast<T*>(p)); });
        }

        template <class... Ts>
        static void registerTypes()
        {
            (registerType<Ts>(), ...);
        }

        // Remove only the bridge entry for `t` (RTTR registration is untouched).
        static void unregisterType(rttr::type t);
        template <class T>
        static void unregisterType()
        {
            unregisterType(rttr::type::get<T>());
        }

        // Wrap `obj` as a variant holding a typed pointer (invalid if unregistered).
        static rttr::variant wrap(rttr::type t, void* obj);

        // Deep-copy the pointee into a self-contained value variant (invalid if
        // unregistered). The result owns its data and is safe to hand to another
        // thread.
        static rttr::variant clone(rttr::type t, void* obj);

        static bool has(rttr::type t);
        static bool isRegistered(rttr::type t)
        {
            return has(t);
        }
        static std::vector<rttr::type> registeredTypes();
    };

} // namespace rpe

// Register a component type's bridge (place next to its RTTR registration).
#define RPE_REGISTER_COMPONENT(T) ::rpe::TypeBridge::registerType<T>()

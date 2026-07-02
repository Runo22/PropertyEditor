#pragma once

#include "rpe/core/rttr_prelude.h"

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
    //
    //  ── Plugin-unload safety ────────────────────────────────────────────────────
    //  The wrap/clone hooks are stored as PLAIN FUNCTION POINTERS, not std::function.
    //  A captureless lambda is just a function pointer, and a function pointer is a
    //  POD: destroying a registry entry (at process exit, or unregisterType) never
    //  calls back into the module that registered it. So even if a plugin DLL is
    //  unloaded while its entries are still in the registry, tearing the registry
    //  down is safe (no "exception on destruction"). The one rule that remains: do
    //  not CALL a hook (wrap/clone, i.e. inspect a component) after its plugin has
    //  unloaded — the code it points at is gone. Call unregisterType() in the
    //  plugin's unload path, or stop inspecting its components first.
    // ─────────────────────────────────────────────────────────────────────────────
    class TypeBridge
    {
    public:
        // Plain function pointers (a captureless lambda decays to one). NOT
        // std::function — see "Plugin-unload safety" above.
        using Wrapper = rttr::variant (*)(void*); // void* -> variant(T*)
        using Cloner = rttr::variant (*)(void*);  // void* -> variant(T by value)

        static void registerEntry(rttr::type t, Wrapper wrap, Cloner clone);

        template <class T>
        static void registerType()
        {
            registerEntry(
                rttr::type::get<T>(),
                +[](void* p) -> rttr::variant { return rttr::variant(static_cast<T*>(p)); },
                +[](void* p) -> rttr::variant { return rttr::variant(*static_cast<T*>(p)); });
        }

        // Same, but also register an explicit flecs component name to resolve to T.
        // Use when your RTTR type name and the flecs component name don't share a
        // short name (e.g. you registered RTTR as "game::Transform" but the flecs
        // component is "TfXform"): resolveByName("TfXform") then finds T. The exact
        // and short-name matching of the no-arg overload still applies on top.
        template <class T>
        static void registerType(const char* flecsName)
        {
            registerType<T>();
            registerAlias(rttr::type::get<T>(), flecsName);
        }

        template <class... Ts>
        static void registerTypes()
        {
            (registerType<Ts>(), ...);
        }

        // Map an explicit flecs component name onto an already-registered type.
        static void registerAlias(rttr::type t, const char* flecsName);

        // Remove only the bridge entry for `t` (RTTR registration is untouched).
        static void unregisterType(rttr::type t);
        template <class T>
        static void unregisterType()
        {
            unregisterType(rttr::type::get<T>());
        }

        // Resolve a flecs component name to a registered bridged type. Tries, in
        // order: an explicit alias (registerType<T>(name) / registerAlias), the
        // exact RTTR name, then the short name (segment after the last "::") — flecs
        // reports the unscoped component name while RTTR types may be registered
        // scoped. Returns an invalid type if nothing registered matches.
        static rttr::type resolveByName(const char* flecsName);

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

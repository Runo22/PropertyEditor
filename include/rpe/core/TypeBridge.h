#pragma once

#include "rpe/core/rttr_prelude.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace rpe
{

    namespace detail
    {
        template <class T>
        constexpr std::string_view prettyTypeSignature()
        {
#if defined(_MSC_VER)
            return __FUNCSIG__;
#else
            return __PRETTY_FUNCTION__;
#endif
        }

        // Pull the fully-qualified C++ name of T out of the compiler's function
        // signature ("game::Stats"). This is the type's REAL namespaced identity —
        // unlike rttr::type::get_name(), which is whatever string the RTTR
        // registration passed and may be namespace-stripped (or even identical for
        // two different types, which is precisely when name-based lookup breaks).
        inline std::string extractTypeName(std::string_view sig)
        {
            if (const auto eq = sig.find("T = "); eq != std::string_view::npos)
            {
                // GCC/Clang: "... [with T = game::Stats; ...]" / "... [T = game::Stats]"
                const auto start = eq + 4;
                auto end = sig.find_first_of(";]", start);
                if (end == std::string_view::npos)
                {
                    end = sig.size();
                }
                sig = sig.substr(start, end - start);
            }
            else
            {
                // MSVC: "...prettyTypeSignature<struct game::Stats>(void)"
                const auto lt = sig.find('<');
                const auto gt = sig.rfind('>');
                if (lt == std::string_view::npos || gt == std::string_view::npos || gt <= lt)
                {
                    return {};
                }
                sig = sig.substr(lt + 1, gt - lt - 1);
            }
            for (std::string_view kw : { std::string_view("struct "), std::string_view("class "),
                                         std::string_view("enum "), std::string_view("union ") })
            {
                if (sig.size() > kw.size() && sig.substr(0, kw.size()) == kw)
                {
                    sig.remove_prefix(kw.size());
                    break;
                }
            }
            while (!sig.empty() && sig.front() == ' ')
            {
                sig.remove_prefix(1);
            }
            while (!sig.empty() && sig.back() == ' ')
            {
                sig.remove_suffix(1);
            }
            return std::string(sig);
        }

        template <class T>
        inline std::string cppTypeName()
        {
            return extractTypeName(prettyTypeSignature<T>());
        }
    } // namespace detail

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
            // Alias the type by its REAL C++ name ("game::Stats"), so a flecs path
            // resolves to the right type even when the RTTR registration used a
            // different — or namespace-stripped, or duplicated — name. Without this,
            // two types registered in RTTR under the same name are indistinguishable
            // and the first one always wins.
            registerAlias(rttr::type::get<T>(), detail::cppTypeName<T>());
        }

        // Same, but also register an explicit flecs component name to resolve to T.
        // Use when your RTTR type name and the flecs component name don't share a
        // short name (e.g. you registered RTTR as "game::Transform" but the flecs
        // component is "TfXform"): resolveByName("TfXform") then finds T. The exact
        // and short-name matching of the no-arg overload still applies on top.
        template <class T>
        static void registerType(std::string_view flecsName)
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
        // Accepts any string source (const char* / std::string / view); the name is
        // copied into the registry, so no null-termination or lifetime is assumed.
        static void registerAlias(rttr::type t, std::string_view flecsName);

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
        // scoped. Returns an invalid type if nothing registered matches. Accepts any
        // string source (const char* / std::string / view).
        static rttr::type resolveByName(std::string_view flecsName);

        // Wrap `obj` as a variant holding a typed pointer (invalid if unregistered).
        static rttr::variant wrap(rttr::type t, void* obj);

        // Deep-copy the pointee into a self-contained value variant (invalid if
        // unregistered). The result owns its data and is safe to hand to another
        // thread.
        static rttr::variant clone(rttr::type t, void* obj);

        // Monotonic counter bumped by every registration change (registerEntry,
        // registerAlias, unregisterType). Lets a consumer cache derived data (e.g.
        // "which flecs components are bridged") and rebuild only when this moves —
        // late plugin registrations are then picked up without polling the registry.
        static uint64_t registryGeneration();

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

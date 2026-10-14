#pragma once

#include "rpe/core/rttr_prelude.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  Releasing a plugin's types before its module unloads.
    //
    //  RTTR frees a module's type data when the module (a plugin DLL) unloads, and
    //  every value read through that module's properties carries function pointers
    //  into its code. Anything rpe still holds — a property tree, a cached value, a
    //  queued mirror update, an open editor's starting value — then points at freed
    //  memory, and the next touch crashes (is_wrapper, get_metadata, a destructor…).
    //
    //  TypeBridge::unregisterType(t) is therefore the RELEASE POINT: before it
    //  returns, every rpe object drops what it holds of `t` — while the plugin's code
    //  is still loaded, on the calling thread, whatever that thread is. It never
    //  waits for another thread to run (only for one to leave a short read section),
    //  so it is safe to call while the GUI thread is blocked waiting for the caller.
    //  The views then reset and rebind by name on their own thread, so a component
    //  that comes back after the reload shows again by itself.
    //
    //  Rules for the host:
    //   • unregister every bridged type of a plugin BEFORE unloading it;
    //   • don't unload from inside an rpe signal handler or menu callback — queue it.
    // ─────────────────────────────────────────────────────────────────────────────

    // A read section over reflected data (a model's types, properties, values).
    // Reentrant per thread; cheap when uncontended. A release waits for the read
    // sections OTHER threads are in to end, and holds new ones off while it runs.
    // Never wait on another thread while holding one (see TypeUseGap).
    class TypeUseScope
    {
    public:
        TypeUseScope();
        ~TypeUseScope();
        TypeUseScope(const TypeUseScope&) = delete;
        TypeUseScope& operator=(const TypeUseScope&) = delete;
    };

    // Step OUT of this thread's read sections for a while — around anything that may
    // wait for another thread (a user lock such as the world's AccessGuard), which
    // might be releasing types and waiting for this thread to leave. On return the
    // read section is held again; data read before the gap may have been released
    // meanwhile, so check before using it.
    class TypeUseGap
    {
    public:
        TypeUseGap();
        ~TypeUseGap();
        TypeUseGap(const TypeUseGap&) = delete;
        TypeUseGap& operator=(const TypeUseGap&) = delete;

    private:
        int _depth = 0;
    };

    // Called by a release with the types being released, on the releasing thread,
    // while no other thread is inside a TypeUseScope. A handler must only drop data
    // it owns (no Qt signals, no widget calls — post a queued call for GUI work).
    using TypeReleaseHandler = std::function<void(const std::vector<rttr::type>& released)>;
    int addTypeReleaseHandler(TypeReleaseHandler handler);
    void removeTypeReleaseHandler(int handle);

    // Bumped by every release.
    uint64_t typeReleaseCount();

    // True if `t` — or the type it wraps / points to / holds as array elements — is
    // one of `released`. Compares handles only (never touches the type's data).
    bool isReleased(rttr::type t, const std::vector<rttr::type>& released);

    namespace detail
    {
        // Run every handler for `types`. Called by TypeBridge::unregisterType.
        void releaseTypes(const std::vector<rttr::type>& types);
    } // namespace detail

} // namespace rpe

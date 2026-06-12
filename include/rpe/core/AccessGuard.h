#pragma once

#include <functional>

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  AccessGuard — synchronously runs `work` with exclusive access to data owned
    //  by another thread (e.g. a flecs world advanced by a simulation thread).
    //
    //  The GUI widgets in this library never touch the world while painting — all
    //  values are cached in the model — but they DO touch it when refreshing,
    //  enumerating entities/components, and writing edits back. Install a guard to
    //  make those touch points safe against a concurrently running owner thread:
    //
    //      std::mutex worldMutex;                     // shared with the sim loop
    //      browser->setWorldAccess([&](const std::function<void()>& work) {
    //          std::lock_guard<std::mutex> lock(worldMutex);
    //          work();
    //      });
    //
    //      // simulation thread:
    //      while (running) {
    //          { std::lock_guard<std::mutex> lock(worldMutex); world.progress(dt); }
    //          ...
    //      }
    //
    //  Alternatively the guard can marshal `work` onto the owning thread and block
    //  until it has run (command-queue style), if pausing the sim via a mutex is
    //  not acceptable.
    //
    //  Contract: the guard must execute `work` exactly once, synchronously, before
    //  returning. Guards are never nested by the library, so a plain (non-recursive)
    //  mutex is sufficient. When no guard is installed, work runs directly
    //  (single-threaded case — previous behaviour).
    // ─────────────────────────────────────────────────────────────────────────────
    using AccessGuard = std::function<void(const std::function<void()>&)>;

    // Invoke `work` through `guard` when set, directly otherwise.
    inline void withGuard(const AccessGuard& guard, const std::function<void()>& work)
    {
        if (guard)
        {
            guard(work);
        }
        else
        {
            work();
        }
    }

} // namespace rpe

# EcsMirror — inspecting a multi-threaded simulation

`EcsMirror` lets the GUI inspect and edit a flecs world that lives on its own
simulation thread, **without the GUI ever touching the world**. The producer
snapshots watched values at frame end; edits queue back and are applied on the
simulation thread. No sync barrier is added to your pipeline (the registered
task declares no terms), and the snapshot runs after the frame merge — custom
phases and `depends_on` chains don't interact with it.

## Which thread does what

**This is the #1 source of crashes.** A flecs world allows only one thread at a
time; everything that touches it belongs to the thread that runs `progress()`.

| Call | Thread |
|---|---|
| `mirror.attach()` / `detach()` / destroying the `EcsMirror` | the flecs thread (the one running `world.progress()`) — `detach()` and the destructor are also safe elsewhere: they never touch the world |
| your plugin's flecs work (`world.component<T>()`, `entity.set<T>()`, creating entities) | the flecs thread |
| the world-taking [health checks](health-checks.md) | the flecs thread |
| `setInterest()`, `setRequiredComponent()`, `queueEdit()`, `setPins()`, `poll*()`, `setMaxPumpRateHz()` | any thread (the channel is thread-safe) |
| `new EntityComponentBrowser`, `browser->setMirror()`, every widget call | the Qt GUI thread |

> ⚠️ The classic mistake: calling `mirror.attach()` from the *main* thread while
> the *flecs* thread runs `progress()`. `attach()` only defers itself when it
> detects readonly mode, which is true only *inside* `progress()` — from another
> thread it installs at once and races, and the crash shows up later inside the
> pump. Run `attach()` on the flecs thread (e.g. as a task the flecs thread
> executes), exactly like your plugin's component registration.

## Attach (on the simulation thread)

```cpp
rpe::EcsMirror mirror;
mirror.attach(&world);                              // PumpMode::System (default)
// or drive it yourself:
mirror.attach(&world, rpe::EcsMirror::PumpMode::Manual);
while (running) { world.progress(dt); mirror.pump(); }
```

- `System` registers a frame-end pump automatically; `Manual` registers no
  system — you call `pump()` after `progress()`.
- The `flecs::world*` you pass may be a **temporary wrapper** (plugin pattern):
  only the underlying `ecs_world_t*` is stored.
- `attach()` may be called mid-`progress()` (e.g. from a system loading a
  plugin) — installation is deferred to frame end.
- `detach()` and the destructor are safe from any thread and never touch the
  world.

## The two performance knobs

```cpp
mirror.setMaxPumpRateHz(60);            // 0 = every frame (default). SET THIS on
                                        // uncapped/debug sims: the GUI only needs
                                        // 30–60 Hz, everything else is waste.
                                        // Keep it clearly ABOVE the sim's normal
                                        // frame rate (60 for a 30 Hz sim): the
                                        // gap is strict, and a cap EQUAL to the
                                        // frame rate skips every other frame.
mirror.setScanIntervalsMs(500, 2000);   // wall-clock throttle for the periodic
                                        // scans (entity list, spawnable prefabs).
                                        // Defaults shown. The add-component
                                        // catalog is rescanned only when the
                                        // component set changes — never on a timer.
mirror.setScanBudgetMsPerPump(1.0);     // the entity scan is INCREMENTAL: at most
                                        // this much labelling work per pump, the
                                        // list publishes when the cycle completes.
                                        // Default 1 ms; 0 = old single-shot scan.
```

What runs when:

| Work | Cadence | Cost drivers |
|---|---|---|
| Pump (watched-leaf reads + dedup) | every frame, capped by `setMaxPumpRateHz` | number of visible/pinned leaves |
| Entity-list scan | `setScanIntervalsMs` first arg (forced by filter/structural changes) | world size; the `requiredComponent` filter narrows the query. Work is sliced by `setScanBudgetMsPerPump` (default 1 ms), so a big world costs a flat ~1 ms/pump instead of one spike; `pumpStats().lastScanMs` reports the whole cycle's total. |
| Prefab list | second arg (forced by structural edits / group-tag changes) | number of prefabs |
| Add-component catalog | when a component type or bridge registration appears, when an Add menu is about to open (pointer on the button), and every 10 s as a backstop | number of component types; name resolution is memoised, so a rescan is a few ms even with thousands of components |

Everything else is cached and change-gated: the selected entity's component
list rebuilds only when its **archetype (table)** or the TypeBridge registry
generation changes; component types resolve once, not per pump.

## Diagnostics

```cpp
auto s = mirror.pumpStats();  // readable from any thread
// s.pumps, s.skipped (rate-cap hits), s.lastPumpMs, s.maxPumpMs, s.lastScanMs,
// s.lastCatalogMs, s.catalogScans (add-component catalog rescans)
```

Reading it: `skipped` growing + tiny `lastPumpMs` → the mirror is not your
bottleneck. Large `lastScanMs` → raise the scan interval or set a
`requiredComponent` filter. The definitive A/B is `mirror.detach()`.

## Edits

GUI edits (property editor, pinned widget) queue through the channel and are
applied on the simulation thread at the start of the next pump via
`setValueByPath`, then announced with **`ecs_modified_id`** — `OnSet`
observers and query change detection see inspector edits exactly like a
hand-written `set<T>()`. Structural add/remove-component requests are applied
the same way.

Every edit carries the **entity + component it was made on**, fixed when it is
queued — an inline editor commits on focus-out, i.e. inside the very click that
selects another entity, so "whatever is selected when the sim applies it" would
often be the wrong target. If you queue edits yourself, address them:

```cpp
mirror.channel()->queueEdit(entityId, "game.Transform", "position.x", rttr::variant(3.0));
mirror.queueEdit("position.x", rttr::variant(3.0));  // = the interest current NOW
```

An edit whose target is gone (entity destroyed, component removed) is dropped,
never redirected; an edit to a [read-only](editor-hints.md#read-only-values)
path is refused on the sim thread too.

## Rules & lifetimes

- `attach()`/`pump()` belong to the simulation thread; `setInterest`,
  `poll*()`, `queueEdit`, `setPins`, `setMaxPumpRateHz` are GUI-side and
  thread-safe.
- The GUI holds the mirror's **channel** via `shared_ptr` — destroying the
  `EcsMirror` on the sim thread first is safe; polls simply return nothing.
- Your own `set`/`get_mut` calls in systems cost the mirror **nothing**: it
  registers no observers/hooks. Only *watched* leaves are ever read.
- Plugin worlds: build `rpe_core` SHARED so host and plugins share one
  TypeBridge registry, and use one flecs library of one version. Late
  `registerType` calls are picked up automatically (registry generation).

## Guard mode — the alternative to the mirror

If you *can* serialise world access (a lock, or marshalling onto the sim thread),
use the browser in **direct mode** with a guard; every world touch runs through
it:

```cpp
std::mutex worldMutex;                  // shared with your sim loop
browser->setWorld(&world);
browser->setWorldAccess([&](const std::function<void()>& work) {
    std::lock_guard<std::mutex> lock(worldMutex);
    work();
});
// sim thread: { std::lock_guard lock(worldMutex); world.progress(dt); }
```

The guard runs `work` synchronously, exactly once; guards never nest, so a plain
mutex is enough. It may instead marshal `work` onto the sim thread and block until
it ran. For a standalone `PropertyEditor` in WriteBack mode on sim-owned data,
use `setWriteGuard` the same way. The mirror is usually the better choice: no
lock in your loop, and the GUI can never stall the simulation.

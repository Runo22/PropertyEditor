# EcsMirror — inspecting a multi-threaded simulation

`EcsMirror` lets the GUI inspect and edit a flecs world that lives on its own
simulation thread, **without the GUI ever touching the world**. The producer
snapshots watched values at frame end; edits queue back and are applied on the
simulation thread. No sync barrier is added to your pipeline (the registered
task declares no terms), and the snapshot runs after the frame merge — custom
phases and `depends_on` chains don't interact with it.

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
mirror.setScanIntervalsMs(500, 2000);   // wall-clock throttle for the two
                                        // FULL-WORLD scans (entity list, add-
                                        // component catalog). Defaults shown.
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
| Catalog scan | second arg | number of component types |

Everything else is cached and change-gated: the selected entity's component
list rebuilds only when its **archetype (table)** or the TypeBridge registry
generation changes; component types resolve once, not per pump.

## Diagnostics

```cpp
auto s = mirror.pumpStats();  // readable from any thread
// s.pumps, s.skipped (rate-cap hits), s.lastPumpMs, s.maxPumpMs, s.lastScanMs
```

Reading it: `skipped` growing + tiny `lastPumpMs` → the mirror is not your
bottleneck. Large `lastScanMs` → raise the scan interval or set a
`requiredComponent` filter. The definitive A/B is `mirror.detach()`.

## Edits

GUI edits (property editor, pinned widget) queue through the channel and are
applied on the simulation thread during the pump via `get_mut` +
`setValueByPath`, then announced with **`ecs_modified_id`** — `OnSet`
observers and query change detection see inspector edits exactly like a
hand-written `set<T>()`. Structural add/remove-component requests are applied
the same way.

## Rules & lifetimes

- `attach()`/`pump()` belong to the simulation thread; `setInterest`,
  `poll*()`, `queueEdit`, `setPins`, `setMaxPumpRateHz` are GUI-side and
  thread-safe.
- The GUI holds the mirror's **channel** via `shared_ptr` — destroying the
  `EcsMirror` on the sim thread first is safe; polls simply return nothing.
- Your own `set`/`get_mut` calls in systems cost the mirror **nothing**: it
  registers no observers/hooks. Only *watched* leaves are ever read.
- Plugin worlds: build `rpe_core` SHARED so host and plugins share one
  TypeBridge registry. Late `registerType` calls are picked up automatically
  (registry generation).

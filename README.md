# rpe — RTTR Property Editor

A reusable, performance-oriented Qt property editor for C++ simulations, built on
[RTTR](https://github.com/rttrorg/rttr) reflection, with an optional
[flecs](https://github.com/SanderMertens/flecs) ECS browser. Think of it as a
lightweight, embeddable "Details panel" in the spirit of Unreal Engine 5.

It provides **two independent features**:

1. **ECS Entity/Component/Property inspector** — a three-level browser:
   `Entities → Components → Properties`, with the entity list optionally
   filtered to those carrying a given component (e.g. a *Transform*). Designed to
   update live from the world at 50 Hz+.
2. **RTTR variant editor** — point it at any registered struct wrapped in an
   `rttr::variant` (owned copy *or* a live external object) and it builds an
   editor/inspector for it. No flecs dependency.

Both share the same generic property grid: numbers, booleans, strings, enums,
file paths, colors, **arrays/sequential containers**, and nested structs — all
discovered automatically from RTTR metadata.

## Layout

```
include/rpe/
  core/   engine-agnostic RTTR logic  → library rpe::core (shared)
          PropertyNode, TypeRenderer, RttrBridge (path get/set),
          RttrVariantWrapper, TypeBridge, EditorHints, AccessGuard, rttr_prelude
  gui/    reusable Qt property-grid widgets  → library rpe::gui
          PropertyModel, PropertyDelegate, EditorWidgets,
          PropertyEditor, VariantEditor
  ecs/    optional flecs integration (compiled with RPE_WITH_FLECS)  → rpe::gui
          EntityListWidget, ComponentListWidget, EntityComponentBrowser,
          EcsMirror (thread-safe sim/GUI bridge)
src/      mirrors include/ ; test/ holds the demo app + mirror_test
```

## Build

Requires CMake ≥ 3.21, a C++17 compiler, and Qt 5 (Widgets). RTTR and flecs are
fetched automatically.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/rpe_demo          # three tabs: ECS browser, property editor, variant editor
```

Options: `-DRPE_WITH_FLECS=OFF` (drop the ECS layer), `-DRPE_BUILD_DEMO=OFF`,
`-DRPE_USE_SYSTEM_DEPS=ON` (resolve rttr/flecs via `find_package` instead of
FetchContent).

Two library targets are produced:

| Target              | Kind   | Links               | For |
| ------------------- | ------ | ------------------- | --- |
| `rpe::core`         | SHARED | QtCore/Gui, RTTR    | type registration, reflection bridge — link from plugins |
| `rpe::gui` (`rpe::rpe`) | STATIC | rpe::core, QtWidgets, flecs | the widgets + ECS browser — link from the host |

`rpe_core` is **shared on purpose**: the `TypeBridge`/RTTR registries live in it
as a single process-wide instance, which a plugin architecture needs (see below).

## Integrating into an existing application

`add_subdirectory` (or FetchContent), then link the host against `rpe::gui`:

```cmake
add_subdirectory(external/PropertyEditor)
target_link_libraries(my_app PRIVATE rpe::gui)     # or rpe::rpe (alias)
```

Dependency resolution is integration-friendly: if your build already defines the
`RTTR::Core_Lib`/`RTTR::Core` or `flecs::flecs_static`/`flecs::flecs` targets,
rpe links those and skips its own FetchContent. Otherwise set
`RPE_USE_SYSTEM_DEPS=ON` to use installed packages, or leave it OFF to fetch
pinned versions.

The widgets are plain `QWidget`s — embed them in a `QDockWidget`, side panel,
tab, or window. `EntityComponentBrowser` emits `entitySelected` /
`componentSelected` (and id/name variants) so the host can mirror the
inspector's selection (e.g. highlight the entity in a viewport).

### Registering component types (plugins)

To inspect/edit a component referenced by a raw pointer, its type needs a
one-line bridge — RTTR cannot wrap a `void*` of a runtime type, so the
compile-time `T` is captured once:

```cpp
rpe::TypeBridge::registerType<Transform>();          // or:
rpe::TypeBridge::registerTypes<Transform, Physics>();
RPE_REGISTER_COMPONENT(Transform);                   // macro form
```

Put this **next to your RTTR registration** (same translation unit, where `T` is
complete) so the two share one lifetime. It's per-type-once, not per-use.

* The registry is process-global and independent of any widget — register
  before or after the browser exists; plugins loaded at runtime are picked up
  immediately. `registerType` is idempotent (add/remove/add is safe).
* `unregisterType` removes only the bridge entry; it never touches RTTR. You
  rarely need it — RTTR has no unregister and its accessors point into the
  defining module, so the safe pattern is host-owned, process-lifetime
  registration.
* **Single-registry requirement:** because `rpe_core` is shared, the host and
  every plugin that links it see one registry. (A statically-linked core copied
  into each module would split the registry and the browser wouldn't see a
  plugin's types — this is why core is shared.)

The widgets are plain `QWidget`s — embed them in a `QDockWidget`, a side panel,
a tab, or a standalone window. `EntityComponentBrowser` additionally emits
`entitySelected` / `componentSelected` so the host can mirror the inspector's
selection (e.g. highlight the entity in a viewport).

### Threading rules

* All widget/model APIs are GUI-thread only, **except**
  `PropertyEditor::setPropertyValue` / `PropertyModel::setPropertyValue`, which
  may be called from any thread (values are coalesced and applied on the GUI
  thread).
* Painting never touches your data: the model caches all values, so Qt repaints
  read only the cache. The world/object is touched only while refreshing,
  enumerating entities/components, and committing WriteBack edits.

**flecs world on a separate simulation thread.** A flecs world must not be
accessed concurrently, and the GUI thread must never touch it directly. Two
options:

#### Mirror mode — recommended, no lock in your loop

`EcsMirror` registers a once-per-frame system that runs *inside your existing
`world.progress()`* on the sim thread. Each frame it snapshots the watched leaf
values into self-contained copies and applies any edits the GUI queued. The GUI
only reads those copies. Neither thread blocks; **you don't change your loop.**

```cpp
rpe::TypeBridge::registerTypes<Transform, Physics>();   // once, by your core

rpe::EcsMirror mirror;
mirror.attach(&world);                 // call on the sim thread (or before progress)
mirror.setRequiredComponent("Transform");

browser->setMirror(&mirror);           // instead of setWorld(); GUI never touches world

// simulation thread — UNCHANGED, no mutex:
while (running)
    world.progress(dt);                // the mirror's system runs here
```

Costs: one value-copy per *watched* leaf per frame (only the fields currently
expanded in the tree, when `setSnapshotOpenFieldsOnly(true)`, the default) and
~1 frame of latency. The demo's ECS tab runs exactly this — a real `std::thread`
advancing the world with no lock.

**Requirements (important for plugin/DLL setups):**

* **Build flecs as one shared instance.** When the world is owned by the host
  and inspected from a plugin DLL, host + rpe + plugins must link the *same*
  flecs shared library (CMake default: `RPE_FLECS_SHARED=ON`). Two static flecs
  copies operating on one world fault inside flecs (e.g. `ecs_get_world`).
* **Call `attach()`/`detach()` on the simulation thread** (the one running
  `progress()`). They make structural changes to the world, which are not safe
  from another thread. `attach()` *may* be called from inside `progress()` — e.g.
  a system that loads the plugin at runtime — it detects readonly mode and defers
  the install to frame-end automatically.
* **Destruction order is safe in any order.** The GUI (`EntityComponentBrowser`)
  holds a `std::shared_ptr<MirrorChannel>`, not the `EcsMirror`. So you may
  destroy the `EcsMirror` on the sim thread *before* the GUI tears down (the
  usual shutdown / plugin-unload order): the browser keeps polling the channel,
  which just returns nothing once the producer is gone — no dangling pointer. The
  channel owns no flecs resources, so its final release on the GUI thread is
  safe.

#### Guard mode — simpler, if you can serialize world access

If you *can* take a lock (or marshal onto the sim thread) around world access,
install a guard and the browser routes every world touch through it:

```cpp
std::mutex worldMutex;                  // shared with your sim loop
browser->setWorldAccess([&](const std::function<void()>& work) {
    std::lock_guard<std::mutex> lock(worldMutex);
    work();
});
// sim thread: { std::lock_guard lock(worldMutex); world.progress(dt); }
```

The guard runs `work` synchronously, exactly once; guards never nest, so a plain
mutex suffices. It may instead marshal `work` onto the sim thread (command
queue) and block until it ran. For a standalone `PropertyEditor` in WriteBack
mode targeting sim-owned data, use `setWriteGuard` the same way.

## Using the property editor

```cpp
#include <rpe/rpe.h>

// 1) Reflect your type with RTTR (optionally add editor hints):
RTTR_REGISTRATION {
    rttr::registration::class_<Light>("Light")
        .property("intensity", &Light::intensity)(
            rttr::metadata(rpe::hint::Min, 0.0),
            rttr::metadata(rpe::hint::Max, 100.0),
            rttr::metadata(rpe::hint::Step, 0.5),
            rttr::metadata(rpe::hint::Decimals, 2))
        .property("iconPath", &Light::iconPath)(
            rttr::metadata(rpe::hint::Editor, rpe::editor::FilePath))
        .property("tint", &Light::tint)(
            rttr::metadata(rpe::hint::Editor, rpe::editor::Color));
}

// 2a) Live read-only / override display, fed from anywhere (thread-safe):
auto* editor = new rpe::PropertyEditor;
editor->bindType(rttr::type::get<Light>());
editor->refresh(rttr::instance(light));                 // GUI thread, 50 Hz
editor->setPropertyValue("intensity", 12.5);            // any thread

// 2b) As a data editor (edits write straight into the object):
editor->editObject(light);   // bind + WriteBack + instance provider in one call
```

### Edit policies

* **Override** (default) — an edited row is *pinned*: it freezes on top of the
  live stream until reset (right-click → *Reset to live*, or *Reset All*).
* **WriteBack** — edits are written straight into the bound object via
  `RttrBridge::setValueByPath`, so the editor becomes a data-authoring tool.

Switch with `setEditPolicy(...)`; the ECS browser exposes this as a
*"Write edits back to world"* toggle.

## ECS browser

```cpp
// Register a (void* -> typed instance) bridge once per component type:
rpe::TypeBridge::registerTypes<Transform, Physics, Material>();

auto* browser = new rpe::EntityComponentBrowser;
browser->setWorld(&world);
browser->setEntityComponentFilter("Transform");   // list only entities with it
browser->setLiveUpdateIntervalMs(20);             // 50 Hz
```

Components are auto-discovered: a flecs component shows up when its name resolves
to a registered `rttr::type` *and* a `TypeBridge` wrapper exists for it.

## Variant editor (independent feature)

```cpp
auto* ve = new rpe::VariantEditor;
ve->setVariant(rttr::variant(myStruct));   // edits an owned copy → ve->variant()
ve->edit(myLiveStruct);                    // or edit an external object in place
```

## Design notes

* **Hot path is cheap.** The tree schema is built once in `bindType`; `refresh`
  only re-reads values and emits tight `dataChanged` ranges. Sequential
  containers are the only thing rebuilt, and only when their size changes. This
  keeps many editors updating from the world inexpensive.
* **Thread-safe injection.** `setPropertyValue` coalesces updates under a mutex
  and flushes them on the GUI thread via a queued call.
* **Type-erased access via `TypeBridge`.** RTTR has no public "instance from
  `(type, void*)`" API, but a variant holding a `T*` acts as an instance of `T`.
  `TypeBridge` registers the one-line, compile-time wrapper that produces that
  typed pointer, which `RttrBridge` then drives for read/write by path.
* **Why not PmPropertyGrid?** It was evaluated (the brief suggested it "if
  convenient"). The in-repo `QAbstractItemModel` grid was kept instead: it gives
  full control over the high-frequency live-update path and per-type editors
  (the performance-critical requirement), needs no third-party GUI dependency,
  and still covers the full range of editors (number/bool/string/enum/path/
  color/array/nested). `RttrVariantWrapper` is the clean seam to adapt the data
  to a different grid later if desired.
```

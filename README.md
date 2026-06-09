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
  core/   engine-agnostic RTTR logic (no Qt widgets)
          PropertyNode, TypeRenderer, RttrBridge (path get/set),
          RttrVariantWrapper, TypeBridge, EditorHints, rttr_prelude
  gui/    reusable Qt property-grid widgets
          PropertyModel, PropertyDelegate, EditorWidgets,
          PropertyEditor, VariantEditor
  ecs/    optional flecs integration (compiled with RPE_WITH_FLECS)
          EntityListWidget, ComponentListWidget, EntityComponentBrowser
src/      mirrors include/ ; test/ holds the demo app
```

## Build

Requires CMake ≥ 3.21, a C++17 compiler, and Qt 5 (Widgets). RTTR and flecs are
fetched automatically.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/rpe_demo          # three tabs: ECS browser, property editor, variant editor
```

Options: `-DRPE_WITH_FLECS=OFF` (drop the ECS layer), `-DRPE_BUILD_DEMO=OFF`.

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

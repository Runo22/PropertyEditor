# rpe — RTTR Property Editor

A reusable, performance-oriented Qt property editor for C++ applications and
simulations, built on [RTTR](https://github.com/rttrorg/rttr) reflection, with an
optional [flecs](https://github.com/SanderMertens/flecs) ECS browser — a
lightweight, embeddable "Details panel" in the spirit of Unreal Engine.

**Full documentation: [`docs/`](docs/README.md).**

## What it does

- **Property grid** for any RTTR-registered type: numbers, booleans, strings,
  enums and bitmask flags, file paths, colours, dates, durations, nested
  structs, arrays, maps, `optional`, `shared_ptr` — discovered from reflection,
  tuned with [editor hints](docs/editor-hints.md) (ranges, pickers, labels,
  read-only).
- **Standalone editors** — edit an owned copy (with a change callback) or an
  object in place; no ECS needed. → [standalone-editors](docs/standalone-editors.md)
- **ECS browser** — `Entities → Components → Properties` for a flecs world:
  filtering, a required-component view, adding/removing components, spawning
  prefabs, deleting entities, custom context menus, full keyboard driving.
  → [ecs-browser](docs/ecs-browser.md)
- **Threading mirror** — inspect and edit a world that runs on its own
  simulation thread, with no lock in your loop. → [threading-mirror](docs/threading-mirror.md)
- **Watch list** — pin properties of many entities into one live, editable list.
  → [pinned-properties](docs/pinned-properties.md)
- **Plugin-friendly** — types registered from plugin DLLs, in any order, with
  hints and read-only locks that work across modules. → [registering-types](docs/registering-types.md)
- **Health checks** — on-demand diagnostics for "why doesn't my component show
  up / bind / edit as expected". → [health-checks](docs/health-checks.md)

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/rpe_demo
```

```cmake
add_subdirectory(external/PropertyEditor)
target_link_libraries(my_app PRIVATE rpe::gui)
```

```cpp
#include <rpe/rpe.h>
#include <rttr/registration.h>

struct Light { double intensity = 1.0; QColor tint = Qt::white; };

RTTR_REGISTRATION
{
    rttr::registration::class_<Light>("Light")
        .property("intensity", &Light::intensity)(
            rttr::metadata(rpe::hint::Min, 0.0), rttr::metadata(rpe::hint::Max, 100.0))
        .property("tint", &Light::tint);
}

Light light;
auto* editor = new rpe::PropertyEditor;
editor->editObject(light);     // edits write straight into `light`
editor->show();
```

Inspecting a flecs world that runs on a simulation thread:

```cpp
rpe::TypeBridge::registerTypes<Transform, Physics>();   // once per component type

rpe::EcsMirror mirror;
mirror.attach(&world);          // on the simulation thread
mirror.setMaxPumpRateHz(60);    // for uncapped sims

auto* browser = new rpe::EntityComponentBrowser;   // GUI thread
browser->setMirror(&mirror);

while (running) world.progress(dt);                 // your loop, unchanged
```

Before you ship it: read [getting-started → Use your flecs](docs/getting-started.md#use-your-flecs)
and [pitfalls](docs/pitfalls.md).

## Layout

```
include/rpe/
  core/  engine-agnostic reflection logic → rpe::core (SHARED: one registry per process)
         TypeBridge, RttrBridge (path get/set), TypeRenderer, PropertyNode,
         EditorHints, ReadOnly, OptionalSupport, FlagsSupport, PairSupport, AccessGuard
  gui/   Qt property-grid widgets → rpe::gui
         PropertyEditor, PropertyModel, PropertyDelegate, VariantEditor, DarkStyle
  ecs/   flecs integration (RPE_WITH_FLECS) → rpe::gui
         EntityComponentBrowser, EntityListWidget, ComponentListWidget,
         PinnedPropertiesWidget, EcsMirror / MirrorChannel,
         ComponentScan, ComponentRegistry (bindComponent), HealthCheck
src/     mirrors include/
test/    the demo app and the regression tests (one executable per test)
docs/    the documentation
```

## Design notes

- **The hot path is cheap.** The tree schema is built once per bound type;
  refreshing re-reads values and emits tight `dataChanged` ranges. Painting reads
  only cached values — it never touches your data.
- **The GUI never blocks the simulation.** In mirror mode the sim thread copies
  only the watched leaf values after each frame; edits queue back. Scans are
  wall-clock throttled and sliced so a big world costs a flat amount per frame.
- **Type-erased access via `TypeBridge`.** RTTR can't make an instance from
  `(type, void*)`, but a variant holding a `T*` acts as one; `TypeBridge`
  captures the compile-time wrapper once per type. Components are worked on in
  place — move-only types included.
- **Why an in-repo grid** (not PmPropertyGrid): full control over the
  high-frequency live-update path and per-type editors, no third-party GUI
  dependency.

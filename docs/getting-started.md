# Getting started

## Contents

- [Build](#build)
- [Integrate into your application](#integrate-into-your-application)
- [Use your flecs](#use-your-flecs)
- [Register a type](#register-a-type)
- [The property editor in 10 lines](#the-property-editor-in-10-lines)
- [Edit policies](#edit-policies)
- [Trimmings](#trimmings)
- [Where next](#where-next)

## Build

Requires CMake ≥ 3.21, a C++17 compiler and Qt 5 (≥ 5.12, Widgets). RTTR 0.9.6
and flecs 4.x are fetched automatically unless you provide them.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/rpe_demo          # tabs: ECS browser, property editor, variant editor
```

| Option | Default | |
|---|---|---|
| `RPE_WITH_FLECS` | `ON` | the flecs integration (ECS browser, mirror, health checks) |
| `RPE_BUILD_DEMO` | `ON` | the demo app and the tests |
| `RPE_USE_SYSTEM_DEPS` | `OFF` | resolve RTTR/flecs with `find_package` instead of fetching |
| `RPE_FLECS_SHARED` | `ON` | build/link flecs as a shared library — keep it ON when a world crosses a DLL boundary |

Two library targets:

| Target | Kind | For |
|---|---|---|
| `rpe::core` | **SHARED** | type registration and the reflection bridge — link it from plugins |
| `rpe::gui` (alias `rpe::rpe`) | STATIC | the widgets and the ECS browser — link it from the host |

`rpe_core` is shared **on purpose**: the TypeBridge registry lives there as one
process-wide instance, which a plugin architecture needs.

> **MSVC:** rpe's targets compile with `/utf-8`. If you build its sources some
> other way, keep that flag — the sources contain UTF-8 literals that otherwise
> turn into mojibake.

## Integrate into your application

```cmake
add_subdirectory(external/PropertyEditor)     # or FetchContent
target_link_libraries(my_app PRIVATE rpe::gui)
```

Dependencies resolve in this order: targets your build already defines
(`RTTR::Core_Lib` / `RTTR::Core`, `flecs::flecs` / `flecs::flecs_static`) →
`find_package` (with `RPE_USE_SYSTEM_DEPS=ON`) → FetchContent.

One include pulls in everything: `#include <rpe/rpe.h>`. The widgets are plain
`QWidget`s — put them in a dock, a side panel, a tab or a window.

## Use your flecs

**rpe and your application must use the same flecs — the same library and the
same version.** rpe's own build fetches a pinned flecs; if your application
uses another version, two flecs builds end up in one process and fail in ways
that make no sense.

Pick one:

```cmake
# 1) Best: add rpe AFTER your flecs target exists — rpe links it, fetches nothing.
add_subdirectory(external/flecs)              # defines flecs::flecs
add_subdirectory(external/PropertyEditor)

# 2) Or point rpe's fetch at your flecs sources:
#    cmake ... -DFETCHCONTENT_SOURCE_DIR_FLECS=/path/to/your/flecs

# 3) Or an installed flecs: -DRPE_USE_SYSTEM_DEPS=ON
```

Then make sure only **one** flecs shared library is loaded at runtime.
`rpe::checkFlecsBuild()` ([health checks](health-checks.md)) reports a version
mismatch between the headers rpe was compiled against and the flecs actually
running.

## Register a type

```cpp
#include <rpe/rpe.h>
#include <rttr/registration.h>

struct Transform
{
    Vec3 position;                    // nested structs expand into sub-rows
    double scale = 1.0;
    std::vector<int> lodBias;         // arrays expand into per-element rows
    std::filesystem::path meshPath;   // gets a file/folder picker
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Transform>("game::Transform")
        .property("position", &Transform::position)
        .property("scale", &Transform::scale)(
            rttr::metadata(rpe::hint::Min, 0.01),     // editor hints (optional)
            rttr::metadata(rpe::hint::Max, 100.0))
        .property("lodBias", &Transform::lodBias)
        .property("meshPath", &Transform::meshPath);
}

// For the ECS browser / mirror / in-place editing, also bridge it once:
rpe::TypeBridge::registerType<Transform>();
```

The details — names and namespaces, plugins, move-only types, special types —
are in [registering-types.md](registering-types.md); every hint is in
[editor-hints.md](editor-hints.md).

## The property editor in 10 lines

```cpp
Transform t;

auto* editor = new rpe::PropertyEditor;
editor->editObject(t);        // bind type + WriteBack + instance provider
editor->show();

// …or a read-only live display, fed from anywhere:
editor->bindType(rttr::type::get<Transform>());
editor->refresh(rttr::instance(t));               // GUI thread
editor->setPropertyValue("scale", 2.0);           // ANY thread (coalesced)
```

For an editor without the ECS browser (owned copy or in place), see
[standalone-editors.md](standalone-editors.md).

## Edit policies

| Policy | What a committed edit does |
|---|---|
| `EditPolicy::LocalEdit` (default) | kept as a **local draft**: the row shows your value (amber) and stops following live updates until *Reset to live* / *Reset All*. The object/world is never written |
| `EditPolicy::WriteBack` | written straight into the bound object through the instance provider (optionally under `setWriteGuard` for objects another thread owns) |

In **mirror mode** the browser sends edits to the simulation thread regardless
of the policy — see [threading-mirror.md](threading-mirror.md).

## Trimmings

```cpp
rpe::TypeRenderer::setFloatDecimals(3);        // float display precision (default 3)
editor->setToolbarVisible(false);              // hide the filter/reset row
editor->setReadOnly(true);                     // inspector-only
qApp->setStyleSheet(rpe::darkStyleSheet());    // built-in dark theme (optional)
```

**Right-click a row** for *Copy value*, *Copy name* and *Copy "name = value"*
(also in read-only mode), *Local edit* / *Reset*, and — when enabled — *Pin to
watch list*.

The toolbar **filter** matches a row's **value** as well as its name, so typing
`7.5`, `true`, or a struct's `[1, 2]` summary narrows the tree. It only re-runs
when the filter text changes, so live values cost nothing at steady state.

## Where next

| | |
|---|---|
| Inspect a flecs world | [ecs-browser.md](ecs-browser.md) |
| The world runs on its own thread | [threading-mirror.md](threading-mirror.md) |
| Something doesn't show / bind | [health-checks.md](health-checks.md), [pitfalls.md](pitfalls.md) |

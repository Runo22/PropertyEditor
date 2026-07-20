# Getting started

## Build & integrate

The project builds two targets (see the root `CMakeLists.txt` / `README.md`
for the full story):

- **`rpe::core`** (SHARED) — RTTR logic, no widgets/flecs. **Must be shared** in
  a plugin architecture: the host and every plugin have to link the *same*
  `TypeBridge` registry instance, or the browser won't see plugin-registered
  types.
- **`rpe::gui`** (STATIC, alias `rpe::rpe`) — the Qt widgets and, with
  `RPE_WITH_FLECS` (default ON), the flecs integration.

```cmake
add_subdirectory(PropertyEditor)      # or FetchContent
target_link_libraries(myapp PRIVATE rpe::gui)
```

Dependencies (Qt5 ≥ 5.12, RTTR 0.9.6, flecs 4.x) resolve via existing targets,
`find_package` (`RPE_USE_SYSTEM_DEPS=ON`), or FetchContent — in that order.

> **MSVC note:** the library's own targets compile with `/utf-8`. If you
> compile rpe sources through a different build system, keep that flag — the
> sources contain UTF-8 string literals ("…") that otherwise turn into
> mojibake ("â€¦") on Windows.

One include pulls in everything: `#include <rpe/rpe.h>`.

## Register your types

Properties become visible through RTTR registration:

```cpp
#include <rpe/rpe.h>
#include <rttr/registration.h>

struct Transform
{
    Vec3 position;                       // nested structs expand into sub-rows
    double scale = 1.0;
    std::vector<int> lodBias;            // arrays expand into per-element rows
    std::filesystem::path meshPath;      // gets a file/folder picker editor
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Transform>("Transform")
        .property("position", &Transform::position)
        .property("scale", &Transform::scale)(
            rttr::metadata(rpe::hint::Min, 0.01),   // editor hints (optional)
            rttr::metadata(rpe::hint::Max, 100.0),
            rttr::metadata(rpe::hint::Step, 0.1))
        .property("lodBias", &Transform::lodBias)
        .property("meshPath", &Transform::meshPath);
}
```

Available hints (`rpe/core/EditorHints.h`): `Min`, `Max`, `Step`, `Decimals`,
`Editor` (`rpe::editor::FilePath` / `SaveFile` / `Directory` / `Color` /
`Multiline`), `Label`, `Tooltip`, `ReadOnly`.

A few type-specific behaviours:

- **`std::string_view`** properties display their text but are deliberately
  read-only (the view points into memory the object owns — editing it through
  the grid would be unsafe).
- **`std::pair<A, B>`** needs one macro call next to your registrations:
  `RPE_REGISTER_PAIR(int, double);` (from `rpe/core/PairSupport.h`) — the pair
  then behaves like a two-field struct everywhere, including inside containers.
- **Small structs (≤ 4 fields)** show a compact `[x, y, z]` summary in the
  value column while their row is collapsed; expanding the row hides the
  summary and the fields edit individually. Wider structs show nothing when
  collapsed; arrays always show their element count `[N]`.

For anything that types a **raw pointer** (the ECS browser, mirror mode,
`VariantEditor::setLinked`), additionally register the bridge next to the RTTR
registration:

```cpp
rpe::TypeBridge::registerType<Transform>();   // or RPE_REGISTER_COMPONENT(Transform)
rpe::TypeBridge::registerTypes<A, B, C>();    // several at once
```

## The property editor in 10 lines

```cpp
Transform t;

auto* editor = new rpe::PropertyEditor;
editor->editObject(t);        // bind type + WriteBack + instance provider
editor->show();

// or read-only live display, fed from anywhere:
editor->bindType(rttr::type::get<Transform>());
editor->refresh(rttr::instance(t));               // GUI thread
editor->setPropertyValue("scale", 2.0);           // ANY thread (coalesced)
```

## Edit policies

| Policy | What a committed edit does |
|---|---|
| `EditPolicy::LocalEdit` (default) | Kept as a **local draft**: the row shows your value (amber) and stops following live updates until *Reset to live* / *Reset All*. **The object/world is never written.** |
| `EditPolicy::WriteBack` | Written straight into the bound object via the instance provider (optionally under a `setWriteGuard` for objects owned by another thread). |

In **mirror mode** the browser routes edits through the mirror's edit queue to
the simulation thread regardless of policy — see
[threading-mirror.md](threading-mirror.md).

## Trimmings

```cpp
rpe::TypeRenderer::setFloatDecimals(3);        // fixed-point float display (default 3)
editor->setToolbarVisible(false);              // hide the filter/reset row
editor->setReadOnly(true);
qApp->setStyleSheet(rpe::darkStyleSheet());    // built-in dark theme (optional to apply)
```

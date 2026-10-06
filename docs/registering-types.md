# Registering types

What makes a type show up — in the property grid, the ECS browser, the Add menu —
and how its flecs component name is matched to its C++ type.

## Contents

- [Two registrations: RTTR and TypeBridge](#two-registrations-rttr-and-typebridge)
- [How a flecs component finds its type](#how-a-flecs-component-finds-its-type)
- [Namespaces, aliases, same-named types](#namespaces-aliases-same-named-types)
- [Plugins](#plugins)
- [Move-only types](#move-only-types)
- [Value types inside components](#value-types-inside-components)
- [Special types](#special-types)
- [Checklist](#checklist)

## Two registrations: RTTR and TypeBridge

| Registration | What it gives you | Needed for |
|---|---|---|
| **RTTR** (`RTTR_REGISTRATION`) | the type's **properties** (and [editor hints](editor-hints.md)) | anything with fields to show |
| **TypeBridge** (`rpe::TypeBridge::registerType<T>()`) | a way to treat a raw `void*` as a `T` — RTTR can't do that without the compile-time `T` | the ECS browser, mirror mode, `VariantEditor::edit/setLinked` |

```cpp
#include <rpe/rpe.h>
#include <rttr/registration.h>

struct Transform { Vec3 position; double scale = 1.0; };

RTTR_REGISTRATION
{
    rttr::registration::class_<Transform>("game::Transform")
        .property("position", &Transform::position)
        .property("scale", &Transform::scale);
}

// Next to it, where T is complete — once per type, not per use:
rpe::TypeBridge::registerType<Transform>();          // or RPE_REGISTER_COMPONENT(Transform)
rpe::TypeBridge::registerTypes<A, B, C>();           // several at once
```

- A standalone `PropertyEditor` / `VariantEditor::setVariant` on a value you own
  needs only RTTR.
- A type **without** RTTR registration can still be bridged: it then lists as a
  component (and can be added/removed) but shows no fields. That's the right
  setup for marker components you use like tags.
- `registerType` is idempotent and order-independent — before or after the
  browser exists, before or after flecs registers the component.

## How a flecs component finds its type

The browser sees a flecs component by its **path** (`game.Transform`) and asks
TypeBridge which bridged type that is. The answer comes from the first rung that
matches:

1. **Alias** — a name given explicitly (`registerType<T>("flecs::path")`,
   `registerAlias`, `bindComponent`) or the **C++ type name**, which
   `registerType<T>()` records automatically (`game::Transform`, with its real
   namespace, whatever the RTTR name says).
2. **Exact RTTR name**, separator-insensitive (`game.Transform` ↔ `game::Transform`).
3. **Scope suffix** — the path is a scope-aligned tail of a longer RTTR name
   (`game.Transform` ↔ `app::game::Transform`).
4. **Short name** — the leaf alone. *Ambiguous* if two bridged types share it; the
   shortest, then alphabetically first, wins — deterministic, but possibly wrong.

`rpe::TypeBridge::explainResolve("game.Transform")` shows which rung decided and
which types competed; [`checkComponents`](health-checks.md) flags every binding
that only got there by the short name, and — as an **Error** — any component
bound to a type of a different size.

## Namespaces, aliases, same-named types

**Same name, different namespaces** (`game::Stats`, `ai::Stats`): register each
with RTTR under its **full** name and bridge both — rungs 1–2 keep them apart.

**The flecs name differs from the C++/RTTR name** (you rename or re-scope
components when registering them with flecs): tell TypeBridge the flecs name.

```cpp
// The full flecs name:
rpe::TypeBridge::registerType<game::Stats>("render::Stats");
// Or just the NAMESPACE, written with a trailing separator — completed with T's
// own leaf, so this is "render::Stats" too:
rpe::TypeBridge::registerType<game::Stats>("render::");
```

**Best of all, when the world is at hand**: ask flecs for the component's
*actual* path, so the two can never drift apart:

```cpp
#include <rpe/ecs/ComponentRegistry.h>

world.component<game::Stats>("render::Stats");      // however you name it…
rpe::bindComponent<game::Stats>(world);              // …this reads the real path
rpe::bindComponents<game::Stats, ai::Stats>(world);  // several; skips unknown types
```

`bindComponent` never registers or renames a flecs component: call it **after**
your own `world.component<T>(…)`. On a type flecs doesn't know yet it does
nothing (and returns an invalid entity).

## Plugins

- **Build `rpe_core` as a shared library** (the default) and link every plugin
  against it. The TypeBridge registry lives there; a plugin that links its own
  static copy registers into a registry the host never sees — its components
  simply don't appear.
- **One flecs, too.** Host, rpe and plugins must use the same flecs *library*
  (`RPE_FLECS_SHARED=ON`, the default) **and the same version** — see
  [getting-started](getting-started.md#use-your-flecs). `checkFlecsBuild()` tells
  you if they differ.
- **Any order works.** A plugin may register with flecs first or with
  TypeBridge first, before or after the browser starts. The add-component
  catalog follows registrations (and refreshes when an Add menu is about to
  open).
- **Unloading:** call `rpe::TypeBridge::unregisterType<T>()` in the plugin's
  unload path, *before* the DLL goes away. Afterwards rpe never calls into the
  unloaded code: rows and pins for that component disappear, lookups stop
  finding it. (RTTR has no unregister of its own — that side is RTTR's.)
- **Hints work from plugins** — `rttr::metadata(rpe::hint::Min, 0.0)` registered
  in a plugin DLL is found by rpe. Always use the `rpe::hint::*` constants (see
  [editor-hints](editor-hints.md#why-the-constants-and-never-strings)).
- **Lock a plugin's types from the host** without touching the plugin:
  `rpe::TypeBridge::setReadOnly("plugin::Type")` — by name, even before it loads.

## Move-only types

A component whose copy constructor is deleted (e.g. it owns a resource through a
move-only base) works: rpe operates on components **in place through a `T*`** and
copies only individual property values (an `int`, a `float`), never `T`.

```cpp
rpe::TypeBridge::registerType<audio::Speaker>();   // compiles for move-only T
```

- `TypeBridge::clone()` returns an invalid variant for such a type (nothing in
  rpe calls it).
- RTTR registration is optional. If you do register it:
  - don't give its constructor `policy::ctor::as_object` (that needs a copy — it
    won't compile, so you'll know); a plain `.constructor<>()` is fine;
  - don't register a **property whose own type is move-only** (the
    `unique_ptr`, the handle). Expose what's inside through a getter instead.
- Adding it from the Add menu default-constructs it — no copy.
- **Prefabs:** see [pitfalls](pitfalls.md#move-only-components-on-prefabs) —
  flecs' default prefab instantiation *copies* components and aborts on a
  move-only one.

## Value types inside components

A struct used as a **field** (`Vec3 position`) only needs RTTR registration — it
expands into sub-rows. It does not need a bridge, even if flecs also happens to
know it as a component. [`checkComponents`](health-checks.md) knows this and
never reports such value types (or `std::` types) as "unbridged".

## Special types

| Type | What you get | What you add |
|---|---|---|
| numbers, `bool`, enums, strings (`std::string`, `std::wstring`, `std::u16string`, `std::u32string`, `QString`) | inline editors (see [editor-hints](editor-hints.md#editors-by-type)) | — |
| nested structs | expandable rows; ≤ 4 fields show a `[x, y, z]` summary while collapsed | RTTR registration |
| `std::vector` & co. | one row per element; the row shows `[N]` | — |
| `std::map` / `std::unordered_map` | one row per key, sorted; values edit, keys don't; path `scores.[alice]` | — |
| `std::optional<T>` | `(none)` when empty; editing engages it | `RPE_REGISTER_OPTIONAL(T);` |
| `std::pair<A, B>` | a two-field struct | `rpe::registerPair<A, B>();` (or `RPE_REGISTER_PAIR`) |
| `std::shared_ptr<T>` | the pointee's fields, edited in place; null → blank, safely | — |
| bitmask enum | `Fire \| Poison` display, multi-check editor | `metadata(rpe::hint::Flags, true)`; **editing** also needs `RPE_REGISTER_FLAGS(E);` |
| `std::chrono` durations | count with a unit suffix (`250 ms`) | — |
| `QDateTime` | calendar-popup editor | — |
| `std::filesystem::path` | line edit + Browse… | — |
| `QColor` | swatch + picker | — |
| `std::string_view` / `std::wstring_view` | displayed, **always read-only** (the view points into memory the object owns) | — |
| flecs **tags** (zero-size) | badge rows; addable from the menu without any registration | — |
| flecs **pairs** carrying data | a `Damage → Fire` row editing the carried type | bridge the carried type |

## Checklist

When a type doesn't show the way you expect, run
[`rpe::checkHealth(world)`](health-checks.md) first. By hand:

1. Is it RTTR-registered (for fields) **and** bridged (for the ECS browser)?
2. Does its flecs name resolve to the right type? `explainResolve(path)`.
3. Does the plugin link the same `rpe_core` and the same flecs?
4. Is its top-level namespace hidden from the Add menu
   (`Settings::hiddenAddNamespaces`, default `settings`)?

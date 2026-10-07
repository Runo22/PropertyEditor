# reflectgen — generated RTTR / rpe / flecs registration

Annotate structs and enums in headers; `reflectgen` (a small exe on top of the
libclang C API) parses each header and writes a `.gen.cpp` that registers the
types with **RTTR** (properties + rpe editor hints), the **rpe** property editor
(`TypeBridge`, flags, optionals) and **flecs** (components, optional meta).
Each module (exe or plugin DLL) gets its own type list with explicit
register/unregister calls, so it works with DLLs loaded and unloaded at runtime.

## Annotating

```cpp
#include <reflect/Reflect.h>

namespace game
{
    inline constexpr double kMaxSpeed = 250.0;

    enum class REFLECT(flags) Damage : uint32_t { None = 0, Fire = 1, Poison = 2 };
    enum class REFLECT() Shape { Circle, Square, Count NOPROP };

    struct REFLECT(meta) Vec3 { PROP(step = 0.1) float x, y, z; };   // value type

    struct REFLECT(component, meta) Light                          // explicit mode
    {
        PROP(min = 0, max = 100, step = 0.5, decimals = 2, slider) float intensity = 1;
        PROP(file, label = "Icon", tooltip = "Shown in the outliner") std::string iconPath;
        PROP(max = kMaxSpeed) double speed = 1;   // values are C++ expressions
        PROP() Damage damage{};                   // flags hint added automatically
        PROP() Vec3 offset;
        float cache = 0;                          // no PROP → not registered
    };

    struct REFLECT(component, all, name = "ai::Stats") Stats       // all public fields
    {
        PROP(min = 0, max = 1000) int hp = 100;   // hints still per field
        std::optional<float> shield;              // OptionalBridge registered for you
        NOPROP int debugCounter = 0;              // opt out
    private:
        PROP(readonly) int secret = 7;
        REFLECT_FRIEND                            // lets generated code reach privates
    };
}
```

The macros expand to nothing for MSVC/GCC/Clang; they become
`[[clang::annotate]]` only while reflectgen parses (`REFLECT_PARSE`). The tool
keys on the annotation text, so the macro names can be changed freely.

| On | Keys |
| --- | --- |
| `REFLECT(...)` struct | `component` (flecs component + editor bridge), `meta` (describe members to flecs), `all` / `explicit` (field selection; default from `--default-fields`, else explicit), `name = "ns::Name"` (flecs path), `rttr_name = "..."` |
| `REFLECT(...)` enum | `flags` (bitmask: `FlagsBridge` + `hint::Flags` on fields of this type), `component`, `name = ...`, `rttr_name = ...`; `NOPROP` on an enumerator skips it |
| `PROP(...)` field | `min`, `max`, `step`, `decimals`, `label`, `tooltip`, `readonly`, `editor = default\|file\|savefile\|dir\|color\|text\|slider`, shorthands `slider` `color` `file` `savefile` `dir` `text`, `flags`, `name = "propName"` |

Mistakes are build errors pointing at the line: unknown keys, `slider` without
`min`+`max`, a private `PROP` without `REFLECT_FRIEND`, bit-fields, templates.

## CMake

```cmake
add_subdirectory(tools/reflectgen)        # builds reflectgen + reflect::runtime
target_link_libraries(game_plugin PRIVATE reflect::runtime rpe::core flecs::flecs)
reflect_generate(game_plugin)             # [DEFAULT_FIELDS all] [HEADERS ...]
```

Headers containing `REFLECT(` are picked at configure time (re-run CMake after
adding the first `REFLECT` to a new header). Each gets one Ninja step that parses
with the target's own include dirs/defines (clang-cl mode under MSVC), writes a
depfile, and only rewrites output that changed: a no-op build stays a no-op.

**Windows:** the official LLVM installer has everything (`include/clang-c`,
`lib/libclang.lib`, `bin/libclang.dll`); set `LLVM_ROOT` if it isn't in
`C:/Program Files/LLVM`. Use a recent LLVM — the MSVC STL refuses Clang versions
older than the ones it supports.

**Verified on MSVC** by `.github/workflows/reflectgen-windows.yml` (VS 18 /
MSVC 14.51, runner LLVM): C++17 with RTTR 0.9.6 and C++23 (`/std:c++latest`)
with RTTR master. RTTR 0.9.6 itself does not compile with MSVC in C++20+ mode
(`bind_impl.h`, `/permissive` does not help) — use RTTR master there.

## Module lifecycle (plugins)

Call from the module that owns the types — the plugin's own entry points:

```cpp
// after LoadLibrary
reflect::register_types();          // RTTR + rpe bridges
reflect::register_world(world);     // flecs components (sim thread, outside progress())

// before FreeLibrary
reflect::unregister_world(world);   // remove from all entities + delete components
reflect::unregister_types();        // the editor stops inspecting these types
FreeLibrary(...);                    // RTTR drops this module's registrations itself
```

Rules that keep unload safe:

* **Delete the plugin's own systems/observers too** before unloading — their
  callbacks are code in the DLL, same as component hooks.
* **Shared types live in a module that never unloads.** RTTR records a type in
  the module that touches it first and forgets it when that module unloads.
  Value types used by several plugins (math types, `std::string`, containers)
  belong in the host or a core DLL that registers them first.
* **Let the editor drop cached values first** (`EcsMirror` clones values; their
  destructors are plugin code): unregister, pump once, then unload.
* **One compiler for RTTR and every module.** RTTR identifies types by
  compiler-generated names; mixing compilers breaks conversions.
* Run `reflect_generate` on the DLL/exe target itself, not a static library
  linked into several modules.
* Consider `FLECS_CPP_NO_AUTO_REGISTRATION` so a component used before
  `register_world()` asserts instead of being silently auto-registered.

## Limits

Class templates (reflect a concrete type), base-class fields (reflect the base
separately), bit-fields. flecs meta covers arithmetic types, enums, other `meta`
structs and fixed arrays of those; other members (e.g. `std::string`) are left
out of flecs meta with a comment, but RTTR/the editor still see them.

## Test

`test/` builds a plugin with `reflect_generate`, then loads → checks RTTR
metadata, editor bridge, flecs components/meta → unloads → reloads:

```sh
cmake -S tools/reflectgen/test -B build-rg -G Ninja -Drttr_DIR=<rttr>/share/rttr/cmake \
      -DFLECS_DISTR=<flecs>/distr -DRPE_ROOT=$PWD && ninja -C build-rg && ctest --test-dir build-rg
```

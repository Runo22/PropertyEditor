# reflectgen — generated RTTR / rpe / flecs registration

Annotate structs and enums in headers; `reflectgen` (a small exe on top of the
libclang C API) parses each header and writes a `.gen.cpp` that registers the
types with **RTTR** (properties + rpe editor hints), the **rpe** property editor
(`TypeBridge`, flags, optionals, read-only) and **flecs** (components and their
member descriptions). Each module (exe or plugin DLL) keeps its own type list
with explicit register / unregister calls, so DLLs can be loaded, unloaded and
hot-reloaded at runtime — with component data kept across the reload.

## Annotating

```cpp
#include <reflect/Reflect.h>

namespace game
{
    inline constexpr double kMaxSpeed = 250.0;

    enum class REFLECT(flags) Damage : uint32_t { None = 0, Fire = 1, Poison = 2 };
    enum class REFLECT() Shape { Circle, Square, Count NOPROP };

    struct REFLECT(meta) Vec3 { PROP(step = 0.1) float x, y, z; };   // value type

    template <class T> struct Vec2T { PROP() T x{}; PROP() T y{}; };
    using Vec2f REFLECT(meta) = Vec2T<float>;                      // a template instantiation

    struct REFLECT(component) Light                                // explicit mode
    {
        PROP(min = 0, max = 100, step = 0.5, decimals = 2, slider) float intensity = 1;
        PROP(file, label = "Icon", tooltip = "Shown in the outliner") std::string iconPath;
        PROP(max = kMaxSpeed) double speed = 1;   // values are C++ expressions
        PROP() Damage damage{};                   // flags hint added automatically
        PROP() Vec3 offset;
        PROP() std::vector<Vec2f> path;
        float cache = 0;                          // no PROP → not registered
    };

    struct REFLECT() Base { PROP(min = 0) int hp = 10; };

    struct REFLECT(component, all, name = "ai::Stats") Stats : Base  // all public fields + Base's
    {
        PROP(max = 1000) int mana = 100;          // hints still per field
        std::optional<float> shield;              // OptionalBridge registered for you
        NOPROP int debugCounter = 0;              // opt out
        // accessor property: read/write through getter + setter ("power")
        PROP(setter = setPower, max = 100) float getPower() const { return power_; }
        void setPower(float v) { power_ = v; }
    private:
        PROP(readonly) int secret = 7;
        float power_ = 5;
        REFLECT_FRIEND                            // lets generated code reach non-public members
    };

    struct REFLECT(component, readonly) Config { PROP() int level = 1; }; // shown, never edited
}
```

The macros expand to nothing for MSVC/GCC/Clang; they become
`[[clang::annotate]]` only while reflectgen parses (`REFLECT_PARSE`). The tool
keys on the annotation text, so the macro names can be changed freely (tell
`reflect_generate` the new name with `MARKER`).

| On | Keys |
| --- | --- |
| `REFLECT(...)` struct / alias | `component` (flecs component + editor bridge; described to flecs unless `nometa`), `meta` (describe a value type to flecs), `nometa`, `all` / `explicit` (field selection; default `DEFAULT_FIELDS`, else explicit), `readonly` (editor shows, never edits), `name = "ns::Name"` (flecs path), `rttr_name = "..."` |
| `REFLECT(...)` enum | `flags` (bitmask: `FlagsBridge` + `hint::Flags` on fields of this type), `component`, `name = ...`, `rttr_name = ...`; `NOPROP` on an enumerator skips it |
| `PROP(...)` field | `min`, `max`, `step`, `decimals`, `label`, `tooltip`, `readonly`, `editor = default\|file\|savefile\|dir\|color\|text\|slider`, shorthands `slider` `color` `file` `savefile` `dir` `text`, `flags`, `name = "propName"` |
| `PROP(...)` const getter | the field keys, plus `setter = setX` (without it the property is read-only); name from `name =` or the getter (`getSpeed` → `speed`) |

* **Base classes:** fields of `REFLECT` bases are registered on the derived
  struct (RTTR and flecs), with the base's own field selection and hints.
  Protected members need `REFLECT_FRIEND` on the derived struct; a base's
  private members cannot be reached and are an error when asked for. A derived
  member hides a base one of the same name.
* **Template aliases:** `using X REFLECT(...) = Tpl<Args>;` reflects that
  instantiation (annotations come from the template, types from the
  instantiation). `REFLECT` on the template itself is an error.

Mistakes are build errors at the header line, saying what to fix: unknown keys,
`slider` without `min`+`max`, a non-public `PROP` without `REFLECT_FRIEND`,
private base members, bit-fields, overloaded setters, non-const getters,
`setter` on a field, `REFLECT` on a class template (`test/diag/Bad.h` has one of
each; the `generator_diagnostics` test requires every message).

## CMake

```cmake
add_subdirectory(tools/reflectgen)        # builds reflectgen + reflect::runtime
target_link_libraries(game_plugin PRIVATE reflect::runtime rpe::core flecs::flecs)
reflect_generate(game_plugin
    SCAN BUILD          # pick up REFLECT in any header without re-running CMake
    BATCH_SIZE 16       # compile 16 generated files per translation unit
    PREINCLUDE <QtWidgets/QtWidgets>)   # optional: heavy headers to precompile
```

Options (all optional):

| Option | |
| --- | --- |
| `DEFAULT_FIELDS explicit\|all` | field selection for `REFLECT` types that don't say |
| `HEADERS h…` | headers to look at (default: the target's `.h/.hh/.hpp/.hxx` sources) |
| `SCAN CONFIGURE\|BUILD` | `CONFIGURE` (default): only headers containing `REFLECT(` when CMake runs get a step — re-run CMake after adding the first `REFLECT` to a header. `BUILD`: every header gets a step; one without the marker is answered with an empty file without parsing, so a new `REFLECT` anywhere is picked up by the next build. |
| `BATCH_SIZE n` | compile the generated files `n` per translation unit (default 16 with `SCAN BUILD`, else 1) — fewer compiles of RTTR's registration headers |
| `PREINCLUDE h…` | headers the target's sources see first; also precompiled for parsing |
| `PARSE_PCH ON\|OFF` | precompile the forced includes for parsing (default `ON`) |
| `MARKER text` | the `REFLECT` macro's name if you renamed it |

**How headers are parsed:** with the target's own include dirs, defines,
compile options, C++ standard, `CMAKE_CXX_FLAGS` and `PRECOMPILE_HEADERS` (as
forced includes), so a header that is only complete inside the target's build
still parses. They reach reflectgen through a response file written with
`file(GENERATE … TARGET)` — which evaluates `$<COMPILE_LANGUAGE:CXX>`
conditions (a custom command silently drops them) and avoids command-line
length limits. reflectgen keeps only options that affect parsing (`-O3`,
`-fno-gnu-unique`, `/W4`, `/MP` … are dropped; `--verbose` lists them). Under
MSVC it parses in clang-cl mode.

**Speed:** parse time is dominated by heavy includes (measured: a header
including QtWidgets 1.8 s; 0.16 s on top of a PCH of it, identical output). The
forced includes are precompiled once per target into a libclang PCH that every
header step loads; a PCH that does not load is never fatal (that header is
parsed without it). Every step writes a Ninja depfile and rewrites its output
only when it changed: a no-op build stays a no-op, and generated symbols are
unique per header, so `UNITY_BUILD` and batching just work.

**Windows:** the official LLVM installer has everything (`include/clang-c`,
`lib/libclang.lib`, `bin/libclang.dll`); set `LLVM_ROOT` if it isn't in
`C:/Program Files/LLVM`. reflectgen tolerates an LLVM older or newer than the
MSVC STL expects (it only parses).

**Verified on MSVC** by `.github/workflows/reflectgen-windows.yml` (VS 18 /
MSVC 14.51, runner LLVM, Qt 5.15.2): C++17 with RTTR 0.9.6, and C++23
(`/std:c++latest`) with RTTR master — RTTR 0.9.6 itself does not compile with
MSVC in C++20+ mode (`bind_impl.h`; `/permissive` does not help).

## Module lifecycle (plugins)

The host, once, before any plugin:

```cpp
reflect::register_std_types(world);   // std::string, std::vector<number|string> for flecs
```

The plugin, from its own entry points:

```cpp
// after LoadLibrary
reflect::register_types();            // RTTR + rpe bridges
reflect::register_world(world);       // flecs components (sim thread, outside progress())
reflect::load_world(world, snapshot); // hot reload: values back (if you kept a snapshot)

// before FreeLibrary
std::string snapshot = reflect::save_world(world);  // hot reload: keep the values
reflect::unregister_world(world);     // remove from all entities + delete the components
reflect::unregister_types();          // rpe drops every value of these types
FreeLibrary(...);                      // RTTR drops this module's registrations itself
```

**Hot reload.** `save_world` writes every value of the module's components (and
which entities carry its tags) keyed by component, entity and member name; the
string holds no pointer into the module. After the new build registered,
`load_world` puts the values back: members that still exist get their value
(converted when compatible, e.g. double → float), new members keep their
default, removed ones are ignored, a member whose type changed incompatibly
keeps its default; missing components or entities are reported, not fatal.
Only what flecs can describe is saved — components are described by default,
including `std::string` / `std::vector` members once the host called
`register_std_types`.

Rules that keep unload safe:

* **Shared types live in a module that never unloads.** flecs' hooks and RTTR's
  type data belong to the module that registered a type first. That is why the
  host registers the std types: plugins only use them (found by stable names —
  compilers spell the same instantiation differently per module), never register
  them. A `std::vector` of a plugin's own type is registered and removed by the
  plugin. Your value types shared by several plugins (math types…) belong in the
  host or a core DLL that registers them first; rpe_core already claims
  `std::string`, `QColor`, … for RTTR.
* **Delete the plugin's own systems/observers too** before unloading — their
  callbacks are code in the DLL, same as component hooks.
* `reflect::unregister_types()` calls `TypeBridge::unregisterType`, rpe's
  release point: the editor, mirror and watch list drop every value of the type
  there, while the plugin is still loaded.
* **One compiler for RTTR and every module.** RTTR identifies types by
  compiler-generated names; mixing compilers breaks conversions.
* Run `reflect_generate` on the DLL/exe target itself, not a static library
  linked into several modules.
* Consider `FLECS_CPP_NO_AUTO_REGISTRATION` so a component used before
  `register_world()` asserts instead of being silently auto-registered.

## Limits

Bit-fields; accessor properties of base classes (register them on the base's
own type); virtual bases; class-template bases of a template alias. flecs meta
covers arithmetic types, enums, other described structs, fixed arrays of those,
`std::string` and `std::vector` of numbers, strings, described structs and
enums; other members stay out of flecs meta (with a comment in the generated
code) but RTTR / the editor still see them.

## Test

`test/` builds a plugin twice (v2 changes a struct) with `reflect_generate`, and:
`plugin_roundtrip` loads v1 → edits values → hot-reloads v2 → v1, checking RTTR
metadata, the editor path, flecs meta/JSON, base fields, accessors, aliases,
read-only, std members and that nothing leaks across unloads;
`scan_build_picks_up_new_header` adds `REFLECT` after configure and builds;
`parse_pch_built` and `generator_diagnostics` as named.

```sh
cmake -S tools/reflectgen/test -B build-rg -G Ninja -DCMAKE_PREFIX_PATH=<rttr install>[;<Qt5>] \
      -DFLECS_ROOT=<flecs checkout> -DRPE_ROOT=$PWD
ninja -C build-rg && ctest --test-dir build-rg
```

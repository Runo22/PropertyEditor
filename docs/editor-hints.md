# Editor hints

Hints are RTTR metadata on a property (or, for `ReadOnly`, on a class) that tune
how the property grid shows and edits it. They live in `rpe/core/EditorHints.h`.

```cpp
RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<Light>("Light")
        .property("intensity", &Light::intensity)(
            metadata(rpe::hint::Min, 0.0),
            metadata(rpe::hint::Max, 100.0),
            metadata(rpe::hint::Step, 0.5),
            metadata(rpe::hint::Decimals, 2),
            metadata(rpe::hint::Label, "Intensity (lm)"),
            metadata(rpe::hint::Tooltip, "Luminous output"))
        .property("iesProfile", &Light::iesProfile)(
            metadata(rpe::hint::Editor, rpe::editor::FilePath))
        .property("tint", &Light::tint)(
            metadata(rpe::hint::Editor, rpe::editor::Color))
        .property("serial", &Light::serial)(
            metadata(rpe::hint::ReadOnly, true));
}
```

## Contents

- [Hint reference](#hint-reference)
- [`rpe::editor::*` values](#rpeeditor-values)
- [Editors by type](#editors-by-type)
- [Read-only values](#read-only-values)
- [Why the constants, and never strings](#why-the-constants-and-never-strings)
- [Edge cases](#edge-cases)

## Hint reference

| Hint | Value | Applies to | Effect |
|---|---|---|---|
| `rpe::hint::Min` | number | floats, `int`/`short`/`char`-sized integers | lower bound of the **editor** |
| `rpe::hint::Max` | number | same | upper bound of the **editor** |
| `rpe::hint::Step` | number | floats, integers, `std::chrono` durations | spin-box step (defaults: 0.1 float, 1 integer) |
| `rpe::hint::Decimals` | integer | floats | decimals in the **editor** (default 4) |
| `rpe::hint::Editor` | an `rpe::editor::*` value | see [below](#rpeeditor-values) | picks a specific editor |
| `rpe::hint::Label` | string | any property | display name instead of the property name |
| `rpe::hint::Tooltip` | string | any property | tooltip on the row |
| `rpe::hint::ReadOnly` | `bool` | a **property**, or a **class** | never opens an editor — see [Read-only values](#read-only-values) |
| `rpe::hint::Flags` | `bool` | an enum property | treat it as a bitmask: `Fire \| Poison` display, multi-check editor. **Editing** also needs `RPE_REGISTER_FLAGS(E);` |

Enums are never auto-detected as flags — a plain enum whose values happen to be
powers of two would be misread — so `Flags` is the opt-in. Display works with the
hint alone; combined values also show leftover unnamed bits as `0xNN`.

## `rpe::editor::*` values

| Value | Property types | Editor |
|---|---|---|
| `Default` | any | chosen by type (same as no hint) |
| `FilePath` | strings, `std::filesystem::path` | line edit + Browse… (open a file) |
| `SaveFile` | strings, `std::filesystem::path` | line edit + Browse… (save-file dialog) |
| `Directory` | strings, `std::filesystem::path` | line edit + Browse… (pick a folder) |
| `Color` | `QColor`, strings | swatch + picker; on a string, stored as `#AARRGGBB` (named colours parse too) |
| `Multiline` | strings | multi-line plain-text editor |
| `Slider` | floats and `int`-sized integers with **both** `Min` and `Max` | a slider next to the number's spin box; `Step` sets the slider's resolution. Without a range it falls back to the plain spin box |

`std::filesystem::path` with **no** hint gets a Browse… that accepts a file or a
folder; a hint pins the dialog kind.

## Editors by type

| Type | Editor |
|---|---|
| `bool` | check box |
| floats (`float`, `double`) | spin box — `Min`/`Max`/`Step`/`Decimals` |
| integers that fit an `int` | spin box — `Min`/`Max`/`Step` |
| `unsigned int`, `long`, `unsigned long`, 64-bit integers | **validated line edit** (so large values are never clamped) — `Min`/`Max`/`Step` are ignored |
| enums | combo box (multi-check with `Flags`) |
| strings | line edit (or per `Editor` hint) |
| `std::filesystem::path` | line edit + Browse… |
| `QColor` | swatch + picker |
| `QDateTime` | calendar-popup date/time edit |
| `std::chrono` durations | spin box with a unit suffix (`ms`) — `Step` |
| structs, containers, `shared_ptr`, `optional` | expand into rows; leaves edit as above |
| `std::string_view`, `std::wstring_view` | read-only |

Float **display** precision is global: `rpe::TypeRenderer::setFloatDecimals(3)`
(the `Decimals` hint is for the editor only).

## Read-only values

A value is shown but **never opens an editor** when any of these hold:

1. the property has **no setter** — `property_readonly(...)`, or a `property(...)`
   registered with only a getter. Everything *below* it is read-only too: editing
   a field of a getter-only struct would write into a temporary copy and vanish;
2. the property carries `metadata(rpe::hint::ReadOnly, true)`;
3. its **type is locked**, wherever that type appears (the whole component, or a
   struct inside one) — by either route:

```cpp
// RTTR side, for a type you register yourself:
rttr::registration::class_<Tuning>("Tuning")(rttr::metadata(rpe::hint::ReadOnly, true))
    .property("gain", &Tuning::gain);

// At runtime, from anywhere — e.g. the host locking a plugin's type, before it loads:
rpe::TypeBridge::setReadOnly<Tuning>();          // by type
rpe::TypeBridge::setReadOnly("audio::Tuning");   // by name (RTTR name, C++ name or alias)
rpe::TypeBridge::setReadOnly<Tuning>(false);     // unlock
```

**How it looks:** the value keeps its normal colour (it's information, not a
disabled control), a small faint lock sits at the right edge of the cell, and the
tooltip gives the reason. The row is still selectable and copyable.

**Where it applies:** the property grid, the [watch list](pinned-properties.md),
and the [mirror](threading-mirror.md) on the simulation thread all ask the same
rule — `rpe::readOnlyReason(rootType, path)` — so none of them is a way around
it. A lock applied while a tree is open takes effect immediately.

The whole editor can also be made read-only: `editor->setReadOnly(true)`.

## Why the constants, and never strings

Use `rpe::hint::Min`, never `"rpe.min"`. The keys are 64-bit values derived from
the key's name at compile time. RTTR compares a `const char*` metadata key **by
address**, and every module (your executable, each plugin DLL, rpe itself) has its
own copy of a string literal — so a string key registered in one module is never
found from another. Value keys compare equal everywhere. The registration syntax
is the same either way; just use the constants.

(The `rpe::editor::*` *values* are compared by text, so a literal like
`"color"` would work too — but use the constants.)

## Edge cases

- **`Min`/`Max` are an editor range, not validation.** The simulation (or any
  other code) can still store a value outside it, and the grid shows it as-is.
  Opening the editor on such a value shows it clamped to the range; if you then
  commit without changing anything, **nothing is written** — only a value you
  actually changed is. (An unchanged commit never writes, for any type.)
- **`Min`/`Max`/`Step` don't apply to `unsigned int`, `long` or 64-bit
  integers** — those use a line edit so large values are never clamped.
- **`Decimals` doesn't change the displayed value** — only the editor. Use
  `TypeRenderer::setFloatDecimals` for display.
- **`Flags` without `RPE_REGISTER_FLAGS(E)`** displays fine, but a combined value
  can't be written back: RTTR 0.9.6 can't build an enum from an integer without
  the compile-time type.
- **Map keys** are never editable; string keys containing `.` or `]` can't be
  addressed by path (so can't be pinned or edited through the mirror).
- **A getter returning a struct by value** makes every field below it read-only
  — by design. Register a setter (or the field itself) if it should be editable.

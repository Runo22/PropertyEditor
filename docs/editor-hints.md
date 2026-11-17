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
- [Quick-edit helpers](#quick-edit-helpers) — inline vector row, drag to scrub, colour swatch
- [Named elements](#named-elements) — an array of name/value structs shown by name
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
| `rpe::hint::ElementLabel` / `ElementValue` | a field's name | a **class** used as an array element | show the array by name — see [Named elements](#named-elements) |

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

## Quick-edit helpers

These need no hint; they kick in by shape.

### Inline vector row

A struct with **2–4 numeric fields** (`Vec2`, `Vec3`, `Color4`, `Size2`, …) can be
edited **on its own row**: double-click the `[1, 2, 3]` summary and every field
gets a compact box side by side. When the fields ARE axes — all named from
`x/y/z/w`, or all from `r/g/b/a` — each carries a coloured axis chip
(red/green/blue/grey); otherwise (a `Rect{x, y, w, h}`, a `Size{width, height}`)
each shows its name. The row still expands as usual to edit one field.

- Each field keeps its own `Min`/`Max`/`Step`/`Decimals`.
- Only the fields you **changed** are written — the others keep their live value
  (in the mirror, each changed field is a separate edit).
- Clicking anywhere else in the window commits and closes it, like any other
  editor (the slider editor too). A picker it opens (a menu, a file or colour
  dialog) doesn't.
- Not offered when the struct is read-only, any field is read-only, a field is a
  `bool`, a struct, or an integer type that gets a line edit (`unsigned int`,
  `long`, 64-bit).

### Drag to scrub

Press on a numeric property's **name** and drag horizontally to change its
value; the cursor turns into a ↔ over names that scrub.

| | |
|---|---|
| step | the `Step` hint, else 0.1 (floats) / 1 (integers) per 4 px — whole steps only: the value (and a write) changes when the drag crosses into the next step, not on every pixel |
| precision | rounded to the `Decimals` hint (else to the step's precision) |
| Shift | fine — step × 0.1 |
| Ctrl | coarse — step × 10 |
| Esc (while dragging) | restores the starting value |
| range | clamped to `Min`/`Max`, and to the type's own range (an unsigned value stops at 0) |

A plain click still just selects the row — nothing changes until the mouse
moves past the platform drag distance, and the value starts from where you
crossed it, so it never jumps. Read-only values never scrub. Turn it off with
`editor->setDragToScrubEnabled(false)` (in the browser: `Settings::dragToScrub`).
It's a host setting only — there is no toggle in the UI.

Each step is a normal edit: under `LocalEdit` the row becomes a draft, under
`WriteBack`/mirror the value is written as you drag.

### Colour swatch

A `QColor` value, and a **string** with `editor::Color`, show a swatch of the
colour in the value cell (a string that isn't a valid colour shows none).

## Named elements

An array whose elements are a **name and a value** — a template struct you use
with several types — can show one row per element, **labelled by the name**,
with the value edited on that row:

```cpp
template <class N, class V> struct Stat { N name; V value; };
struct Character { std::vector<Stat<std::string, float>> stats; }; // speed, armor, …
```

| without | with |
|---|---|
| `stats ▸ [1] ▸ name = armor` / `stats ▸ [1] ▸ value = 7` | `stats ▸ armor = 7` |

Mark the element type any of these ways — the runtime ones win over the metadata:

```cpp
// 1. Class metadata, where you register it
registration::class_<Stat<std::string, float>>("Stat<std::string,float>")(
        metadata(rpe::hint::ElementLabel, "name"),
        metadata(rpe::hint::ElementValue, "value"))
    .property("name", &Stat<std::string, float>::name)
    .property("value", &Stat<std::string, float>::value);

// 2. At runtime, one type (or by its name: setNamedElement("Stat<std::string,float>", …))
rpe::TypeBridge::setNamedElement<Stat<std::string, float>>("name", "value");

// 3. At runtime, EVERY instantiation of the template — including ones a plugin
//    registers later. Matched by the registered name ("Stat<…>", or "game::Stat<…>"
//    for "Stat"), so register them under names that start with the template's.
rpe::TypeBridge::setNamedElementTemplate("Stat", "name", "value");

// Off for one type (whatever the metadata or the template say):
rpe::TypeBridge::setNamedElement<Stat<int, int>>("");
```

- **The name can be any type with a text:** `std::string`, `std::string_view`,
  `const char*`, `QString`, an enum (its enumerator's name), a number, a getter.
  It's shown, **never edited** (it's read-only everywhere, the mirror included).
- **The value** is edited on the row with its usual editor and hints. Without
  `ElementValue` — or when the value is itself a struct — the row is labelled by
  the name and expands to the fields. Any other fields are rows below it.
- **Search and the filter** find an element by its name or its value, as one row.
- **Pins follow the element by name.** A pin made on a named row is kept as
  `stats.{armor}.value` and shown as `Character.stats.armor`; reorder or resize
  the array and it still shows `armor`'s value (`—` while there's no `armor`).
  Paths accept `{name}` anywhere: `getValueByPath(obj, "stats.{armor}.value")`.
  Two elements with the same name, or a name containing `.` or `}`, are pinned
  by index instead.
- **Mirror mode** reads the names on the simulation thread and sends them as
  text: a `string_view` / `const char*` name points into the simulation's
  memory, which the GUI never reads.
- Applies to **sequential** containers (`std::vector`, `std::array`, `std::list`,
  …). A single struct of a marked type, not in an array, shows as usual.
- **Marking at runtime takes effect on screen** — open grids lay their arrays out
  again (the browser on its next poll; a bare `PropertyEditor` on its next
  refresh / value, or call `model()->syncNamedElements()`).

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

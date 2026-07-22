# PropertyEditor — Expansion Plan

> Now living on `new-features`. Two axes of expansion:
>   - **C++ TYPE support** — which value types the grid can display/edit.
>     Substantially advanced (see §0 below); this is the "türler" axis.
>   - **METADATA support** — richer producer metadata the grid reads (units,
>     categories, pretty enum labels, type-level labels). Still a design/spec
>     (§2 onward); nothing here is implemented yet.

---

## 0. C++ type support — status & feasibility

### Implemented

| Type | Editing | Notes / registration |
| --- | --- | --- |
| arithmetic (all widths, incl. 64-bit) | ✅ | wide ints use a validated line edit, never clamped through `int` |
| `bool` | ✅ | checkbox |
| `enum` (single choice) | ✅ | combo |
| **`enum` bitmask / flags** | ✅ | `hint::Flags` + `RPE_REGISTER_FLAGS(T)` for combined writes |
| `std::string`, `QString` | ✅ | |
| **`std::wstring`, `std::u16string`, `std::u32string`** | ✅ | share the string editors |
| **`std::string_view` / `std::wstring_view`** | display only | read-only by design (view into owned memory) |
| `QColor` | ✅ | swatch + picker |
| `std::filesystem::path` | ✅ | auto file/folder editor |
| **`QDateTime`** | ✅ | calendar popup |
| **`std::chrono` durations** (6 aliases) | ✅ | shown as `250 ms`; unit-suffixed spin |
| sequential containers (`vector`/`array`/`list`) | ✅ | per-element rows |
| **`std::map` / `std::unordered_map`** | ✅ | one row per key (`scores.[alice]`), sorted |
| nested structs | ✅ | expand into sub-rows; ≤4-field structs get a collapsed `[a, b]` summary |
| **`std::pair<A,B>`** | ✅ | plain RTTR reg, or `rpe::registerPair<A,B>()` |
| **`std::optional<T>`** | ✅ | `RPE_REGISTER_OPTIONAL(T)`; shows `(none)` |
| **`std::shared_ptr<T>`** | ✅ | expands to pointee, edits in place; null → blank (safe) |
| raw `T*` | display/edit via pointee | falls out of the shared_ptr `rawType` deref |

### Candidates — feasibility (RTTR 0.9.6)

Effort is gauged against the constraints already probed: RTTR can turn an enum
**into** an int but not the reverse; its associative view has no `set_value`
(edit = erase+insert); smart pointers unwrap to a raw pointer; and it reflects
none of the std wrapper/tuple templates on its own.

| Type | Feasibility | Effort | Verdict |
| --- | --- | --- | --- |
| `std::set` / `std::unordered_set` | element rows are read-only (keys aren't editable); already show a count | Low | Do if a component uses one |
| `std::tuple<...>` | a `registerTuple<Ts...>()` capturing element accessors, mirroring `registerPair` | Medium | Only when actually used |
| `QVector` / `QStringList` / `QMap` / `QHash` | need RTTR container-trait registration for the Qt types; then reuse the seq/assoc paths | Medium | Prefer std containers; skip unless required |
| Qt value types (`QPointF`, `QSize`, `QRectF`, `QVector3D`) | register as plain structs (x/y/…); collapsed summary already covers them | Low | Easy win if needed |
| `std::weak_ptr<T>` | display-only via a `.lock()` snapshot; editing is unsafe | Low–Med | Read-only, if used |
| `std::chrono::time_point` | render as a date via the QDateTime path; needs an epoch decision | Medium | If timestamps appear |
| `std::unique_ptr<T>` | move-only ⇒ can't round-trip through `rttr::variant`; **needs a probe** | Unknown | Investigate before promising |
| `std::variant<...>` | type-switching row + a discriminator editor; RTTR has no support | High | Not recommended |
| `std::function` / callables | not meaningfully editable | — | Out of scope |

---

## Why this exists

Today the editor reads exactly **8 property-level metadata keys** (see
`include/rpe/core/EditorHints.h`) and **no type-level or enumerator-level
metadata at all** — `get_metadata` is only ever called on `rttr::property`
(`src/gui/PropertyModel.cpp`). That means a producer can emit rich metadata
(units, categories, friendly enum labels, component display names) and the
editor silently drops all of it.

The goal is to close those gaps generically: each new capability is one
metadata key + one well-scoped hook, so a new producer or a new editor type is
a small, local addition — never a rewrite.

---

## 1. Current contract (what the editor reads today)

### Property-level keys — `rpe::hint::*` (`EditorHints.h`)

| Key | Constant | Type | Consumed in |
| --- | --- | --- | --- |
| `rpe.min` | `hint::Min` | double | `PropertyModel::data` → `MinRole`; spin range |
| `rpe.max` | `hint::Max` | double | `PropertyModel::data` → `MaxRole`; spin range |
| `rpe.step` | `hint::Step` | double | `StepRole`; spin single-step |
| `rpe.decimals` | `hint::Decimals` | int | `DecimalsRole`; double-spin decimals |
| `rpe.label` | `hint::Label` | string | `_buildTree` → `PropertyNode::displayName` |
| `rpe.tooltip` | `hint::Tooltip` | string | `_buildTree` → `PropertyNode::tooltip` |
| `rpe.editor` | `hint::Editor` | string (`editor::*`) | `EditorHintRole`; delegate editor pick |
| `rpe.readOnly` | `hint::ReadOnly` | bool | `PropertyModel::flags` (forces non-editable) |

### Editor types — `rpe::editor::*`

| Value | Constant | Widget | Trigger |
| --- | --- | --- | --- |
| `default` | `editor::Default` | auto by type | (no hint) |
| `file` | `editor::FilePath` | `FilePathEditor(OpenFile)` | hint, or `std::filesystem::path` (auto) |
| `savefile` | `editor::SaveFile` | `FilePathEditor(SaveFile)` | hint |
| `dir` | `editor::Directory` | `FilePathEditor(Directory)` | hint |
| `color` | `editor::Color` | `ColorEditor` | hint, or `QColor` (auto) |
| `text` | `editor::Multiline` | `QPlainTextEdit` | hint (string types) |
| `slider` | `editor::Slider` | slider + spin | hint (needs Min/Max) |

All picking happens in `PropertyDelegate::_makeEditor` from the node's **declared
type** + the `rpe.editor` hint.

---

## 2. Supports to ADD

Each item below lists: the new metadata key, its value type and level, the
rationale, and the concrete code hook.

### 2.1 Units suffix — `rpe.units` (string, property-level)

**Gap.** Numeric editors and value cells show no unit (`kN`, `%`, `degC`).

**Behaviour.** Append the unit as a suffix on numeric editors and in the
read-only value display, e.g. `12.5 kN`.

**Hooks.**
- Add `hint::Units = "rpe.units"` to `EditorHints.h`.
- Add `UnitsRole` to `PropertyModel::PropertyRole`; return `metaString(prop, hint::Units)`.
- `PropertyDelegate::_makeEditor`: on `QDoubleSpinBox`/`QSpinBox`,
  `sb->setSuffix(" " + units)` (lines ~164/188).
- Value display: append units in `PropertyModel::data` for `Qt::DisplayRole`
  (column 1), so the suffix shows even when the row is not being edited.

### 2.2 Category grouping — `rpe.category` (string, property-level)

**Gap.** The property tree is flat (`_buildTree` appends every property directly
under its owner). No collapsible category sections like a UE5 Details panel.

**Behaviour.** Group sibling properties that share a `rpe.category` under a
synthetic, non-editable, bold header row; preserve first-seen category order;
properties with no category stay ungrouped (or fall under a default group).

**Hooks.**
- Add `hint::Category = "rpe.category"` to `EditorHints.h`.
- `PropertyNode`: add an `isCategory` flag (synthetic node, no `rttr::property`).
- `PropertyModel::_buildTree`: bucket child properties by category, emit a
  category `PropertyNode` parent per bucket, reparent the properties under it.
- `PropertyModel::flags`: category rows are selectable but never editable.
- Delegate: render category rows spanning both columns, bold, no editor.

> Note: this is the largest change (it alters tree shape). Keep it opt-in — when
> no property carries `rpe.category`, the tree stays exactly as today.

### 2.3 Pretty enum labels — per-enumerator display (enumeration metadata)

**Gap.** Enum combos show raw C++ enumerator names
(`PropertyDelegate.cpp:154` uses `get_enumeration().get_names()` verbatim), so a
user sees `Continuous`, not `Continuous Relight`.

**Source.** RTTR 0.9.6 has no per-enumerator metadata slot, so producers flatten
enumerator metadata onto the **enumeration** as `"<Enumerator>.<Key>"` — e.g.
SimMeta emits `rttr::metadata("Continuous.DisplayName", "Continuous Relight")`.

**Behaviour.** In the combo, show the label but store the enum value; in the
read-only display, map the enumerator name → label.

**Hooks.**
- Add a helper `enumLabel(const rttr::enumeration&, name)` that reads
  `e.get_metadata(name + ".DisplayName")` and falls back to the raw name.
- `PropertyDelegate::_makeEditor` enum branch: `cb->addItem(label, QVariant::fromValue(value))`.
- `TypeRenderer::toDisplayString` (or the model): apply `enumLabel` for
  enumeration values in the value column.
- (Optional) also read `"<Enumerator>.Tooltip"` for combo item tooltips.

### 2.4 Type-level label & tooltip — `rpe.label` / `rpe.tooltip` on the **type**

**Gap.** The editor reads only *property* metadata; component/type display uses
raw C++ type paths (e.g. `sim::propulsion::TurbofanEngine`). Producers already
put a friendly name on the type (`registration::class_<T>(...)( metadata(...) )`).

**Behaviour.** Show the type's `rpe.label` in the component list and as the
editor's title/root, with `rpe.tooltip` as its tooltip.

**Hooks.**
- Reuse the same keys at type level: `rttr::type::get_metadata(hint::Label)`.
- `ComponentListWidget` / `EntityComponentBrowser`: item text from the type
  label when present.
- `PropertyEditor` / `VariantEditor`: optional title label from the bound type.

---

## 3. Target metadata contract (after expansion)

| Key | Level | Type | Meaning |
| --- | --- | --- | --- |
| `rpe.label` | property **and type** | string | display name (falls back to name) |
| `rpe.tooltip` | property **and type** | string | tooltip / description |
| `rpe.min` / `rpe.max` | property | double | numeric bounds |
| `rpe.step` | property | double | spin step |
| `rpe.decimals` | property | int | float precision |
| `rpe.editor` | property | `editor::*` | explicit editor selection |
| `rpe.readOnly` | property | bool | force non-editable |
| `rpe.units` | property | string | **new** — unit suffix |
| `rpe.category` | property | string | **new** — grouping section |
| `<Enum>.DisplayName` | enumeration | string | **new** — pretty enumerator label |

Editor types (`editor::*`) are unchanged — full coverage already exists; this
plan makes the editor *feed* them from richer metadata.

---

## 4. Optional / future (not yet modeled)

- **EditCondition** — enable/grey-out a row based on another property's value.
- **Advanced** — rows collapsed by default under an "Advanced" disclosure.
- **ClampMin/Max vs UIMin/Max** — hard value clamp vs. slider-range-only.
- **Secret/password masking** — `QLineEdit::Password` echo mode for a hint.

These need new keys *and* new widget behaviour; defer until a producer models
them.

---

## 5. Design rules

- **Opt-in / backward compatible.** A missing key must leave current behaviour
  unchanged. New roles return an invalid `QVariant` when unset; grouping is a
  no-op when no `rpe.category` is present.
- **One key, one hook.** Every capability is a single metadata key plus a
  scoped read site — keeps the surface generic and easy to extend.
- **Read via the existing helpers.** Use `metaString` / `metaNumber` /
  `metaBool` in `PropertyModel.cpp`; add type-level and enumeration-level twins
  where needed rather than sprinkling raw `get_metadata` calls.

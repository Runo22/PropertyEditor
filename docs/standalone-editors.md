# Standalone editors (no ECS browser)

`rpe::VariantEditor` builds a full property grid for **any RTTR-registered
value**, completely independent of the `EntityComponentBrowser` and flecs.
Drop it in a dialog, a dock page, or a tool window.

Two modes, one rule of thumb:

| Mode | Call | Edits go to | Requires `TypeBridge` | Use when |
|---|---|---|---|---|
| **Owned copy** | `setVariant(v)` | an internal copy — read back with `variant()` | no | Apply/Cancel flows, editing a snapshot |
| **In-place (linked)** | `setLinked(type, ptr)` / `edit(obj)` | straight into your object | **yes** | live settings/config objects you own |

## 0. Prerequisites

Register the type with RTTR (this is what makes properties visible):

```cpp
#include <rpe/rpe.h>
#include <rttr/registration.h>

struct Light
{
    double intensity = 1.0;
    QColor tint = Qt::white;
    std::filesystem::path iesProfile; // gets a file-picker editor automatically
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Light>("Light")
        .property("intensity", &Light::intensity)(
            rttr::metadata(rpe::hint::Min, 0.0),
            rttr::metadata(rpe::hint::Max, 100.0))
        .property("tint", &Light::tint)
        .property("iesProfile", &Light::iesProfile);
}
```

For **in-place mode only**, additionally register the bridge (place it next to
the RTTR registration; it is what lets the editor type a raw pointer):

```cpp
rpe::TypeBridge::registerType<Light>();   // or RPE_REGISTER_COMPONENT(Light)
```

## 1. Owned copy — edit a variant, get the result via callback

You hold an `rttr::variant`; you want a callback with the **whole updated
variant** after every edit. The original variant is never touched.

```cpp
rttr::variant value = /* a registered struct, e.g. Light{} */;
std::function<void(const rttr::variant&)> onEdited = /* your callback */;

auto* editor = new rpe::VariantEditor;
editor->setVariant(value); // builds the grid from the variant's type + values

QObject::connect(editor, &rpe::VariantEditor::valueChanged, editor,
                 [editor, onEdited](const QString& /*path*/, const rttr::variant& /*leaf*/) {
                     onEdited(editor->variant()); // the whole updated struct
                 });

editor->show();
```

- `valueChanged(path, leafValue)` reports the edited **field** (`"tint"`,
  `"position.x"`, `"weights.[2]"`); the whole struct is `editor->variant()`.
- Load a new value at any time with another `setVariant(v)`.

## 2. In-place — edit your object directly (write-through)

Edits land in your object the moment they are committed; there is nothing to
copy back. **You guarantee the object outlives the editor binding.**

```cpp
// Somewhere with a stable lifetime (member, singleton, arena slot…):
Light headlight;

auto* editor = new rpe::VariantEditor;
editor->edit(headlight); // == setLinked(rttr::type::get<Light>(), &headlight)

// Optional: react to edits (the write has ALREADY landed in `headlight`).
QObject::connect(editor, &rpe::VariantEditor::valueChanged, editor,
                 [&headlight](const QString& path, const rttr::variant&) {
                     qDebug() << path << "changed; intensity is now" << headlight.intensity;
                 });

editor->show();
```

When the type is only known at runtime, use the non-template form:

```cpp
editor->setLinked(value.get_type(), objPtr); // objPtr: void* to a live object
```

In-place rules:

- The type **must** be `TypeBridge`-registered (see §0) — without it,
  `setLinked` cannot type the pointer and the editor stays empty.
- If the object is mutated **externally** (another system wrote to it), call
  `editor->refreshFromSource()` to re-read the rows.
- If the object **moves in memory** (vector reallocation, pool compaction),
  re-bind with `setLinked(type, newPtr)` before the next interaction.
- If another **thread** owns the object, do not use plain in-place editing —
  that is exactly what the ECS mirror path is for.

## 3. Shortcuts & trimmings

```cpp
// One-liner for a compile-time-known type (owned by you, same-thread):
editor->edit(myObject);                        // VariantEditor, in-place
// or, at the lower level, PropertyEditor directly:
propertyEditor->editObject(myObject);          // bind + WriteBack + provider

editor->setReadOnly(true);                     // inspector-only mode
editor->editor()->setToolbarVisible(false);    // hide filter/reset row
editor->setStyleSheet(rpe::darkStyleSheet());  // the built-in dark theme
```

`editor->editor()` exposes the underlying `rpe::PropertyEditor` for anything
else (filtering, expandAll, pinned-path tinting, …).

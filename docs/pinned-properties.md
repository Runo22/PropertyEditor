# PinnedPropertiesWidget — the cross-entity watch list

A pin is a **watch, not a local edit**: pinned properties keep following the
simulation and are shown together in one standalone widget, across any number
of entities and components. Rows are `Entity | Property | Value`; the Value
cell is editable in place.

Requires **mirror mode** — pinned values flow through the `EcsMirror` channel
(there is no producer to feed them in direct mode).

## Wiring (one call)

```cpp
rpe::PinnedPropertiesWidget pinWidget;        // standalone QWidget — host-owned
browser.setPinnedPropertiesWidget(&pinWidget);
myDock->addPage(&pinWidget);                  // put it anywhere (own dock page…)
pinWidget.setTitleVisible(false);             // the dock tab already titles it
```

That single call wires everything:

- the property tree's context menu gains **"Pin to watch list"** /
  **"Unpin from watch list"** — on values, and on whole arrays, structs and
  array elements too (see below),
- pinned rows of the current entity+component are tinted **teal** in the main
  tree (a local-edit draft's amber wins if both apply),
- the widget receives the mirror channel: values flow at ~30 Hz, edits go back
  to the simulation thread.

## Using the widget

- **Edit**: double-click the Value cell — you get the same type-specific editor
  as in the property grid (check box, spin box, combo, colour, file picker…).
  The editor is chosen from the property's **declared** type, so a pin is
  editable the moment it's created, before its first value arrives. The edit is
  applied sim-side, addressed to that pin's entity + component (with
  `ecs_modified_id`, so observers fire).
- **Read-only** leaves (no setter, `ReadOnly` hint, locked type — see
  [editor-hints](editor-hints.md#read-only-values)) never open an editor here
  either; they show the same faint lock and the reason in the tooltip.
- **Whole arrays / structs / elements** show a one-line summary of their content
  — `[1, 2, 3]`, `{speed, 4}`, `[{speed, 4}, {armor, 7}, … +12]` — that follows
  the content. They're read-only here (pin a field or element to edit it).
- **Array elements and map entries** (`items.[2].value`, `counts.[b]`) watch
  that index / key. When it isn't there any more — the array got shorter, the
  key was erased — the row shows a greyed **—** (no editor) instead of the last
  value, and the value comes back when the index / key does. The row is only
  removed when the component or the entity goes. An index pin follows the
  **slot**: after a reorder it shows whatever element is at that index now.
- **Unpin**: right-click → *Unpin* / *Unpin all*.
- While you are editing a cell, the live echo won't overwrite your typing.

## Host API

```cpp
pinWidget.pin(entityId, "EntityLabel", "game.Transform", "position.x");
pinWidget.unpin(entityId, "game.Transform", "position.x");
pinWidget.clearPins();
pinWidget.pins();                              // QVector<MirrorChannel::PinKey>
pinWidget.pinnedPaths(entityId, component);    // QSet<QString> — for tinting
connect(&pinWidget, &rpe::PinnedPropertiesWidget::pinsChanged, …);
```

Driving it without a browser is also possible: `pinWidget.setMirror(&mirror)`
(or `setChannel(mirror.channel())`) plus the `pin()` calls above.

## Producer-side notes

Pins are mirrored every pump independently of the selection, deduplicated per
pin (unchanged values publish nothing), and their component resolution is
cached — a handful of pins adds no measurable per-frame cost. A whole-array pin
reads (copies) its array every pump, like any read of it; it's compared by its
summary, so only changes among the elements it shows are published. A missing
path is published once (`PinValue::missing`), not every pump. Pin edits are
addressed explicitly (entity + component + path), so they work for entities
that are not selected anywhere.

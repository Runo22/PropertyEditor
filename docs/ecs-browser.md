# EntityComponentBrowser

The three-panel inspector: **entities → components → properties**. Entities and
components are listed alphabetically; when nothing is selected, the top entity
is selected automatically so the panels are never blank.

## Two modes

### Direct mode — the GUI may touch the world

```cpp
rpe::EntityComponentBrowser browser;
browser.setWorld(&world);                 // GUI-side world access
browser.setWorldAccess(guard);            // optional: lock if another thread owns it
```

The browser reads entities/components itself and refreshes values on a timer
(`liveUpdateIntervalMs`, default 20 ms). Every world touch runs under the
optional `AccessGuard`.

### Mirror mode — the world lives on a simulation thread

```cpp
rpe::EcsMirror mirror;                    // created/attached on the SIM thread
mirror.attach(&world);

rpe::EntityComponentBrowser browser;      // GUI thread
browser.setMirror(&mirror);               // safe to call from the sim thread too
```

The GUI never touches the world; data flows through a thread-safe channel.
See [threading-mirror.md](threading-mirror.md) for the producer side.

## Settings

```cpp
rpe::EntityComponentBrowser::Settings st = browser.settings();
st.layout = rpe::EntityComponentBrowser::Layout::Vertical; // Unreal-style sidebar
st.requiredComponent = "Transform";       // entity-list filter…
st.requiredComponentEnabled = true;       // …only entities carrying it are listed
st.snapshotOpenFieldsOnly = true;         // mirror only the expanded rows
st.editPolicy = rpe::EditPolicy::LocalEdit;
st.allowComponentEditing = true;          // "+ Add" button and per-row trash
st.mirrorPollIntervalMs = 33;             // GUI poll cadence (mirror mode)
st.liveUpdateIntervalMs = 20;             // value refresh cadence (direct mode)
browser.setSettings(st);
```

The `requiredComponent` filter is also a **performance lever** in mirror mode:
the producer narrows its entity query to entities carrying that component, so
large worlds scan far fewer entities.

> Always drive the filter through `Settings` — a direct
> `EcsMirror::setRequiredComponent()` call gets overwritten the next time the
> browser re-applies its settings.

## Selection

```cpp
// React to what the user selects (GUI-thread signals, both modes):
connect(&browser, &rpe::EntityComponentBrowser::entityIdSelected,
        this, [](qulonglong id) { /* highlight in the 3D view… */ });

// Programmatic selection — public slots, queued-connection safe:
browser.selectEntity(entity);             // flecs::entity overload (registered metatype)
browser.selectEntity(entityId);           // raw id overload
QMetaObject::invokeMethod(&browser, "selectEntity",
                          Qt::QueuedConnection, Q_ARG(flecs::entity, e));
```

If the requested entity is not in the list yet (mirror still loading), the
request is remembered and applied when it appears.

`entitySelected(flecs::entity)` additionally fires in **direct mode only**
(mirror mode has no world handle on the GUI thread).

## Add / remove components

With `allowComponentEditing = true` the component panel gains a **"+ Add"**
button (popup grouped by namespace, filter box with arrow/Enter navigation)
and a per-row trash glyph with a two-step confirm. Structural changes are
applied on the simulation thread in mirror mode.

## Related

- Pin properties to a cross-entity watch list: [pinned-properties.md](pinned-properties.md)
- Edit semantics (LocalEdit drafts vs write-back): [getting-started.md](getting-started.md)

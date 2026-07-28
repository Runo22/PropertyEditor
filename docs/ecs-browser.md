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

## Tags & pairs

The component list shows the entity's full composition. Zero-size **tags** and
dataless **pairs** appear as dimmed badge rows — presence state, removable but
not selectable. A pair that **carries data** (flecs' `ecs_get_typeid` rule: the
relation's type first, else the target's) is a normal selectable row displayed
as `Damage → Fire` with a `pair` badge: clicking it opens the regular property
editor for the carried type, and edits write back to that pair instance.
Zero-size tags are also offered by the "+ Add" picker under a *(tags)* group.

## Add / remove components

With `allowComponentEditing = true` the component panel gains a **"+ Add"**
button (popup grouped by namespace, filter box with arrow/Enter navigation)
and a per-row trash glyph with a two-step confirm. Structural changes are
applied on the simulation thread in mirror mode.

## Add entities (spawn from prefabs)

`browser.setEntityAddingEnabled(true)` shows an **"Add"** button over the entity
list. It opens a picker of spawnable **prefabs**, grouped by tag. The list is
narrowed by the same required-component filter as the entity list — only prefabs
that carry the required component are offered.

```cpp
browser.setEntityAddingEnabled(true);
// group prefabs by these tags (optional icon per group header):
browser.setPrefabGroups({ { "Enemy", enemyIcon }, { "Prop", QIcon() } });
```

The producer scans prefab entities, files each under the first of your group
tags it carries, and publishes the list. Picking one queues a spawn to the
simulation thread, which instantiates it with `is_a(prefab)` and then calls a
**configurator you set on the mirror** so you can set/override components on the
new instance (setting *after* `is_a` overrides the prefab's shared value — the
flecs idiom):

```cpp
mirror.setSpawnConfigurator([](flecs::entity e) {
    e.set<Transform>({ spawnPos });   // per-instance override
    e.set<Health>({ 100 });
});
```

The configurator runs on the simulation thread where the world is owned, so it
touches the entity directly and safely. In direct mode the pick spawns under the
world guard instead.

## Related

- Pin properties to a cross-entity watch list: [pinned-properties.md](pinned-properties.md)
- Edit semantics (LocalEdit drafts vs write-back): [getting-started.md](getting-started.md)

# EntityComponentBrowser

The three-panel inspector: **entities → components → properties**. Entities and
components are listed alphabetically; when nothing is selected, the top entity
is selected automatically so the panels are never blank.

## Contents

- [Two modes](#two-modes) — direct vs. mirror
- [Settings](#settings)
- [Selection](#selection)
- [Keyboard](#keyboard)
- [Search all components](#search-all-components)
- [Tags & pairs](#tags--pairs)
- [Add / remove components](#add--remove-components)
- [Health checks](#health-checks)
- [Add entities (spawn from prefabs)](#add-entities-spawn-from-prefabs)
- [Deleting + right-click menus](#deleting--right-click-menus)
  - [Custom menu actions & hooks](#custom-menu-actions--hooks)
- [Related](#related)

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
st.preferredComponents = { "game::Player", "Transform" }; // auto-selected component (see Selection)
st.dragToScrub = true;                    // drag a number's name to change it
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

**Which component is selected** when an entity is selected:

1. the component you were on, if the new entity has it (or one with the same
   short name in another namespace);
2. otherwise the first of `Settings::preferredComponents` the entity has — names
   as full path (`game.Player` / `game::Player`) or a trailing part (`Player`);
   `browser.setPreferredComponents({...})` sets just this;
3. otherwise the default: direct mode the first row; mirror mode the row where
   the previous selection was (nothing on the very first entity).

A component **removed from the entity you're on** hands the selection to the row
that took its place — the preference is for arriving at an entity, not for that.

`entitySelected(flecs::entity)` additionally fires in **direct mode only**
(mirror mode has no world handle on the GUI thread).

All three panels have a **filter box**: the entity and component lists narrow by
typed text, and the property grid's filter matches a row's value too (see
[getting-started](getting-started.md#trimmings)).

## Keyboard

Every search-then-pick surface in the browser behaves the same way: **you type,
the best match is already highlighted, and focus never leaves the box** — so you
can keep typing at any point in the walk.

In the entity and component **filter boxes**:

| Key | |
|---|---|
| `Down` / `Up` | walk the visible rows (which selects them) |
| `PageDown` / `PageUp` | jump a screenful |
| `Enter` | hand focus to the list, to carry on there |
| `Esc` | clear the filter |
| `Tab` | leave for the **next panel** |

…and from the **list** itself, `Tab` (or `Shift+Tab`) hands focus straight back
to that panel's filter box, with your filter text intact and the caret at its
end — so a panel is a loop: type, `Enter` or `↓` into the rows, browse, `Tab`
back to refine.

The rows are deliberately **not** in the Tab chain (they are reached by click,
`Enter` or the arrows). That is what keeps the loop escapable: `Tab` from a
filter box carries on to the next panel instead of dropping straight back into
the rows it just came from.

In the **"Add" pickers** (spawn a prefab, add a component) the same keys drive
the option tree, plus:

| Key | |
|---|---|
| `Enter` | take the highlighted option — with nothing walked to yet, that is the **topmost match**, so filter-then-`Enter` picks the obvious one |
| `Tab` / `Shift+Tab` | walk the options too, wrapping around (a popup has nowhere else to tab to) |
| `Esc` | close without picking |

Hidden rows are never walked, group headers and the "(nothing available)"
placeholder are never highlighted, and `Enter` with no match does nothing rather
than picking something else. `Home` / `End` stay with the text caret.

Across the browser, `Tab` runs **write-back checkbox → entity filter →
component filter → property grid**.

## Search all components

The property filter (above the grid) searches the **selected entity's other
components too**:

- **While the filter has text**, the component list shows a **match count** on
  every component with a match and dims the ones without — so "it's in Weapon"
  is visible without leaving the component you're on. The grid itself still
  filters only the selected component.
- **The scope button** (the layers icon beside the filter, or `Ctrl+Shift+F` —
  see below to change the keys)
  switches the grid to **all components**: every match, grouped under a header
  per component (with its count), live values, matched text highlighted.
- **Rows there edit in place**, with the usual editors, and the edit goes to that
  row's own component. Pinning works too.
- **Picking a row** follows its component in the list. **Turning the scope off**
  goes back to one component — the one you picked — with the filter kept and
  the row selected. Picking a component in the list while the scope is on just
  scrolls to its group.

A row matches by its **name** (the label shown) or its **value** text, like the
filter always did; a nested struct matching by name shows as one row, and a
container matches by its name or any element. A row whose value stops matching
drops out.

**Cost.** Nothing runs while the filter is empty. With text, mirror mode searches
on the simulation thread a round every ~250 ms, **sliced across pumps with a
~0.2 ms budget each**, so no frame pays for a whole round. Measured on an entity
with 40 components (~1000 fields, 40 arrays of 64 floats, Release build): the
99.9th-percentile pump went from 0.20 ms to 0.30 ms for a name search like
`speed`, 0.34 ms for a number like `1.25` (the worst case: every number is
rendered to text — skipped entirely when the text can't occur in a number).
The budget is checked between components, so ONE component can't be split: a
component holding a very large container still costs its whole read in one
pump (containers of numbers are skipped when the text can't occur in a number;
a container's text is matched on its first 256 elements).
Direct mode searches on the GUI thread on the same timer.

`PropertyEditor` on its own has no scope button; `setSearchAllAvailable(true)`
shows it, and a host feeds the results with `bindGroups()` (see
`PropertyModel::bindGroups`) and the header counts with
`model()->setGroupCounts()`. Its key is `Ctrl+Shift+F` by default — if your
application uses those keys, Qt fires neither; pick others with
`setSearchAllShortcut(…)` (an empty sequence turns it off). The search itself is `rpe::searchProperties`
(`rpe/core/PropertySearch.h`).

## Tags & pairs

The component list shows the entity's full composition. Zero-size **tags** and
dataless **pairs** appear as dimmed badge rows — presence state, removable but
not selectable. A pair that **carries data** (flecs' `ecs_get_typeid` rule: the
relation's type first, else the target's) is a normal selectable row displayed
as `Damage → Fire` with a `pair` badge: clicking it opens the regular property
editor for the carried type, and edits write back to that pair instance.
Zero-size tags are also offered by the "+ Add" picker, under a **Tags** group.

## Add / remove components

With `allowComponentEditing = true` the component panel gains a **"+ Add"**
button (popup grouped by namespace, filter box with [arrow/Enter
navigation](#keyboard)) and a per-row trash glyph with a two-step confirm.
Structural changes are applied on the simulation thread in mirror mode.

The picker is a **namespace tree**: every scope segment is its own level, for
either separator, so `plugins.output.hud.Speed` and `plugins.output.ig.Radar`
file under **plugins ▸ output ▸ hud / ig**. Unscoped components sit under
*(global)*; tags get their own **Tags** root, last, nested the same way. The
filter matches a component's full path, so typing a namespace (`hud`) narrows
to that branch.

## Health checks

When a component doesn't show, binds the wrong type, or the picker stays flat,
ask: `rpe::checkHealth(world, { requiredComponent, prefabGroupTags })` returns a
report of what's wrong and how to fix it — nothing runs or logs by itself. Call
it on the simulation thread. Every check is listed in
[health-checks.md](health-checks.md).

### "My component isn't in the Add menu"

A component is offered when **all** of these hold:

1. it exists in the world as a named flecs component (`world.component<T>(…)`);
2. it is **bridged** — `rpe::TypeBridge::registerType<T>()` (or
   `RPE_REGISTER_COMPONENT(T)`) was called — or it is a zero-size tag. RTTR
   registration alone is not enough: the inspector needs the compile-time `T` the
   bridge captures. `rpe::checkComponents(world)` tells you when RTTR knows the
   type but the bridge doesn't (see [Health checks](#health-checks));
3. the bridge call reached the **same `rpe_core`** the host uses — a plugin that
   links its own static copy registers into a registry the host never sees;
4. its top-level namespace isn't in `hiddenAddNamespaces` (default: `settings`);
5. the selected entity doesn't already have it.

Plugins can register in any order (flecs first or bridge first, before or after
the browser starts): the catalog follows component and bridge registrations, and
is also refreshed whenever an Add menu is about to open.

Top-level namespaces in `Settings::hiddenAddNamespaces` are not offered at all —
by default `settings` (case-insensitive), for world-wide configuration
components that are never added to an entity. Change it with
`setHiddenAddNamespaces({...})` or via `setSettings()`; an empty list offers
everything. This only affects the Add picker — an entity that already carries
such a component still lists it.

## Add entities (spawn from prefabs)

`browser.setEntityAddingEnabled(true)` shows an **"Add"** button over the entity
list. It opens a picker of spawnable **prefabs**, grouped by tag, driven by the
same [keyboard](#keyboard) as the component picker. The list is narrowed by the
same required-component filter as the entity list — only prefabs that carry the
required component are offered.

```cpp
browser.setEntityAddingEnabled(true);
// group prefabs by these tags (optional icon per group header):
browser.setPrefabGroups({ { "game::npc::Enemy", enemyIcon }, { "Prop", QIcon() } });
```

Give the tag exactly as the world knows it: its **full path**
(`game::npc::Enemy`) or an **alias** you registered with `world.use(tag,
"Enemy")` — the producer resolves it with `world.lookup()`, and that is also
what the icon is keyed on. `lookup()`'s separator is `::`, so a **dotted**
spelling (`game.npc.Enemy`) is *not* found and the picker silently ends up
ungrouped. Call order does not matter — before or after `setMirror()`. The picker
**displays only its leaf** (`game::npc::Enemy` → **Enemy**, dotted paths too),
with the full name on the header's tooltip. The scope is only ever shortened for
display, so two groups sharing a leaf stay two separate groups.

The tag must be on the **prefab entity itself** — the producer asks
`prefab.has(tag)`, and instances inherit the tag through `is_a`, not the other
way round. All three ways this goes wrong warn once on the console instead of
just leaving the picker flat (and `rpe::checkPrefabGroups(world, tags)` reports
the same on demand):

| symptom | cause |
|---|---|
| `tag "X" not found in the world` | the name isn't the tag's path or a `world.use()` alias (a **dotted** path is never resolved) |
| `…exists but no prefab carries it … It IS on N non-prefab entities` | the tag was added to the **instances**, not the prefab |
| `…A prefab uses it as a PAIR` | added as `prefab.add(tag, target)` — a pair id is not the tag id |

Grouping is optional: with **no** `setPrefabGroups`, the picker is a flat,
alphabetical list (no group headers). Add groups only when you want the prefabs
split into sections. The producer scans prefab entities, files each under the
first of your group tags it carries, and publishes the list. Instances are named
after the prefab (minus a trailing `" Prefab"` suffix), uniquified to avoid
flecs' duplicate-name abort — `"Zombie Prefab"` → `Zombie`, `Zombie (1)`, …

Picking one queues a spawn to the simulation thread, which instantiates it with
`is_a(prefab)` and then calls a **configurator you set on the mirror** so you can
set/override components on the new instance (setting *after* `is_a` overrides the
prefab's shared value — the flecs idiom):

```cpp
mirror.setSpawnConfigurator([](flecs::entity e) {
    e.set<Transform>({ spawnPos });   // per-instance override
    e.set<Health>({ 100 });
});
```

The configurator runs on the simulation thread where the world is owned, so it
touches the entity directly and safely. In direct mode the pick spawns under the
world guard instead.

## Deleting + right-click menus

`browser.setEntityRemovingEnabled(true)` adds a per-row **trash glyph** to the
entity list (two-step confirm, like the component list) and a **"Delete entity"**
entry to its right-click menu. Deleting destroys the entity — queued to the sim
thread in mirror mode, `destruct()` under the world guard in direct mode. The
component panel already offers the same for components (a `×` per row and a
"Remove component" menu entry) when `allowComponentEditing` is on.

### Custom menu actions & hooks

Both lists' right-click menus are extensible, two ways — a **static** action
(fixed label, registered once) or a **dynamic** hook (a function that builds
entries each time the menu opens):

| | Entity | Component |
| --- | --- | --- |
| **Static** | `addEntityAction` / `removeEntityAction` | `addComponentAction` / `removeComponentAction` |
| **Dynamic, many** | `addEntityMenuHook` / `removeMenuHook` | `addComponentMenuHook` / `removeMenuHook` |
| **Dynamic, one** | `setEntityMenuHook` | `setComponentMenuHook` |

Every `add*` returns an `int` handle for its `remove*`.

#### Static — a fixed label + callback, optionally in a submenu

```cpp
// Entity: the callback gets the clicked entity's id.
browser.addEntityAction({ "Focus camera", cameraIcon,
                          [](qulonglong entityId) { focusCameraOn(entityId); } });

// The 4th field files it under a submenu: Debug ▸ Physics ▸ Wake.
browser.addEntityAction({ "Wake", QIcon(), [](qulonglong id) { wake(id); },
                          { "Debug", "Physics" } });

// Component: the callback gets the selected entity id + the clicked component's
// key (a data component's path, or a pair's "Rel (Target)") and its flecs id.
browser.addComponentAction({ "Copy as JSON", QIcon(),
    [](qulonglong entityId, const QString& component, qulonglong rawId) {
        copyComponent(entityId, component);
    },
    { "Export" } });
```

#### Dynamic — a hook handed the live `QMenu`

Run each time the menu opens, so entries can depend on the entity or component —
conditional items, checkable actions, anything `QMenu` supports. Use
**`rpe::subMenu(menu, { … })`** for submenus: it finds or creates each level, so
static actions, several hooks and several plugins that name the same path share
**one** submenu instead of each adding its own "Debug".

```cpp
// A plugin: add on load, keep the handle, remove before unloading.
int handle = browser.addEntityMenuHook([](qulonglong id, QMenu& menu) {
    QMenu* physics = rpe::subMenu(menu, { "Debug", "Physics" });
    physics->addAction("Freeze", [id] { freeze(id); });
    if (isSleeping(id))
        physics->addAction("Wake", [id] { wake(id); });
});
// …
browser.removeMenuHook(handle);

// The host's own hook (one slot, replaced on each call; nullptr clears it).
browser.setComponentMenuHook(
    [](qulonglong entityId, const QString& component, qulonglong rawId, QMenu& menu) {
        rpe::subMenu(menu, { "Export" })->addAction("Copy as YAML", [=] { copyYaml(entityId, rawId); });
    });
```

**Order in the menu:** built-in Delete / Remove, then static actions (in the
order added), then the `set*` hook, then the `add*` hooks (in the order added). A
submenu sits where its path was first used.

**Why a path list and not `"Debug/Physics"`:** a label may legitimately contain
`/` ("I/O", "Save/Load"); a list needs no escaping rules. A level matches an
existing submenu by its title with `&` mnemonics ignored (`"&Debug"` and `"Debug"`
are the same submenu). Blank levels are skipped.

**Plugins:** remove what you added **before the DLL unloads** — the stored
callbacks are code in that DLL (see [pitfalls](pitfalls.md#unloading-a-plugin)).
Use `add*MenuHook`, not `set*MenuHook`: the `set*` slot holds one hook, so a
second plugin calling it silently replaces the first.

Callbacks run on the GUI thread; component callbacks also receive the
currently-selected entity's id. The component key is the same in direct and
mirror mode.

## Related

- Pin properties to a cross-entity watch list: [pinned-properties.md](pinned-properties.md)
- Edit semantics (LocalEdit drafts vs write-back): [getting-started.md](getting-started.md#edit-policies)
- Component names, namespaces, plugins: [registering-types.md](registering-types.md)
- What goes wrong and why: [pitfalls.md](pitfalls.md)

# Pitfalls, edge cases & things to avoid

Most of these were found the hard way. Start with the symptom table; the sections
below explain each case. When in doubt, run
[`rpe::checkHealth(world)`](health-checks.md) first — it catches many of them.

## Contents

- [Symptom → cause](#symptom--cause)
- [Threading](#threading)
- [Registration & names](#registration--names)
- [Plugins & builds](#plugins--builds)
- [Prefabs & flecs](#prefabs--flecs)
- [Editing semantics](#editing-semantics)
- [Performance](#performance)
- [Limits by design](#limits-by-design)

## Symptom → cause

| Symptom | Likely cause | Look at |
|---|---|---|
| Component not listed, or missing from the Add menu | not bridged; plugin links its own `rpe_core`; namespace hidden (`settings`); entity already has it | `checkComponents` → `unbridged-rttr-type`; [Add-menu checklist](ecs-browser.md#my-component-isnt-in-the-add-menu) |
| Values look **shifted** / wrong fields for a component | the component is bound to the **wrong type** (same short name, different namespace) | `checkComponents` → `size-mismatch`, `same-type-twice`, `ambiguous-name` |
| Component listed but shows **no fields** | its bridged type has no RTTR properties (registration in a translation unit the linker dropped, or under another type) | `checkTypeRegistry` → `no-properties` |
| `Min`/`Max`/`Label`… ignored on a plugin's type | raw string keys, or a plugin built against old headers | [Hints from plugins](#hints-from-plugins) |
| Add-entity picker is flat (no groups) | group tag not found (dotted name), on instances instead of prefabs, or used as a pair | `checkPrefabGroups` |
| Entity list **empty** with a required component | the name doesn't resolve — a scoped name only matches its scope | `checkRequiredComponent` |
| Process **aborts** when spawning a prefab | a **move-only** component on the prefab | [Move-only components on prefabs](#move-only-components-on-prefabs) |
| Crashes that make no sense, "stack smashing", impossible values | two flecs versions in one process, or objects compiled against different headers | `checkFlecsBuild`; [Mixed builds](#mixed-builds) |
| Crash later inside the mirror's pump (`world.lookup`) | the world was touched from a thread that isn't running `progress()` (e.g. `attach()` from the main thread) | [Threading](#threading) |
| Mirror pumps at **half** the expected rate | `setMaxPumpRateHz` set equal to the sim's frame rate | [Performance → rate cap aliasing](#performance) |
| A typed value **snaps back** | read-only (no setter / locked type), or clamped by `Min`/`Max` | [Editing semantics](#editing-semantics) |
| Uncapped sim spends time in the mirror | no rate cap — the mirror pumps every frame | [Performance](#performance) |

## Threading

- **A flecs world allows one thread at a time.** Everything that touches it —
  creating entities/components, `mirror.attach()` (it installs a system), your
  plugin's `world.component<T>()` — runs on the thread that runs `progress()`.
  `attach()` only defers itself automatically when called *inside*
  `progress()`; called from the main thread while the sim thread is running, it
  races and crashes later.
- **The GUI never touches the world in mirror mode.** Only the channel calls
  (`setInterest`, `poll*`, `queueEdit`, `setPins`, `setMaxPumpRateHz`) are
  thread-safe. The world-taking [health checks](health-checks.md) belong on the
  sim thread too.
- **Widgets are GUI-thread only** — except `PropertyEditor::setPropertyValue`,
  which coalesces from any thread. `browser->setMirror()` marshals itself to the
  GUI thread if called from elsewhere; prefer calling it on the GUI thread.
- **Direct mode with another thread owning the world:** install
  `setWorldAccess(guard)` so every world touch runs under your lock, or use
  mirror mode.
- **Destruction order is free:** destroying the `EcsMirror` on the sim thread
  before the GUI is fine — the GUI holds the channel, which just goes quiet.

See [threading-mirror.md](threading-mirror.md#which-thread-does-what).

## Registration & names

- **Same short name in two namespaces** (`game::Stats`, `ai::Stats`): register
  both under their **full** names in RTTR and bridge both. Never rely on the
  short name alone — the tie-break is deterministic, not correct.
- **flecs name ≠ C++ name:** if you re-scope or rename components when
  registering them with flecs, tell TypeBridge —
  `registerType<T>("render::")` (a namespace, completed with T's leaf) or the
  full name — or better, `rpe::bindComponent<T>(world)` **after** your own
  `world.component<T>(…)`.
- **`bindComponent` before flecs knows the type does nothing** — it never
  registers or renames a component (doing so made flecs raise a redefinition
  error). Call it after.
- **Prefab group tags and `world.lookup()`:** `lookup` splits on `::`, so a
  **dotted** tag name (`game.npc.Enemy`) is never found. Use `game::npc::Enemy`
  or a `world.use()` alias.
- **Required component:** give the full path (`game::Player` or `game.Player`).
  A scoped name only ever matches that scope; a bare leaf matching several
  components picks the shortest path (and the mirror warns once).
- **Same entity names are fine** — entities are identified by id. Only names
  within one flecs *scope* must be unique; spawned instances are uniquified
  (`Zombie`, `Zombie (1)`, …). Your own `set_name` with a duplicate in the same
  scope makes flecs abort.
- **Namespaces named like flecs:** only the real `flecs` scope is treated as
  built-in. (A user namespace like `flecsx` was hidden by older versions.)

## Plugins & builds

### Hints from plugins

Use the `rpe::hint::*` constants, never `"rpe.min"`-style strings, and rebuild
plugins against the current headers. RTTR compares a string metadata key by
address, and each module has its own copy of a literal — so string keys from a
plugin are invisible to rpe. See
[editor-hints](editor-hints.md#why-the-constants-and-never-strings).

### One `rpe_core`, one flecs

- `rpe_core` must be **shared** and linked by host and plugins alike: the
  TypeBridge registry lives there. A static copy per module splits it.
- Host, rpe and plugins must use **one flecs library of one version**. rpe's own
  build fetches a pinned flecs; if your application uses another, build rpe
  against yours (see [getting-started](getting-started.md#use-your-flecs)).
  `checkFlecsBuild()` reports a mismatch as an Error.

### Mixed builds

Objects compiled against **different versions of a header** in one binary —
two flecs versions, or a partial rebuild after a header changed (e.g. editing
headers while a build runs) — produce crashes that make no sense: "stack
smashing detected", fields with impossible values. When in doubt: clean rebuild.

### Unloading a plugin

Call `rpe::TypeBridge::unregisterType<T>()` **before** the DLL is unloaded. After
that rpe never calls into the plugin's code; inspecting a component whose bridge
points into an unloaded DLL would.

## Prefabs & flecs

### Move-only components on prefabs

flecs v4's default `OnInstantiate` policy **copies** a prefab's components into
each instance — and a move-only component has an illegal copy hook, so
`is_a(prefab)` **aborts the process**. That includes spawning from the browser's
Add-entity menu.

| Policy | `is_a(prefab)` | Writing it on the instance |
|---|---|---|
| default (copy into instance) | **abort** | — |
| `(OnInstantiate, DontInherit)` | ✅ instance doesn't get it | — |
| `(OnInstantiate, Inherit)` | ✅ shared | **abort** on the first write (needs a copy) |

Use `DontInherit`, and add the component per instance in the spawn configurator
(`add<T>()` default-constructs, no copy):

```cpp
world.component<Speaker>().add(flecs::OnInstantiate, flecs::DontInherit);
mirror.setSpawnConfigurator([](flecs::entity e) { e.add<Speaker>(); });
```

### Group tags belong on the prefab

The picker asks `prefab.has(tag)`. A tag added to the **instances** (in the
configurator, say) or used as a **pair** (`prefab.add(tag, target)`) never groups
anything.

## Editing semantics

- **Mirror mode ignores `EditPolicy`.** Edits always go to the sim thread through
  the channel. `liveUpdateIntervalMs` is direct-mode only, too.
- **Edits land on the entity they were made on** — even if the selection moved
  before the sim applied them. If you queue edits through the channel yourself,
  address them: `channel->queueEdit(entity, component, path, value)`.
- **Read-only values never open an editor** (no setter, `ReadOnly` hint, locked
  type). Anything *below* a getter-only struct is read-only too — editing it
  would write into a temporary copy.
- **`Min`/`Max` is an editor range, not validation.** An out-of-range value from
  the simulation shows as-is; opening its editor clamps it, and committing (even
  an unchanged Enter) writes the clamped value.
- **`Min`/`Max`/`Step` don't apply to `unsigned int`, `long` or 64-bit
  integers** — they get a line edit so large values are never clamped.
- **Drive the required-component filter through `Settings`.** A direct
  `EcsMirror::setRequiredComponent()` is overwritten the next time the browser
  re-applies its settings.
- **`setWorld()` and `setMirror()` are exclusive** — setting one leaves the other
  mode.

## Performance

- **Cap the mirror on uncapped sims:** `mirror.setMaxPumpRateHz(60)`. Without
  it the mirror pumps every frame (hundreds or thousands of times a second) for
  a GUI that polls at ~30 Hz.
- **Rate cap aliasing.** The cap is a strict minimum gap. Set it *equal* to the
  sim's frame rate and frame jitter makes it skip every other frame — half rate.
  Use a cap clearly above the normal frame rate (60 for a 30 Hz sim), or `0`.
- **Collapse what you don't need.** With `snapshotOpenFieldsOnly` (the default)
  collapsed rows aren't read on the sim thread at all, and the tree remembers
  what you collapsed per type across entity switches. (Small structs of ≤ 4
  fields stay watched to render their `[a, b]` summary.)
- **Use a required component** on big worlds: the entity scan only visits
  entities that carry it.
- **Debug builds** are several times slower in every scan. Measure with
  `mirror.pumpStats()` (`lastPumpMs`, `maxPumpMs`, `lastScanMs`,
  `catalogScans`); the definitive A/B is `mirror.detach()`.

## Limits by design

- The entity list shows at most **5000** rows (in id order when it caps); the
  selected entity is always kept in it.
- **Map keys** aren't editable; string keys containing `.` or `]` can't be
  addressed by path.
- **`std::string_view` / `std::wstring_view`** are always read-only.
- **Flags enums** need `RPE_REGISTER_FLAGS(E)` to *edit* combined values.
- **`rpe::editor::Slider`** is declared but not implemented: the property gets
  its normal spin box.
- **`TypeBridge::clone()`** returns an invalid variant for move-only types.
- **flecs pairs without data** are badge rows only; pairs carrying data edit the
  carried type.

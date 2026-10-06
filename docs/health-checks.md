# Health checks

On-demand diagnostics for "why doesn't my component show up / bind / edit the
way I expect?" — in `rpe/ecs/HealthCheck.h`.

**Nothing runs or logs by itself.** Call what you need when you need it, read the
report, or print it. No `EcsMirror` or browser is required.

```cpp
#include <rpe/ecs/HealthCheck.h>
#include <QDebug>

// On the thread that owns the world — e.g. right after world.progress():
rpe::HealthReport report = rpe::checkHealth(world, {
    /*requiredComponent=*/ "game::Player",
    /*prefabGroupTags=*/   { "game::npc::Enemy", "game::Prop" },
});

if (report.hasErrors() || report.hasWarnings())
    qInfo().noquote() << report.toText(rpe::HealthIssue::Severity::Warning);
```

## Contents

- [Threading](#threading)
- [The report](#the-report)
- [Functions](#functions)
- [Every check](#every-check)
- [Explaining one name](#explaining-one-name)
- [Recipes](#recipes)

## Threading

Functions that take a `flecs::world` **read the world**: call them where you may
touch it — on the simulation thread (after `progress()`, or from a system), or
with `progress()` stopped. In mirror mode the GUI thread must not call them.
`checkTypeRegistry()` and `checkFlecsBuild()` need no world and are safe anywhere.

## The report

```cpp
struct HealthIssue {
    enum class Severity { Info, Warning, Error };
    Severity severity;
    QString check;    // stable id, e.g. "size-mismatch"
    QString subject;  // what it's about: a component path, a type or tag name
    QString message;  // what's wrong
    QString fix;      // what to do (may be empty)
};
```

| Severity | Meaning |
|---|---|
| **Error** | definitely wrong: values would be read/written incorrectly |
| **Warning** | probably a mistake: something won't show or bind as expected |
| **Info** | worth knowing; often intentional |

`HealthReport` helpers: `hasErrors()`, `hasWarnings()`, `isClean()`,
`count(atLeast)`, `byCheck("size-mismatch")`, `about("game.Stats")`,
`append(other)`, and `toText(minimum)` — one block per issue, most severe first,
with a `fix:` line; `"No issues."` when there is nothing at or above `minimum`.

## Functions

| Function | World? | Checks |
|---|---|---|
| `checkComponents(world)` | yes | every named, non-built-in component against the bridge registry |
| `checkTypeRegistry()` | no | the TypeBridge registry on its own |
| `checkRequiredComponent(world, name)` | yes | a required-component name, resolved exactly as the browser's filter does |
| `checkPrefabGroups(world, tags)` | yes | prefab group tags, resolved exactly as the Add-entity picker does |
| `checkFlecsBuild()` | no | the flecs build running vs the headers rpe was compiled against |
| `checkHealth(world, options)` | yes | all of the above; the required/tag checks only when given |

## Every check

### From `checkComponents`

| id | Severity | When | Fix |
|---|---|---|---|
| `size-mismatch` | **Error** | the component binds an RTTR type of a **different size** — its values would be read and written through the wrong layout ("values look shifted") | bind the right type: `registerType<T>("flecs::path")` / `bindComponent<T>(world)` |
| `same-type-twice` | Warning | two components bind the **same** RTTR type — at most one of them really is it | bind each to its own type, as above |
| `ambiguous-name` | Warning | the name matched several bridged types; one was picked by a tie-break | make it exact, as above |
| `short-name-only` | Info | bound only through its leaf name — works today, breaks when another type shares the leaf | make it exact, as above |
| `unbridged-rttr-type` | Warning / Info | RTTR knows a type by the component's **full** name, TypeBridge doesn't, so it is neither shown nor addable. **Warning** if entities carry it (it's in use and invisible); **Info** if none do yet | `TypeBridge::registerType<T>()` in the same `rpe_core` the host uses |
| `unbridged-maybe` | Info | a component **in use** isn't bridged, and an unbridged RTTR type with the same **short** name exists — possibly its type | if it is: `registerType<T>("flecs::path")` |
| `unused-bridge` | Info | a bridged type no component in this world binds | expected if its plugin isn't loaded; otherwise its flecs name doesn't match |

What `checkComponents` deliberately does **not** report:

- **value types** — any type that appears inside a bridged type (as a field, a
  container element, an `optional`'s value, recursively), like `Vec3` in
  `Transform::position` or `std::optional<int>`: RTTR knows them as fields, flecs
  may know them as components for its own reasons, and they were never meant to
  be bridged;
- `std::` types;
- unbridged **tags** (they're addable without a bridge);
- flecs' own built-in components;
- the short-name hint for a component no entity uses.

### From `checkTypeRegistry`

| id | Severity | When | Fix |
|---|---|---|---|
| `duplicate-rttr-name` | Warning | two bridged types are registered with RTTR under the **same name** — a name can't tell them apart | register each under its full namespaced name, or give each component an alias |
| `no-properties` | Info | a bridged type with no RTTR properties: lists and can be added/removed, but shows no fields | intended for markers; otherwise register its properties |

### From `checkRequiredComponent`

| id | Severity | When |
|---|---|---|
| `required-not-found` | Warning | nothing matches — the entity list stays empty. A **scoped** name only ever matches that scope |
| `required-ambiguous` | Warning | several components match; the shortest path is used |
| `required-inexact` | Info | matched by scope suffix or by short name, not by full path |

### From `checkPrefabGroups`

| id | Severity | When |
|---|---|---|
| `group-tag-not-found` | Warning | `world.lookup()` can't find the tag — notably a **dotted** spelling, which is never resolved; use `::` or a `world.use()` alias |
| `group-tag-on-no-prefab` | Warning | the tag exists but no prefab carries it; the message says whether it's on **instances** instead, or used as a **pair** |

### From `checkFlecsBuild`

| id | Severity | When |
|---|---|---|
| `flecs-version-mismatch` | **Error** | rpe was compiled against one flecs version, the process runs another — header/ABI mismatch, crashes that make no sense |
| `flecs-debug-build` | Info | flecs is a debug build (asserts on, slower) |

## Explaining one name

```cpp
auto why = rpe::TypeBridge::explainResolve("render.Stats");
// why.type        — the type it binds (invalid if none)
// why.via         — Alias, ExactName, ScopedSuffix, ShortName, or None
// why.candidates  — every bridged type that matched at that rung
```

More than one candidate at `ShortName` (or `ScopedSuffix`) means the binding was
a tie-break — see [registering-types](registering-types.md#how-a-flecs-component-finds-its-type).

## Recipes

**Fail a test / CI run on real problems:**

```cpp
const auto r = rpe::checkHealth(world);
if (r.hasErrors() || r.hasWarnings()) {
    std::fprintf(stderr, "%s\n", qPrintable(r.toText(rpe::HealthIssue::Severity::Warning)));
    return 1;
}
```

**Run once after plugins load** (on the sim thread), log anything at Warning or
above:

```cpp
const auto r = rpe::checkComponents(world);
if (!r.toText(rpe::HealthIssue::Severity::Warning).startsWith("No issues"))
    qWarning().noquote() << r.toText(rpe::HealthIssue::Severity::Warning);
```

**Ask about one component:**

```cpp
for (const auto& i : rpe::checkComponents(world).about("plug.Speaker"))
    qInfo() << i.check << i.message << i.fix;
```

**Ignore an id you've decided is fine:**

```cpp
auto r = rpe::checkComponents(world);
r.issues.erase(std::remove_if(r.issues.begin(), r.issues.end(),
                   [](const rpe::HealthIssue& i) { return i.check == "unused-bridge"; }),
               r.issues.end());
```

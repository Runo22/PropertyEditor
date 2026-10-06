# rpe documentation

## Start here

| I want to… | Read |
|---|---|
| build rpe and put a property editor on screen | [getting-started.md](getting-started.md) |
| make my types (and their fields) show up | [registering-types.md](registering-types.md) |
| control how a field is edited — ranges, pickers, labels, read-only | [editor-hints.md](editor-hints.md) |
| edit a value or an object without any ECS | [standalone-editors.md](standalone-editors.md) |
| inspect and edit a flecs world | [ecs-browser.md](ecs-browser.md) |
| …whose world runs on its own simulation thread | [threading-mirror.md](threading-mirror.md) |
| watch properties of many entities at once | [pinned-properties.md](pinned-properties.md) |
| find out why something doesn't show, bind or edit | [health-checks.md](health-checks.md) |
| avoid the known traps | [pitfalls.md](pitfalls.md) |

## All documents

| Doc | Covers |
|---|---|
| [getting-started.md](getting-started.md) | Build options and targets, integrating into your app, **using your flecs**, a first editor, edit policies (LocalEdit vs WriteBack), trimmings |
| [registering-types.md](registering-types.md) | RTTR vs TypeBridge, how a flecs name finds its type, namespaces and aliases (`registerType<T>("ns::")`, `bindComponent`), plugins, **move-only types**, value types, special types (`optional`, flags, maps, pairs, …) |
| [editor-hints.md](editor-hints.md) | Every `rpe::hint::*` key and `rpe::editor::*` value, the editor each type gets, quick-edit helpers (inline vector row, drag to scrub, colour swatch), **read-only values** (getter-only, `ReadOnly` hint, locked types), hint edge cases |
| [standalone-editors.md](standalone-editors.md) | `VariantEditor`: owned-copy editing with a callback, in-place (write-through) editing |
| [ecs-browser.md](ecs-browser.md) | `EntityComponentBrowser`: direct vs mirror mode, settings, selection, keyboard, tags & pairs, add/remove components, spawning prefabs, deleting, custom menus |
| [threading-mirror.md](threading-mirror.md) | `EcsMirror`: **which thread does what**, pump modes, rate cap and scan settings, diagnostics, how edits travel, guard mode |
| [pinned-properties.md](pinned-properties.md) | The cross-entity watch list: pinning, live values, in-row editing |
| [health-checks.md](health-checks.md) | On-demand diagnostics: every check, its severity and fix; `explainResolve`; recipes |
| [pitfalls.md](pitfalls.md) | Symptom → cause table, threading, names, plugins & builds, prefabs, editing semantics, performance, limits by design |

## Conventions

- Component **paths** are written the way flecs reports them, dotted
  (`game.Transform`); everywhere rpe accepts a component name it also accepts
  `::` (`game::Transform`). The one exception is prefab **group tags**, resolved
  with flecs' `world.lookup()`: use `::` or a `world.use()` alias there.
- Property **paths** are dotted from the component root: `position.x`,
  `lodBias.[2]`, `scores.[alice]`.

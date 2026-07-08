# Naming Conventions

This document is the source of truth for Aobus naming policy. It covers
identifier shapes, type and contract names, semantic vocabulary, file names, and
helper/support allocation.

Test case names and Catch2 tags are covered separately in
`doc/dev/testing/naming-and-assertions.md`.

## Goals

- Prefer names that expose the domain concept, ownership, lifetime, or boundary.
- Keep project-owned names strict by default; fix unclear code names instead of
  growing one-off exception rules.
- Keep external or compatibility vocabulary at the boundary and translate it
  before it becomes project-owned API.

## Identifier Shapes

- Types, classes, structs, enum classes, and scoped enum values use
  `PascalCase`: `TrackStore`, `Metadata`, `Code::IoError`.
- Functions and variables use `camelCase`: `loadMetadata()`, `trackCount`.
- Non-static class data members use `_camelCase`: `_handle`, `_tracks`.
- Struct data members use plain `camelCase`: `trackId`, `year`.
- Constants use `kCamelCase`: `kMaxSize`, `kDefaultFlags`.
- Unscoped constants use `kCamelCase`.
- Use normal project acronym casing inside `PascalCase` names: `Id`, not `ID`;
  `Metadata`, not ambiguous `Meta`, unless matching a real external boundary.

## Type And Contract Names

- Type names describe the domain concept, role, or public contract. Do not name
  a type after storage shape or declaration grouping when a concrete domain name
  exists.
- Do not prefix project-owned contracts or abstract interfaces with `I`. Use the
  contract name for the public role, such as `Backend` or
  `TrackListProjection`, and give concrete implementations a semantic qualifier,
  such as `LiveTrackListProjection`.
- Avoid catch-all type and header concepts such as `*Types`, `*Common`, and
  generic `*Model` when the declarations can be split or named by a concrete
  concept such as capability, descriptor, binding, metadata, or decoded stream
  information.
- Use `*Count` for cardinality and `*Number` for domain ordinals.
- Use `indices` for positional-index collections. Use `indexes` only for
  database/search index entities.

## Pointer Names

- Managed pointers (`std::shared_ptr`, `std::unique_ptr`, `std::weak_ptr`,
  `Glib::RefPtr`) must end with the `Ptr` suffix: `trackPtr`.
- Raw pointers (`T*`) must not use the `Ptr` suffix. They represent non-owning
  observers, cursors, or iterators.
- The `Ptr` suffix rule applies to variables, fields, and parameters that hold
  pointer values. Raw-pointer-returning helper functions may use established
  names such as `asPtr()` when the function name describes a view/conversion
  contract rather than ownership.
- Do not use Hungarian notation for pointer types: avoid `pBuffer` and `_pRow`;
  use semantic names such as `bufferData` or `_activeRow`.

## Optional Names

- `std::optional` variables, fields, and parameters must use an `opt` prefix:
  `optUri`, `optView`.
- Optional-returning functions should describe the domain rule, not the return
  container. Prefer `nonEmptyString()` or `dictionaryNameWhenPresent()` over
  names like `optionalString()`.
- Optional existence-check syntax is covered in `doc/dev/coding-style.md`
  because it is a C++ usage rule.

## Function And Method Names

### Accessors And Predicates

- Project-owned functions and methods use `camelCase`.
- Ordinary value and property accessors use the domain noun: `title()`,
  `trackIds()`, `trackCount()`, `currentTrack()`.
- Do not use `get*` just because a function returns a value. Reserve `get*` for
  framework or third-party signatures, established container/dictionary lookup
  vocabulary, compound acquisition such as `getOrCreate()` or
  `getOrDefault()`, and low-level interop handles where `get` is the external
  API vocabulary.
- Side-effect-free boolean queries must use predicate prefixes such as `is*`,
  `has*`, `can*`, `should*`, or `contains*`. Prefer `isEnabled()`,
  `isValid()`, `isEditable()`, `isEmpty()`, and `isEditing()` over
  `enabled()`, `valid()`, `editable()`, `empty()`, and `editing()`, except when
  matching an external boundary or standard-library-compatible API.
- Boolean-returning actions keep action verbs when the return value reports
  success, change, consumption, or completion: `applyPatch()`,
  `writeTrackFieldPatch()`, `bind()`, `goBack()`.
- Return one value with a singular noun: `trackId()`, `xPosition()`. Return a
  collection with a plural noun: `trackIds()`, `xPositions()`. Counts use a
  singular target: `trackCount()`, not `tracksCount()`.
- Use singular targets for single-item operations and plural targets for batch
  operations: `removeTrack(id)` versus `removeTracks(ids)`.

### General Verb Allocation

Use the narrowest verb that describes the observable contract.

| Verb family | Use |
| --- | --- |
| `make*` / `create*` / `build*` | `make*` constructs pure in-memory values or objects. `create*` creates persistent, registered, externally visible, or owned resources. `build*` assembles derived plans, trees, projections, or aggregates from existing inputs. Do not decide by return type. |
| `new*` | Do not add project-owned `new*` functions or methods. Use `make*` or `create*`. |
| `load*` / `read*` / `parse*` | `load*` materializes from durable sources. `read*` consumes bytes, records, fields, streams, or current state. `parse*` converts syntax, text, or binary representation into structured data. |
| `resolve*` / `find*` / `lookup*` | `resolve*` binds an id, name, or reference using context. `find*` searches locally and may not find a match. `lookup*` queries a table, catalog, schema, dictionary, or registry. |
| `write*` / `serialize*` / `emit*` / `dump*` | `write*` writes into a target object, file, storage, node, patch, or buffer. `serialize*` converts to storage or wire representation. `emit*` produces structured output, signal payload, or event text. `dump*` is diagnostic or user-requested raw/plain output. |
| `export*` / `import*` | Use for full transfer workflows across a boundary. |
| `to*` / `from*` / `as*` | `to*` creates a new representation. `from*` constructs or restores from a representation. `as*` returns a view, coercion, or access wrapper and must not imply ownership or expensive conversion. DTO helpers use `to*Dto()` and `from*Dto()`, not bare `*Dto()`. |
| `validate*` / `check*` / `ensure*` / `require*` | `validate*` checks business or format rules and returns a validation result or `Result`. `check*` is for tests, tools, diagnostics, or local consistency checks. `ensure*` establishes a postcondition and may create, initialize, repair, or error. `require*` enforces a mandatory precondition. |
| `try*` | Use only for expected failure or not-applicable paths that are not errors. True errors use `Result<T>` and a precise action name. |
| `compute*` / `calculate*` / `derive*` / `estimate*` / `measure*` | `compute*` deterministically derives a value without durable state changes. `calculate*` is numeric, geometric, time, size, or formula work. `derive*` infers domain meaning from source data. `estimate*` is approximate or heuristic. `measure*` observes external or runtime state. |
| `map*` / `translate*` / `convert*` / `adapt*` | `map*` is for key, id, index, or element mapping. `translate*` crosses namespaces, coordinate systems, protocols, or UI/domain boundaries. `convert*` changes type or representation, but prefer `to*` and `from*` when they fit. `adapt*` reshapes behavior or data for an interface, backend, or framework boundary. |
| `copy*` / `clone*` / `duplicate*` / `snapshot*` | `copy*` copies data to a target. `clone*` creates an equivalent independent object, especially owned or polymorphic objects. `duplicate*` is domain-level copying that creates a new identity or sibling. `snapshot*` captures static current state, not a live view. |
| `save*` / `store*` / `persist*` / `cache*` | `save*` is a user or business save workflow. `store*` places a value into storage, a container, a key-value backend, or a lower-level target. `persist*` guarantees cross-session or cross-process durability. `cache*` writes or maintains invalidatable derived data, not the source of truth. |
| `fetch*` / `request*` / `receive*` / `send*` / `post*` | `fetch*` actively obtains data from an external system, backend, remote, or cache-miss source. `request*` asks another layer to perform work. `receive*` passively accepts incoming data or events. `send*` transmits messages, commands, or data outward. `post*` enqueues asynchronous work or messages. |
| `print*` / `log*` / `report*` / `trace*` | `print*` writes directly to CLI/stdout/stderr or a stream. `log*` writes to the logging system. `report*` creates or submits aggregated diagnostics or results. `trace*` is low-level instrumentation. |
| `format*` / `describe*` / `label*` / `*Text` / `toString()` | `format*` creates presentation strings with formatting policy. `describe*` creates longer human or diagnostic text. `label*` creates short UI label text. `*Text` names existing text properties or payloads. Avoid `*String` names that only repeat the return type. Reserve `toString()` for generic enum or value conversion, not domain presentation formatting. |
| `diagnose*` / `explain*` | `diagnose*` analyzes problems and produces diagnostics or a report. `explain*` creates human-facing reasons; plain error text should usually use `format*` or `describe*`. |
| `process*` / `run*` / `execute*` | Avoid `process*` unless the function is truly a batch, stream, or work-item pipeline. Use `run*` for commands, tasks, tests, workflows, and loops. Use `execute*` for commands, instructions, actions, or executor contexts. Do not add `do*` or `perform*`. |
| `prepare*` / `configure*` / `finalize*` / `complete*` / `finish*` | `prepare*` establishes prerequisites for a later action. `configure*` sets options, policy, callbacks, or dependencies. `finalize*` ends a builder, serialization, transaction, or other phased flow and produces final state. `complete*` marks or advances work to success. Avoid new `finish*`; use `complete*`, `finalize*`, `close*`, or `commit*` when one is more precise. |

- Empty fluent builder factories use `makeEmpty()`, not `createNew()`.

### State, Lifecycle, And Time

- `set*` directly sets state or a property. `update*` computes, refreshes, or
  advances based on current state. `apply*` applies a patch, spec, event,
  decision, or state to a target. `refresh*` pulls, recomputes, or synchronizes
  current view/cache state.
- `reset*` returns something to default, initial, or empty state. `clear*`
  removes current content. `remove*` removes an item from a collection or
  parent. `delete*` deletes a persistent or domain entity. `erase*` is
  low-level container, storage, or STL-like removal. `toggle*` flips state; use
  `set*` when the target state is known.
- `open*` and `close*` are for resources, sessions, streams, files, database
  handles, and windows. `start*` and `stop*` are for ongoing activities,
  workers, timers, playback, or subscriptions. `pause*`, `resume*`, and
  `restart*` keep their ordinary lifecycle meanings.
- `begin*` starts a session, transaction, edit operation, or phase with a clear
  lifecycle. `end*` ends such a session or phase without implying success.
  `enter*` and `exit*` are for modes, states, and scopes.
- `initialize*` is the full spelling. Prefer constructors or factories over
  two-phase initialization; use `initialize*` only for explicit
  post-construction framework or external setup. Test arrangement may use
  `setup*`.
- `wait*` blocks for a condition, event, thread, or future. `await*` is only for
  coroutine or future-style async APIs. `poll*` checks external state or a queue
  without blocking. `tick*` drives one clock, timer, runtime, game-loop, or test
  scheduler step. `advance*` moves time, cursors, iterators, playback position,
  or state machines forward.
- State accessors use precise state words: `current*` is current context,
  `active*` is engaged/running state, `selected*` is selection model state,
  `focused*` is input/navigation focus, and `default*` is fallback or
  configured default value. Boolean predicates use `hasCurrent*()`,
  `isActive()`, `isSelected()`, `isFocused()`, and `isDefault()`.

### Events, Callbacks, And Subscriptions

- `on*` names a callback entry point or framework signal handler. `handle*`
  dispatches or orchestrates an event, command, or request into behavior.
  `notify*` notifies subscribers or services. `emit*` produces a signal,
  output, or event payload. Replace project-owned `fire*` with `emit*`.
- Use `dispatch*` only for real distribution to multiple handlers, a queue, or
  an executor.
- `subscribe*` creates an observer or callback subscription, preferably with a
  lifetime handle. `unsubscribe*` is explicit unsubscribe when RAII cannot cover
  it. `connect*` and `disconnect*` are for signal/slot, framework, or external
  connections. `register*` and `unregister*` are for long-term registries,
  catalogs, descriptors, capabilities, providers, types, or actions.
- Use `*Callback` for callable parameters or members, `*Handler` for event,
  command, or request processing behavior, and `*Observer` for state-change
  subscriptions. Do not add project-owned `*Listener` or `*Delegate`; keep those
  names only at third-party or framework boundaries.
- Use `before*` for pre-operation hooks and `after*` for post-operation hooks.
  Do not add project-owned `will*` or `did*`.
- Event/fact names use factual suffixes after the fact: `*Changed`, `*Added`,
  `*Removed`, `*Requested`, `*Completed`, and `*Failed`. Prefer
  `emitTrackChanged()`, `onTrackChanged()`, and `handleTrackChanged()` shapes.

### Collections, Matching, And Traversal

- `add*` adds membership without emphasizing position. `insert*` inserts at an
  index, iterator, or before/after location. `append*` and `prepend*` insert at
  the end or beginning. `replace*` substitutes a value or item. `swap*`
  exchanges two existing values or items. `push*` and `pop*` are only for
  stack, queue, buffer, or low-level container-like APIs.
- `move*` moves existing items while preserving identity. `reorder*` changes a
  batch order, usually from a complete order or spec. `sort*` orders by a key or
  comparator; do not use it for manual drag reordering. `rank*` computes ranks
  or priority and does not necessarily mutate order.
- `collect*` traverses sources, nodes, or items and returns a result set.
  `accumulate*` performs actual accumulation or reduction. Do not add
  `gather*`; use `collect*` or a more specific verb.
- `filter*` returns a filtered collection/view or configures a filter. A
  per-item predicate uses `matches*`, `accepts*`, `contains*`, or another
  predicate name. `compare*` is for ordering, diffing, or comparison reports;
  ordinary equality should use `operator==` or a domain-specific predicate such
  as `hasSameIdentityAs()`.
- `visit*` is for visitor pattern entry points or visiting one node/item.
  `walk*` recursively or sequentially walks a structure. Use `traverse*` and
  `iterate*` only when their algorithmic meaning is important; otherwise prefer
  `collect*`, `build*`, `find*`, `resolve*`, or `lookup*`.
- `merge*` combines same-kind data and handles conflicts, overrides, or
  priority. `combine*` forms one result from multiple inputs without conflict
  semantics. `compose*` combines behavior, functions, operations, or UI pieces.
  `join*` is for strings, paths, collections, or thread joins. `split*` divides
  one input into parts; parsing should still use `parse*`.

### UI, Library, And Playback Vocabulary

- `show*` and `hide*` only change visibility. `present*` brings an existing or
  creatable top-level window/dialog into the user's view. `reveal*` expands or
  exposes an internal UI area or state. `dismiss*` closes transient UI without
  implying resource destruction. Avoid `display*` as a generic verb.
- `focus*` is only for input or navigation focus. `activate*` and
  `deactivate*` enter or leave active state. `enable*` and `disable*` change
  capability or availability, not visibility or selection.
- `render*` creates visual representation or renderable data. `draw*` and
  `paint*` are low-level drawing callbacks or drawing-context operations.
  `layout*` is only UI measure, allocation, or child positioning; do not use it
  for domain metadata or schema. `arrange*` changes item order or placement.
  `position*` is for coordinates and placement, not generic order.
- `browse*` starts browsing UI or an external browser. `choose*` is explicit
  user choice from a dialog or options. `pick*` is internal or heuristic
  selection from candidates. `select*` mutates selection state or a selection
  model.
- `scan*` actively traverses external sources such as a filesystem or library
  source. `discover*` finds previously unknown resources, capabilities,
  devices, or entry points. `index*` builds or updates searchable indexes or
  catalogs for known items. `sync*` aligns state sources and should make
  direction clear when one-way. `watch*` continuously observes external
  changes.
- Playback actions use `play*`, `pause*`, `resume*`, `stop*`, `seek*`, and
  `scrub*` with their audio meanings. Read-only playback state uses predicates
  and accessors such as `isPlaying()` and `playbackPosition()`.

### Resource, Error, And Control Flow Vocabulary

- `cleanup*` is for test or local temporary-resource cleanup. Production APIs
  should prefer concrete verbs such as `close*`, `stop*`, `unsubscribe*`,
  `remove*`, or `delete*`. `destroy*` explicitly ends object/resource
  lifetime. `dispose*` is only for framework or binding vocabulary. `release*`
  releases ownership, handles, locks, or resources and must have a clear
  postcondition. `free*` is only for C/manual-memory boundaries.
- `commit*` makes staged, transactional, or edit-session changes official.
  `rollback*` undoes uncommitted changes within that transaction/session.
  `submit*` sends a request, command, form, or task to another layer.
  `publish*` makes state, artifacts, events, or results externally visible.
- `undo*` and `redo*` are undo-stack operations. `revert*` returns to a previous
  known or source-of-truth state. `restore*` recovers from a backup, snapshot,
  or persisted state.
- `lock*` and `unlock*` are for mutexes, resources, or explicit locked state.
  `guard*` creates or uses a guard object for scope, invariants, or reentrancy.
  `protect*` is security, permission, or protection vocabulary, not ordinary
  mutex or null-check vocabulary.
- `cancel*` is an expected cancellation of pending or ongoing work. `abort*` is
  forced or abnormal termination. `fail*` marks an operation, test, or result
  failed. `reject*` refuses a request, command, or input for validation,
  permission, or domain reasons. `skip*` deliberately skips and continues.
  `ignore*` deliberately discards; prefer `skip*`, `reject*`, or `cancel*`
  when they are more precise.
- Error and warning names are nouns or accessors such as `errorMessage()` and
  `warningCount()`, not action verbs. Result accessors use `hasError()`,
  `error()`, and `errorMessage()`, not `getError()`.

### Modifiers, Ownership, And Test Vocabulary

- `*IfNeeded` is only for idempotent guard-style actions. `*OrThrow` is
  discouraged; project-owned APIs should prefer `Result<T>`. `*Unchecked`
  means validation or precondition checks are deliberately skipped and should be
  private or internal. `*Unsafe` is only for explicit lifetime, threading, or
  security escape hatches. Avoid `*Internal` in public names; use privacy,
  `detail`, or a more specific name. `*Impl` is only for implementation hooks,
  pimpls, or backing helpers, not public/domain API.
- Do not encode ordinary ownership with `own*`, `borrow*`, or `retain*`
  functions. Use types, signatures, and member names to express ownership and
  lifetime. Keep `retain*` only at ref-counted or framework boundaries.
- Use prepositions only when they disambiguate. `*For*` names target,
  purpose, or context. `*By*` names a lookup, sort, group, or filter key.
  `*With*` names meaningful extra options or inputs. `*At*` names index, time,
  or coordinate location. `*From*` names a source. `*Into*` names a write
  target.
- Test arrangement may use `setup*`; cleanup may use `teardown*` when RAII is
  not enough. `*Fixture` owns test state or lifecycle. `Fake*` is a working
  controlled implementation with state or simplified real behavior. `Mock*` is
  for interaction expectations. `Stub*` provides fixed or minimal responses.
- Do not add `*ForTest()` functions or methods. Existing `*ForTest()` APIs are
  design debt to remove through normal APIs, constructor injection, small
  interfaces, fixtures, or test support. GTK component child widgets and
  observable UI state should use normal public accessors when that
  observability is part of the component contract; do not hide routine widget
  access behind a test peer. If a case cannot be removed cleanly, stop and
  review the design instead of documenting a permanent exception.

## Semantic Vocabulary

Durable project-owned identifiers use full semantic words by default. Use
precise names such as:

- `rowIndex`
- `byteOffset`
- `xPosition`
- `dictionaryId`
- `transaction`
- `argument`
- `parameter`
- `metadata`

Avoid durable spellings such as:

- `idx`
- `pos`
- `curr`
- `prev`
- `dest`
- `dict`
- `txn`
- `tmp`
- bare `arg`
- ambiguous `meta`
- `param`/`params`

Allowed project short forms are limited to stable, obvious vocabulary:

- `id` and `ids`
- `min` and `max`
- `lhs` and `rhs`
- `config`
- tiny-local `i`, `j`, and `it`
- scoped conversion `src` and `dst`
- argument-list `args`
- storage-handle `db`
- temp-file `temp`
- coordinate record fields `x` and `y`

Use `cancelled` for Aobus domain state, user-facing domain text, and async
control-flow names. Use `canceled` only when matching external API, framework,
or protocol spelling, such as GTK-style signal names.

## Boundary Vocabulary

External API, file-format, protocol, framework, and user-interface boundaries
may keep established vocabulary when matching that boundary is clearer or
preserves compatibility. Translate those names before they become
project-owned API.

Examples:

- LMDB `txn` and `MDB_*` names inside the LMDB wrapper.
- SPA/PipeWire `dict` and `param`.
- ALSA `params`.
- MP4 `meta` atoms.
- Keyboard `Meta`.
- Existing CLI flags or serialized keys such as `--dict`, `--meta`, and
  YAML `meta:`.

## File Names

- Production file names use the primary type, responsibility, or adapter
  boundary they contain. Prefer a concrete domain name over a generic container
  name.
- Do not add new production `Utils`, `Util`, `Utility`, or `*Types` catch-all
  files. If a header exists only to gather unrelated declarations, split it; if
  the declarations share a real concept, name that concept directly.
- Directory context may supply part of the name. A local leaf such as `File.h`
  is acceptable when the parent directory is already the clear domain, such as a
  specific tag format directory.
- Tooling and adapter files may keep names that match external APIs,
  clang-tidy check ids, file formats, protocols, or user-visible compatibility
  surfaces.

## Helper And Support Names

- Use `*Helpers` only for internal/detail/tooling free-function collections tied
  to a clear owner or domain.
- Do not add domain test helper files named `*Utils`, `*Util`, or `*Utility`.
- In tests, `*Fixture` owns test state or lifecycle.
- In tests, `*TestSupport` groups reusable domain test scaffolding.
- The root `test/unit/TestUtils.h` header is reserved for low-level shared test
  utilities.

## Review Practice

- Prefer renaming unclear project-owned names in code over documenting narrow
  exceptions.
- Document a boundary principle once when a repeated external vocabulary could
  confuse future reviewers.
- Leave vocabulary audit tooling report-only until the code backlog is small and
  the policy is settled.

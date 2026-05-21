# yaml-cpp to rapidyaml Migration Implementation Plan

## Purpose

Replace `yaml-cpp` with `rapidyaml` (`ryml`) without preserving the allocation
pattern that made YAML parsing and emitting expensive. The migration should not
be a mechanical `YAML::Node` to `NodeRef` rewrite: the main goal is to avoid
per-field owning `std::string` conversions in YAML-heavy paths while preserving
the current YAML file formats for configuration, library import/export, and GTK
layout documents.

Design background: `doc/design/yaml-cpp-to-ryml-migration.md`.

## Success Criteria

- No production target links `yaml-cpp` or includes `<yaml-cpp/yaml.h>`.
- YAML reads use ryml scalar views (`c4::csubstr`/`std::string_view`) at the YAML
  boundary instead of `YAML::Node::as<std::string>()`-style owning conversions.
- Long-lived YAML trees store their own scalar content in ryml's tree arena, not
  in scattered `std::string` objects.
- Library export still emits cover-art anchors and aliases, and import still
  accepts alias-backed cover-art scalars.
- Existing config, layout, and library import/export tests pass with focused
  coverage for parse errors, empty/missing files, aliases, and save-before-flush
  lifetime cases.

## Non-Goals

- Do not redesign the YAML schemas.
- Do not remove necessary ownership at real domain boundaries. Loading a file
  still needs one owning byte buffer, and domain objects that own strings should
  continue to own them.
- Do not rewrite unrelated serialization formats or library storage code.
- Do not introduce a large generic serialization framework. Keep helpers small
  and directly motivated by current `ConfigStore`, `LibraryImporter`,
  `LibraryExporter`, and `LayoutDocument` needs.

## Current Verified State

- `yaml-cpp` is discovered in `cmake/Dependencies.cmake`, added by `shell.nix`,
  and linked by `app_runtime` plus test targets.
- Production yaml-cpp usage is concentrated in:
  - `app/runtime/ConfigStore.h`
  - `app/runtime/ConfigStore.cpp`
  - `app/runtime/ConfigYamlTraits.h`
  - `app/runtime/LibraryImporter.cpp`
  - `app/runtime/LibraryExporter.cpp`
  - `app/linux-gtk/layout/document/LayoutYaml.h`
  - `app/linux-gtk/layout/document/LayoutDocument.cpp`
- Tests also parse YAML directly in runtime and GTK layout tests.
- `LibraryExporter` currently emits cover-art anchors/aliases using
  `YAML::Anchor` and `YAML::Alias`; tests assert the emitted text contains
  `&cover_` and `*cover_`.
- The Nix rapidyaml package is available as `rapidyaml` and installs CMake
  package `ryml` with imported target `ryml::ryml`.
- In rapidyaml 0.11.1 the in-place parser API is named `parse_in_place()`, not
  `parse_in_situ()`.

## Ownership and Allocation Rules

### Allowed Owning Storage

Use owning storage only where it represents real ownership or a required output:

- One file buffer per parsed YAML file, preferably `std::vector<char>`.
- ryml tree arena copies for data that must survive beyond the source object or
  buffer.
- Existing domain-owned `std::string` fields after deserialization.
- Generated values that are inherently new data, such as base64 cover payloads,
  formatted UUIDs, paths, and anchor names.
- Test-only strings used to build fixtures or inspect output text.

### Disallowed Migration Pattern

Avoid this pattern in production YAML traversal:

```cpp
auto value = node.as<std::string>();
use(std::string_view{value});
```

The ryml replacement should prefer:

```cpp
auto value = scalarView(node); // string_view into file buffer or tree arena
use(value);
```

### Tree Lifetime Rules

- Parsed tree values point into the mutable parse buffer when using
  `ryml::parse_in_place()`. The buffer must outlive every `NodeRef`,
  `ConstNodeRef`, `csubstr`, and `string_view` derived from that tree.
- Trees built for delayed emission must copy non-stable scalar views into the
  ryml arena with an explicit arena-copy step.
- Trees built and emitted inside one transaction may reference stable LMDB or
  dictionary views only until emission finishes. If a view cannot be proven to
  outlive emission, copy it into the ryml arena.
- `ConfigStore` is long-lived and may flush after callers' source objects are
  destroyed. All saved scalar values must therefore be owned by the tree arena.
- Alias resolution may copy referenced scalar values into the tree. After import
  parsing, resolve references before normal validation/traversal unless a test
  proves the unresolved form is handled explicitly.

## ryml API Conventions

- Include `<ryml.hpp>` and targeted ryml headers only where needed.
- Use `ryml::parse_in_place(filename, substr, &tree)` for mutable file buffers.
- Use `ryml::parse_in_arena(filename, csubstr, &tree)` only for immutable input
  that cannot be modified, such as GResource data. This copies into the ryml
  arena but avoids constructing a temporary `std::string`.
- Do not use `parse_in_situ`; it is not present in the currently packaged ryml.
- Prefer local callbacks on `ryml::Parser`/`ryml::Tree` over global
  `ryml::set_callbacks()`. The error callbacks must not return; they should
  throw an Aobus exception type or `std::runtime_error` with filename/location
  context.
- Emit with either `std::ofstream << tree` or `emitrs_yaml<std::string>(tree)`
  followed by one write. Do not claim `emitrs_yaml` writes directly to streams.

## Phase 0: Baseline and Scope Lock

### Work

1. Confirm current direct yaml-cpp references:

   ```bash
   rg -n "yaml-cpp|YAML::|yaml_cpp|YAML_CPP" app test cmake shell.nix
   ```

2. Record the current focused test set and any pre-existing failures.
3. Treat the YAML schemas as compatibility contracts. Do not make formatting-only
   changes that affect tests unless ryml emission requires them and the parser
   compatibility is preserved.

### Verification

Prefer focused tests during iteration, then run the full debug build at the end:

```bash
./build.sh debug
```

## Phase 1: Build System Migration

### Work

1. In `shell.nix`, remove `yaml-cpp` and add `rapidyaml`.
2. In `cmake/Dependencies.cmake`:
   - Replace `find_package(yaml-cpp REQUIRED)` with `find_package(ryml REQUIRED)`.
   - Add a small wrapper target only if useful for project consistency:

     ```cmake
     add_library(PkgRapidYaml INTERFACE)
     target_link_libraries(PkgRapidYaml INTERFACE ryml::ryml)
     ```

3. Link `PkgRapidYaml`/`ryml::ryml` with the correct visibility:
   - `app_runtime`: `PUBLIC` if `ConfigStore.h` or exported runtime headers
     include ryml headers; otherwise `PRIVATE`.
   - `aobus-gtk-lib`: `PUBLIC` if `LayoutYaml.h` remains public and includes
     ryml headers; otherwise `PRIVATE`.
   - Test targets should not link yaml-cpp.
4. Remove the clang-tidy include-cleaner yaml-cpp exception if no longer needed.

### Verification

Configure only after the code compiles far enough for dependency discovery:

```bash
nix-shell --run "cmake --preset linux-debug -B /tmp/build/debug"
```

## Phase 2: Minimal ryml Support Layer

### Intent

Keep ryml-specific error handling and scalar lifetime rules consistent without
creating a broad framework.

### Work

Add a small internal YAML support area near the current runtime YAML code if the
first migrated caller would otherwise duplicate this logic. Keep it focused on:

- File read into `std::vector<char>`.
- Conversion from byte buffer to `c4::substr`.
- Parser/tree construction with local throwing callbacks.
- `scalarView(ConstNodeRef)` returning `std::string_view` without allocation.
- Numeric/bool parsing helpers that use ryml operators or `c4::from_chars`
  rather than constructing strings.
- Arena-copy helpers with names that make ownership explicit, for example
  `copyScalarToArena(tree, view)` or direct documented use of `tree.to_arena()`.

### Constraints

- No helper should return a view to a temporary formatted string.
- Do not hide ownership in a generic `toString()` helper.
- Keep thrown error messages compatible with existing `Result`/`Exception`
  handling where callers currently catch yaml-cpp exceptions.

### Verification

Add or update focused tests only when a helper has behavior beyond trivial API
wrapping, especially parse-error conversion and scalar lifetime behavior.

## Phase 3: ConfigStore Migration

### Intent

`ConfigStore` owns a long-lived YAML tree. It must avoid scattered
`std::string` allocations but cannot store views to caller-owned objects.

### Work

1. Replace `YAML::Node _root` with:
   - `ryml::Tree _root` or equivalent.
   - `std::vector<char> _inputBuffer` for parsed file contents.
   - Existing `_loaded` and path/mode state.
2. Update `ensureLoaded()`:
   - Missing read-write file keeps an empty map root.
   - Missing read-only file returns `Error::Code::NotFound` as today.
   - Existing file is read into `_inputBuffer` and parsed with
     `parse_in_place()`.
   - Empty file should behave like an empty map, matching current practical
     config behavior.
3. Update `flush()`:
   - Ensure parent directories exist.
   - Emit `_root` to the config file.
   - Preserve `IoError` reporting for write failures.
4. Rewrite `ConfigStore::save<T>()` in the header:
   - Keep read-only logic error behavior.
   - Ensure loaded.
   - Ensure root is a map.
   - Create or replace the group child.
   - Serialize `obj` into the child, copying all scalar content needed after
     `save()` into the ryml tree arena.
5. Rewrite `ConfigStore::load<T>()` in the header:
   - Ensure loaded.
   - If the group is absent, leave `obj` unchanged and return success as today.
   - Decode from a `ConstNodeRef` with no owning string conversion unless the
     destination type itself owns a string.
   - Convert decode exceptions to `Error::Code::FormatRejected` with the group
     name in the message.
6. Replace `ConfigYamlTraits.h`:
   - Filesystem paths: read scalar view into `std::filesystem::path`; write the
     path string representation into the tree arena.
   - `std::optional<T>`: null/absent clears; present delegates to `T`.
   - Enums: store as integers, matching current tests.
   - PFR aggregates: map fields by `boost::pfr::names_as_array<T>()`.
   - Strong types with `raw()`: delegate to the raw value type.
   - Containers used by current config tests: `std::vector<T>` as sequences and
     `std::map<std::string, T, std::less<>>` as maps.

### Extra Tests

- Save an object containing strings, mutate/destroy the original object, then
  flush and reload. This proves `save()` copied into the ryml arena rather than
  retaining caller-owned views.
- Load invalid YAML and verify `ConfigStore::load()` returns an error instead
  of aborting.
- Keep existing `[app][runtime][config]` round-trip tests.

### Focused Verification

```bash
nix-shell --run "/tmp/build/debug/test/ao_test --rng-seed time '[app][runtime][config]'"
```

## Phase 4: Library Importer Migration

### Intent

Import should parse once, traverse by view, and remove the current
`std::deque<std::string>` keep-alive workaround.

### Work

1. In `LibraryImporter::Impl::importFromYaml()`:
   - Read the YAML file into one mutable buffer.
   - Parse with `parse_in_place()` using local throwing callbacks.
   - Resolve aliases with `tree.resolve()` before validation/traversal unless
     explicit unresolved-alias handling is implemented.
   - Keep the buffer and tree alive until all validation, import, builder
     preparation, and transaction work that consumes scalar views is complete.
2. Replace validation signatures from `YAML::Node const&` to ryml node refs or
   node ids.
3. Store validated track/list node references only while the owning tree is
   alive. Prefer node ids if this avoids accidental dangling references in
   containers.
4. Replace scalar reads:
   - Required/optional string fields become `std::string_view` where downstream
     APIs accept views.
   - Numeric fields parse directly from scalars.
   - Base64 cover values pass a view to the decoder; allocate only for decoded
     binary data as required by the resource store.
5. Delete the importer-local `std::deque<std::string>` keep-alive storage once
   metadata, tags, and custom fields are all passed as views from the parse
   buffer/tree.
6. Preserve import error messages where tests assert user-facing behavior.

### Anchor/Alias Requirements

- Add or preserve a fixture where `coverArtBase64` is an alias to an anchored
  scalar.
- Verify import stores the same cover resource content as the plain scalar path.
- If `tree.resolve()` clears anchors, this is fine for import; export tests cover
  emission separately.

### Focused Verification

```bash
nix-shell --run "/tmp/build/debug/test/ao_test --rng-seed time '[app][core][yaml]'"
```

## Phase 5: Library Exporter Migration

### Intent

Export should build or stream YAML without per-field `std::string` conversions,
while preserving anchors/aliases for duplicate cover payloads.

### Work

1. Replace `YAML::Emitter` with ryml tree construction.
2. Use static string literals or stable `string_view` keys for YAML field names.
3. For values from stable database/dictionary views, either:
   - Reference them directly until immediate emission completes within the same
     transaction, or
   - Copy to the tree arena when lifetime is not obvious.
4. For generated values:
   - Prefer numeric node assignment for integers/floats.
   - Format UUIDs and paths only once per field, then copy to the tree arena.
   - Keep base64 as an allowed generated string unless `base64Encode()` is later
     extended to write directly into an arena-backed buffer.
5. Preserve cover-art deduplication:
   - First occurrence: set the scalar value and `set_val_anchor("cover_N")`.
   - Later occurrences: create a value ref with `set_val_ref("cover_N")`.
   - Keep the exporter-local `ResourceId -> anchor name` map, but avoid heap
     strings for anchor names where practical by formatting into a small local
     buffer and copying into the tree arena.
6. Emit before the transaction and any referenced views are invalidated.

### Extra Tests

- Preserve the textual anchor/alias test checking `&cover_` and `*cover_`.
- Parse the emitted YAML with ryml and verify list-only/delta export structure
  previously checked with yaml-cpp.
- Round-trip duplicate cover art through export/import.

### Focused Verification

```bash
nix-shell --run "/tmp/build/debug/test/ao_test --rng-seed time '[app][core][yaml]'"
```

## Phase 6: GTK Layout YAML Migration

### Intent

Layout serialization should share the same no-owning-conversion discipline while
keeping saved layout compatibility.

### Work

1. Replace `LayoutYaml.h` yaml-cpp specializations with ryml read/write
   overloads for:
   - `LayoutValue`
   - `LayoutNode`
   - `LayoutDocument`
   - `std::map<std::string, LayoutValue, std::less<>>`
   - `std::map<std::string, LayoutNode, std::less<>>`
2. In `LayoutDocument.cpp`, load the built-in GResource YAML without copying it
   into `std::string`:
   - Build a `c4::csubstr` directly from the GResource bytes.
   - Parse with `parse_in_arena()` because the GResource bytes are immutable.
   - Decode immediately into `LayoutDocument`.
3. Keep `loadLayout()` and `saveLayout()` using `ConfigStore`, so layout config
   persistence benefits from the ConfigStore arena ownership rules.
4. Preserve variant type behavior for bool, number, string, arrays/maps, and
   null values as currently tested.

### Focused Verification

```bash
nix-shell --run "/tmp/build/debug/test/ao_test_gtk --rng-seed time '[layout]'"
```

If GTK tests are built into a different binary in the active build tree, use the
existing debug build's GTK/layout test command and record it in the final change
notes.

## Phase 7: Test and Fixture Migration

### Work

1. Replace test-only `<yaml-cpp/yaml.h>` includes with ryml helpers or direct
   text assertions.
2. For tests that only check YAML shape, prefer parsing with ryml and checking
   nodes by key/index.
3. For tests that only check emission of anchors/aliases, keep simple textual
   assertions because the textual YAML feature is the behavior under test.
4. Update any test tags in local run instructions. Relevant current tags are:
   - `[app][runtime][config]`
   - `[app][core][yaml]`
   - `[layout]`
5. Add regression coverage for:
   - ConfigStore save-before-flush string lifetime.
   - Import of alias-backed `coverArtBase64`.
   - Parse-error conversion to Aobus error/exception paths.
   - Empty config file behavior.

### Verification

Run focused tests first, then the full build:

```bash
nix-shell --run "/tmp/build/debug/test/ao_test --rng-seed time '[app][runtime][config]'"
nix-shell --run "/tmp/build/debug/test/ao_test --rng-seed time '[app][core][yaml]'"
nix-shell --run "/tmp/build/debug/test/ao_test_gtk --rng-seed time '[layout]'"
./build.sh debug
```

## Phase 8: Cleanup and Guardrails

### Work

1. Remove all remaining yaml-cpp includes and link references.
2. Remove any compatibility comments that describe yaml-cpp behavior as current
   behavior.
3. Search for accidental owning conversions introduced during the migration:

   ```bash
   rg -n "as<std::string>|std::string\{|std::string\(" app/runtime app/linux-gtk/layout
   rg -n "yaml-cpp|YAML::|yaml_cpp|YAML_CPP" app test cmake shell.nix script
   ```

   Review matches manually; generated strings and domain ownership are allowed,
   YAML traversal conversions are not.
4. Run clang-tidy on changed C++ files if the implementation changes production
   C++:

   ```bash
   ./script/run-clang-tidy.sh
   ```

## Implementation Order Recommendation

1. Add ryml dependency and minimal support helpers.
2. Migrate `ConfigStore` and its tests first. This proves long-lived tree arena
   ownership and generic traits.
3. Migrate `LayoutDocument` next because it exercises config traits and immutable
   GResource parsing.
4. Migrate `LibraryImporter` next because it benefits most from scalar views and
   removes keep-alive strings.
5. Migrate `LibraryExporter` last because anchors/aliases and generated values
   are the highest-risk emission behavior.
6. Remove yaml-cpp from the build only after all production and test includes are
   gone, unless doing so earlier is needed to catch accidental dependencies.

## Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| Dangling scalar views after parse buffer destruction | Keep buffers at least as long as tree traversal/import; add ConfigStore save-before-flush lifetime test. |
| Saved config tree points to caller-owned strings | Always copy `save()` scalars into the ryml arena. |
| ryml parse callback aborts process | Use local throwing callbacks on parser/tree construction. |
| Anchor/alias behavior differs from yaml-cpp | Explicitly use `set_val_anchor`/`set_val_ref` on export and `tree.resolve()` on import; keep cover-art tests. |
| Test commands use stale tags | Use actual tags `[app][runtime][config]`, `[app][core][yaml]`, and `[layout]`. |
| Mechanical rewrite preserves std::string churn | Review YAML traversal for owning conversions and prefer scalar views. |

## Definition of Done

- `rg -n "yaml-cpp|YAML::|yaml_cpp|YAML_CPP" app test cmake shell.nix script`
  has no production dependency matches, except historical documentation if left
  intentionally.
- Focused config, library YAML, and layout tests pass.
- `./build.sh debug` passes.
- The final diff shows YAML traversal using views/arena ownership rather than
  repeated temporary `std::string` conversions.

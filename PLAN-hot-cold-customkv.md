# Hot/Cold + Custom KV Implementation Plan

## Scope And Constraints

This plan follows three explicit constraints:

1. Do **not** introduce a dedicated dictionary for custom keys.
2. During filtering, evaluation is centered on a `TrackId`; each compiled expression decides whether to load `hot`, `cold`, or `both`.
3. Custom KV values are stored as plain strings. No typed storage layer is introduced. Any type behavior is handled in expression runtime conversion.

## Goals

1. Keep common filter/search fast by scanning compact hot records first.
2. Preserve extensibility by placing long-tail metadata in cold custom KV.
3. Avoid schema churn for custom fields while keeping expression semantics predictable.
4. keep implementation incremental.

## Non-Goals

1. No custom-key dictionary or typed custom-value schema.
2. No broad query-language redesign beyond what is needed for hot/cold routing and runtime conversion.

## Data Model

### Hot Store (`tracks_hot`)

Hot store holds all high-frequency filter/search fields.

**Hot fields (from existing TrackHeader)**:

| Field | Type | Notes |
|-------|------|-------|
| tagBloom | uint32_t | Bloom filter for tags |
| durationMs | uint32_t | Track duration in milliseconds |
| bitrate | uint32_t | Bitrate in bps |
| sampleRate | uint32_t | Sample rate in Hz |
| artistId | DictionaryId | Artist dictionary ID |
| albumId | DictionaryId | Album dictionary ID |
| genreId | DictionaryId | Genre dictionary ID |
| albumArtistId | DictionaryId | Album artist dictionary ID |
| year | uint16_t | Release year |
| title | string | Track title (variable length) |
| codecId | uint16_t | Audio codec identifier |
| channels | uint8_t | Number of audio channels |
| bitDepth | uint8_t | Bits per sample |
| rating | uint8_t | User rating (0-5) |
| tagCount | uint8_t | Number of tags |
| (tag IDs) | variable | Tag IDs payload |

Notes:
- `fileSize`, `mtime` moved to cold store (display/refresh only)
- `coverArtId`, `trackNumber`, `totalTracks`, `discNumber`, `totalDiscs`, `uri` moved to cold store
- `title` remains in hot for fast search/filter

### Cold Store (`tracks_cold`)

Cold store is keyed by `TrackId` and contains display-only metadata and custom KV payload.

**Cold fixed fields** (TrackColdHeader, 40 bytes):

| Field | Type | Description |
|-------|------|-------------|
| fileSize | uint64_t | File size in bytes |
| mtime | uint64_t | Last modification time (unix timestamp) |
| coverArtId | uint32_t | ResourceStore ID for cover art |
| trackNumber | uint16_t | Track number |
| totalTracks | uint16_t | Total tracks in album |
| discNumber | uint16_t | Disc number |
| totalDiscs | uint16_t | Total discs in album |
| uriOffset | uint16_t | Offset to URI string |
| uriLen | uint16_t | Length of URI string |
| reserved | uint8_t | Padding for alignment |

**Variable payload** (after header):

1. `uint16_t customPairCount`
2. Repeated entries: `uint16_t keyLen`, `uint16_t valueLen`, then raw bytes for key/value (UTF-8)
3. `char[uriLen]` URI string (null-terminated)

Notes:

1. Custom keys are stored as-is (no dictionary deduplication per Constraint #1).
2. Custom values are stored as plain strings.
3. Keys are stored as-is (case preserved) for now. No normalization on write.

## Storage Layer Changes


## Track Store API

Refactor `TrackStore` into a dual-store abstraction while preserving `TrackId` keying.

Suggested API additions:

1. `Reader::hot(TrackId)` and `Reader::cold(TrackId)` - returns hot/cold view directly
2. `Reader::hot()` and `Reader::cold()` - return iterators for each store 

Write path:

1. `create(hotData, coldData)` appends to hot first, writes cold with same `TrackId`.
2. `update(id, hotData?, coldData?)` supports partial updates.
3. `del(id)` deletes both records in one write transaction.

## Record Materialization

Split serialization in `TrackRecord`:

1. `serializeHot()`.
2. `serializeCold()`.

`TrackRecord` should include a simple custom KV container:

1. `std::vector<std::pair<std::string, std::string>> customMeta;`

No type field is stored.

## Expression Engine Changes

## Syntax

Introduce custom-key variable prefix `%`:

1. `%replaygain_track_gain_db < -6`
2. `%edition ~ "remaster"`
3. `%isrc = "USSM19999999"`

Parser changes:

1. Add `VariableType::Custom`.
2. Extend `varType` symbols with `%`.

## Access Planning (Hot/Cold Routing)

During compile, compute `AccessProfile` for each plan:

1. `HotOnly`: expression references only hot-accessible fields.
2. `ColdOnly`: expression references only cold/custom fields.
3. `HotAndCold`: references both sets.

This profile becomes part of `ExecutionPlan`.

## Evaluator Contract

Evaluator should operate on `TrackId` + lazy loaders, not only a preloaded full record.

Suggested call shape:

```cpp
bool matches(ExecutionPlan const& plan,
             core::TrackId id,
             Loaders const& loaders) const;
```

Where `Loaders` exposes:

1. `hot(id)` -> hot view.
2. `cold(id)` -> cold view.

Load strategy by profile:

1. `HotOnly`: load hot once, never touch cold.
2. `ColdOnly`: load cold once (hot only if mandatory for framework checks).
3. `HotAndCold`: load hot first; load cold lazily only when instruction stream first touches cold/custom field.

## Runtime Type Conversion For Custom KV

**⚠️ WARNING: Lexicographic Comparison Pitfall ⚠️**

All custom KV values are treated as **strings only**. No runtime type conversion is performed.

### String Comparison Semantics

1. For `~` (LIKE): string contains match.
2. For `=` and `!=`: string equality comparison.
3. For `<`, `<=`, `>`, `>=`: **lexicographic** (dictionary order) comparison.

### Important Behavioral Caveat

Numeric values stored as strings compare lexicographically, not numerically:

- `%replaygain_track_gain_db < -6` will compare as strings, NOT as numbers
- `"10" < "6"` is `true` (because `"1" < "6"` in ASCII)
- This may surprise users expecting numeric semantics

Future work may add runtime type detection and numeric comparison for custom KV, but not in this phase.

## Query Execution Flow

In filter loop (`track show`):

1. Iterate hot DB track IDs.
2. For each `TrackId`, call evaluator with plan + lazy loaders.
3. Evaluator uses plan profile to load hot/cold as needed.
4. Build result list from matched IDs.
5. Materialize additional cold fields only for output formatting if needed.

This keeps the hot scan path compact while preserving custom expressiveness.

## Import / Update Path

When reading tags:

1. Continue reading known fields to hot/cold fixed slots.
2. Collect unknown/custom fields from tag parsers.
3. Normalize custom keys.
4. Convert custom values to strings at ingestion time.
5. Persist them into cold custom KV payload.

No custom-key dictionary lookup is performed.

## Testing Plan

## Core Storage Tests

1. Create/read/update/delete for hot+cold pair with same `TrackId`.
2. Partial update tests (`hot only`, `cold only`).
3. Deletion consistency (both DBs removed).

## Expression Tests

1. Parser: `%custom_key` variables and mixed expressions.
2. Compiler: correct `AccessProfile` generation.
3. Evaluator: load routing behavior (`HotOnly`, `ColdOnly`, `HotAndCold`).
4. Runtime conversion tests:
   1. numeric compare success/failure.
   2. string fallback behavior.
   3. LIKE behavior.

## End-to-End Tests

1. `track show --filter` for hot-only query should not load cold.
2. mixed hot/custom query loads cold lazily.
3. custom-only query works on string-stored numeric values.

## Implementation Phases

1. Phase 1: Add DB plumbing (`tracks_hot`, `tracks_cold`) and `TrackStore` dual read/write primitives.
2. Phase 2: Split `TrackRecord` serialization into hot/cold, include custom KV string payload in cold.
3. Phase 3: Wire importer and track creation paths to populate both stores.
4. Phase 4: Extend parser/AST/compiler for `%` and `AccessProfile`.
5. Phase 5: Refactor evaluator to `TrackId + lazy loaders` and implement runtime string conversion logic.
6. Phase 6: Add regression tests and finalize.
7. Phase 7 (Future): Secondary index support for custom KV search optimization.

## Risks And Mitigations

1. ⚠️ **Known Limitation: Lexicographic comparison** - Numeric custom KV values (e.g., `%replaygain_track_gain_db < -6`) compare as strings. Document this clearly in user-facing documentation.
2. Risk: Cold loads accidentally happen in hot-only queries. Mitigation: explicit `AccessProfile` tests and loader-call counters in tests.
3. Risk: Parser complexity growth. Mitigation: minimal grammar extension (`%` only) and preserve existing operators.
4. Risk: Hot/cold write non-atomicity if not properly sharing WriteTransaction. Mitigation: Both stores must open within the same transaction context (see Commit 3).

## Acceptance Criteria

1. Hot-only filters run without cold DB reads.
2. Custom KV filters work with string storage and string comparison.
3. No custom-key dictionary exists in schema or code path.
4. Existing `$`, `@`, `#` expressions keep behavior compatibility.
5. Create/update/delete operations are atomic across hot and cold stores.
6. Custom KV lexicographic comparison behavior is documented.

## Commit-By-Commit Execution Checklist

This section gives a concrete implementation order so the work can be landed incrementally with low risk.

### Commit 1: Introduce Hot/Cold DB Plumbing

Files:

1. `include/rs/core/MusicLibrary.h`
2. `src/core/MusicLibrary.cpp`

Changes:

1. Add DB handles for `tracks_hot` and `tracks_cold`.
2. Raise LMDB `maxDatabases` from 4 to 6 (tracks_hot, tracks_cold, lists, resources, dictionary + headroom).
3. Remove legacy `tracks` DB. This is a clean-start design: no migration path, existing data is not preserved.

**Note:** LMDB supports atomic multi-DBI operations within a single WriteTransaction. Both hot and cold DBs must be opened within the same transaction context for atomic create/update/delete.

Validation:

1. Existing startup/tests still pass with no behavior change (for new/empty DBs).
2. Two new DBs are created on disk.

### Commit 2: Add Cold Layout + Custom KV Codec

Files:

1. `include/rs/core/TrackColdLayout.h` (new)
2. `src/core/TrackColdLayout.cpp` (new)
3. `test/core/TrackColdLayoutTest.cpp` (new)

Changes:

1. Define binary format for cold fixed fields and custom key/value pairs.
2. Add encode/decode helpers for `vector<pair<string,string>>`.
3. Add key normalization helper (`lowercase` + trim).

Validation:

1. Round-trip tests for empty, single, and multi KV payload.
2. Normalization behavior tests.

### Commit 3: Refactor TrackStore To Dual-Store API

Files:

1. `include/rs/core/TrackStore.h`
2. `src/core/TrackStore.cpp`
3. `test/core/TrackStoreTest.cpp`

Changes:

1. Add `Reader::hot(TrackId)` and `Reader::cold(TrackId)` for direct view access.
2. Add `Reader::hot()` and `Reader::cold()` for iterator access to each store.
3. Add `Writer::create(hot, cold)`, `updateHot`, `updateCold`, and synchronized `del`.
4. **Critical:** Ensure both stores share the same WriteTransaction context so that create/update/delete are atomic across hot and cold.

Validation:

1. CRUD tests cover both DBs and same `TrackId` integrity.
2. Delete removes both records atomically.

### Commit 4: Split TrackRecord Serialization

Files:

1. `include/rs/core/TrackRecord.h`
2. `src/core/TrackRecord.cpp`
3. `test/core/TrackRecordTest.cpp`

Changes:

1. Add `customMeta: vector<pair<string,string>>` field.
2. Implement `serializeHot()` and `serializeCold()`.
3. Replace legacy `serialize()` with hot/cold split.

Validation:

1. Existing known fields preserved in hot.
2. Custom KV round-trips through cold serializer.

### Commit 5: Wire Import/Create Flows To Hot+Cold

Files:

1. `tool/InitCommand.cpp`
2. `tool/TrackCommand.cpp`
3. `src/tag/flac/File.cpp` (only if normalization entry point centralized here)
4. `src/tag/mp4/File.cpp` (same note)

Changes:

1. Populate `TrackRecord.customMeta` from parser custom fields.
2. Normalize keys before persistence.
3. Write both hot and cold payloads on create/update.

Validation:

1. CLI create/import still works.
2. Custom fields are present in cold payload after import.

### Commit 6: Extend Expr AST/Parser For Custom Variables

Files:

1. `include/rs/expr/Expression.h`
2. `src/expr/Parser.cpp`
3. `test/query/ParserTest.cpp`

Changes:

1. Add `VariableType::Custom`.
2. Add `%` variable prefix parsing.
3. Keep existing `$`, `@`, `#` grammar unchanged.

Validation:

1. Parser tests for `%foo`, mixed expressions, and precedence stability.

### Commit 7: Add AccessProfile To ExecutionPlan

Files:

1. `include/rs/expr/ExecutionPlan.h`
2. `src/expr/ExecutionPlan.cpp`
3. `test/query/ExecutionPlanTest.cpp`

Changes:

1. Add `enum class AccessProfile { HotOnly, ColdOnly, HotAndCold };`.
2. Compiler marks plan access profile based on referenced variables.
3. Preserve existing opcodes where possible; add custom field handling metadata.

Validation:

1. Compile tests assert correct profile for hot-only/custom-only/mixed queries.

### Commit 8: Evaluator Refactor To TrackId + Lazy Loaders

Files:

1. `include/rs/expr/PlanEvaluator.h`
2. `src/expr/PlanEvaluator.cpp`
3. `test/query/PlanEvaluatorTest.cpp`

Changes:

1. Evaluator API accepts `TrackId` and loader callbacks/adaptor.
2. Implement lazy load behavior driven by `AccessProfile`.
3. Add custom KV lookup path from cold payload.

Validation:

1. Loader-call-count tests verify no cold load for `HotOnly` plans.
2. Mixed plan triggers cold load only on-demand.

### Commit 9: Runtime String Comparison Semantics

Files:

1. `src/expr/PlanEvaluator.cpp`
2. `test/query/PlanEvaluatorTest.cpp`

Changes:

1. Implement string comparison for all custom KV operators (`=`, `!=`, `<`, `<=`, `>`, `>=`, `~`).
2. No runtime type conversion - all values are strings.

Validation:

1. String comparison tests for all operators.
2. Edge-case tests for empty strings and special characters.

### Commit 10: Integrate Query Pipeline In `track show`

Files:

1. `tool/TrackCommand.cpp`

Changes:

1. Filter loop runs by `TrackId` + plan + lazy loaders.
2. Output formatting loads only what is needed.
3. Maintain same user-facing CLI behavior.

Validation:

1. Existing filter scenarios still return equivalent results.
2. Hot-only filters show reduced cold reads (instrumentation/log assertion in tests where feasible).


### Commit 11: Cleanup And Finalization

Files:

1. all touched modules

Changes:

1. Finalize comments/docs for new `%` semantics and lexicographic comparison behavior.
2. Ensure style and test coverage are consistent.

Validation:

1. Full project build.
2. Query and core test suite pass.

## Progress Tracking Template

Use this checklist in PR description to keep rollout visible:

1. [ ] Commit 1 done
2. [ ] Commit 2 done
3. [ ] Commit 3 done
4. [ ] Commit 4 done
5. [ ] Commit 5 done
6. [ ] Commit 6 done
7. [ ] Commit 7 done
8. [ ] Commit 8 done
9. [ ] Commit 9 done
10. [ ] Commit 10 done
11. [ ] Commit 11 done

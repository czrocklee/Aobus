# Error-Handling Model Rollout Plan

## Context

The [error-handling design doc](../design/error-handling.md) defines a three-mechanism model:
`ao::Result<T>` for recoverable failures, `std::optional<T>` for legitimate absence,
and `ao::Exception` for invariant violations. The doc was just finalized; the codebase
has accumulated patterns that predate the model. This plan sequences the fixes by
priority: infrastructure first, then API contracts, then internal cleanup, then async
boundary.

---

## Phase 0: Infrastructure (blocking for all later phases)

### 0a. Add `Error::Code` to `ao::Exception`

**File:** `include/ao/Exception.h`

Add a 4-arg constructor `Exception(Error::Code, std::string what, char const* file, std::int32_t line)`.
Store `Error::Code _code` as a private member. Add `Error::Code code() const noexcept` accessor.

Keep the existing 3-arg constructor for backward compat during migration; it defaults `_code`
to `Error::Code::Generic`.

### 0b. Add `InvalidState` to `Error::Code`

**File:** `include/ao/Error.h` (enum at line 12)

Add `InvalidState` after `NotSupported`. This is the code for contract/invariant violations.

### 0c. Add `Error::Code` overloads to `throwException()`

**File:** `include/ao/Exception.h`

Add two new overloads that take `Error::Code` as the first parameter:
- Format-string variant (requires `sizeof...(Args) > 0`)
- Simple string_view variant

Old overloads (without `Error::Code`) remain for transitional use.

### 0d. Update `main.cpp` catch blocks

**Files:**
- `app/linux-gtk/main.cpp` (line 292)
- `app/cli/main.cpp` (line 40)

Replace single `catch (std::exception const&)` with the two-block pattern:
```cpp
catch (ao::Exception const& e)  { /* our bug, log with code */ }
catch (std::exception const& e) { /* third-party escape, log */ }
```

### Verification Phase 0

- [ ] Build succeeds
- [ ] Existing `throwException` calls (without Code) still compile
- [ ] `ao::Exception::code()` accessor works
- [ ] Unit test for new constructor

---

## Phase 1: Fix `std::logic_error` throws

### 1a. ConfigStore

**Files:**
- `app/runtime/ConfigStore.h:45` — `save()` template
- `app/runtime/ConfigStore.cpp:27` — `flush()`

Change `throw std::logic_error{"..."}` to:
```cpp
ao::throwException<ao::Exception>(Error::Code::InvalidState, "...");
```

### 1b. Update ConfigStore tests

**File:** `test/unit/runtime/ConfigStoreTest.cpp` (line ~571)

Change `CHECK_THROWS_AS(..., std::logic_error)` to `CHECK_THROWS_AS(..., ao::Exception)`.

### Verification Phase 1

- [ ] Build succeeds
- [ ] ConfigStore tests pass
- [ ] Zero `std::logic_error` throws from Aobus code

---

## Phase 2: Public APIs — throw → `Result<T>`

### 2a. `LibraryExporter::exportToYaml()` → `Result<>`

**Files:**
- `app/runtime/LibraryExporter.h:45` — change signature from `void` to `Result<>`
- `app/runtime/LibraryExporter.cpp:393, 401` — change `throwException` to `makeError`

### 2b. `LibraryImporter::importFromYaml()` → `Result<>`

**Files:**
- `app/runtime/LibraryImporter.h:41` — change signature from `void` to `Result<>`
- `app/runtime/LibraryImporter.cpp` — convert all `throwException` calls (~12 locations:
  lines 210, 258, 265, 286, 316, 323, 334, 357, 364, 369, 378) to `makeError`

Internal helpers `validate()`, `validateTracks()`, `validateLists()` must also change
from throwing to returning `Result<T>`. The error codes map as:
- IO failures → `Error::Code::IoError`
- Missing fields / bad format → `Error::Code::FormatRejected`
- Duplicate IDs / unsupported version → `Error::Code::FormatRejected`

### 2c. `Demuxer::parseTrack()` → `Result<>`

**Files:**
- `include/ao/media/mp4/Demuxer.h:42` — change from `std::string` to `Result<>`
- `lib/media/mp4/Demuxer.cpp` — replace `return "error string"` with `makeError`
- `lib/audio/AlacDecoderSession.cpp` — update call site

### 2d. `DictionaryStore::put()` — fix misleading comment

**File:** `include/ao/library/DictionaryStore.h:40`

Remove the "or 0 on failure" clause. LMDB failures already throw `ao::Exception` via
`ThrowError.h`; this is correct for low-level storage defects.

### 2e. Update tests

**File:** `test/unit/runtime/LibraryExportImportTest.cpp`

- `REQUIRE_NOTHROW(exportToYaml(...))` → `REQUIRE(exportToYaml(...))`
- `REQUIRE_NOTHROW(importFromYaml(...))` → `REQUIRE(importFromYaml(...))`
- `REQUIRE_THROWS_WITH(...)` → `REQUIRE(!result); CHECK(result.error().code == ...)`

### 2f. Update async callers (minimal)

**File:** `app/runtime/LibraryMutationService.cpp`

The async wrappers (`importLibraryAsync`, `exportLibraryAsync`) currently call the sync
methods which throw. After conversion they return `Result<>`. The async wrappers should
check the result and propagate:
- If sync call fails, log and return (error stays as log entry for now)
- Per the doc: "async workflows that call fallible services should carry `Result<T>` back
  to the UI thread" — but converting the full async chain to `Task<Result<>>` is deferred
  to Phase 4

### Verification Phase 2

- [ ] Build succeeds
- [ ] `LibraryExportImportTest` passes with new assertions
- [ ] `Demuxer` callers compile and pass tests
- [ ] Import/export functionality works end-to-end

---

## Phase 3: `bool+errorMessage` → `optional<Error>`

### 3a. `SmartListSource::QueryState`

**Files:**
- `app/runtime/SmartListSource.h:61-67` — struct definition
- `app/runtime/SmartListSource.cpp:85,92` — error assignments
- `app/runtime/SmartListEvaluator.cpp:264,342,386,459` — error checks
- `app/linux-gtk/list/SmartListDialog.cpp:426` — UI error check

Replace `bool hasError` + `std::string errorMessage` with `std::optional<ao::Error> error`.

### 3b. `ProjectionTypes::FilterStatusChanged`

**Files:**
- `app/runtime/ProjectionTypes.h:102-109` — struct definition
- `app/runtime/ViewService.cpp:280` — error assignment
- `app/linux-gtk/track/TrackQuickFilter.cpp:161` — error check

Replace `bool hasError` + `std::string errorMessage` with `std::optional<ao::Error> error`.

### Verification Phase 3

- [ ] Build succeeds
- [ ] Smart list dialog works (manual check)
- [ ] Quick filter error display works (manual check)

---

## Phase 4: Async boundary + silent error swallowing

### 4a. `ImportExportCoordinator` — standardize error handling

**File:** `app/linux-gtk/portal/ImportExportCoordinator.cpp`

Remove redundant try/catch blocks from `executeExportTask()` (line 325) and
`onLibraryImportSelected()` (line 374). After Phase 2, the sync APIs no longer throw
for recoverable failures. Unexpected exceptions are caught by `spawnWithLifetime`'s
root handler.

Add missing try/catch or result-check to `scanLibrary()` (line 86) and
`executeImportTask()` (line 165), matching the pattern used in `executeExportTask`.

### 4b. `GtkControlExecutor` — distinguish `ao::Exception`

**File:** `app/linux-gtk/app/GtkControlExecutor.cpp:74`

Add `catch (ao::Exception const&)` before `catch (std::exception const&)` for better
diagnostics.

### 4c. `LayoutNode` — narrow catch scope

**File:** `app/linux-gtk/layout/document/LayoutNode.h:94,160`

Replace `catch (...)` in `asInt64()` and `asDouble()` with explicit
`catch (std::invalid_argument const&)` and `catch (std::out_of_range const&)`.

### Verification Phase 4

- [ ] Build succeeds
- [ ] Import with bad file → error notification appears (manual)
- [ ] Export to unwritable path → error notification appears (manual)
- [ ] Layout conversion of garbage value returns default (manual)

---

## Dependency Graph

```
Phase 0 (Infrastructure)
  ├── Phase 1 (ConfigStore std::logic_error)
  ├── Phase 2 (API throw→Result)
  │     └── Phase 4 (async boundary, depends on Phase 2 sync APIs)
  └── Phase 3 (bool+errorMessage, independent)
```

Phase 3 can run in parallel with Phases 1+2. Phase 4 should follow Phase 2.

## Key Files Summary

| File | Phase | Change |
|------|-------|--------|
| `include/ao/Exception.h` | 0 | Add `Error::Code` field + new ctor + new throwException overloads |
| `include/ao/Error.h` | 0 | Add `InvalidState` enum value |
| `app/linux-gtk/main.cpp`, `app/cli/main.cpp` | 0 | Two-tier catch blocks |
| `app/runtime/ConfigStore.h`, `.cpp` | 1 | `std::logic_error` → `ao::Exception` |
| `app/runtime/LibraryExporter.h`, `.cpp` | 2 | `void` → `Result<>`, throw→makeError |
| `app/runtime/LibraryImporter.h`, `.cpp` | 2 | `void` → `Result<>`, ~12 throw→makeError |
| `include/ao/media/mp4/Demuxer.h`, `lib/media/mp4/Demuxer.cpp` | 2 | `string` → `Result<>` |
| `app/runtime/LibraryMutationService.cpp` | 2 | Check Result in async wrappers |
| `app/runtime/SmartListSource.h`, `.cpp` | 3 | `hasError+errorMessage` → `optional<Error>` |
| `app/runtime/ProjectionTypes.h` | 3 | Same pattern |
| `app/linux-gtk/portal/ImportExportCoordinator.cpp` | 4 | Standardize error handling |
| `app/linux-gtk/app/GtkControlExecutor.cpp` | 4 | ao::Exception catch |
| `app/linux-gtk/layout/document/LayoutNode.h` | 4 | Narrow catch scope |

## Design Decisions

1. **Async APIs stay `Task<void>` for now.** Converting to `Task<Result<T>>` touches all
   callers and is a mechanical rewrite. The design doc says avoid those. Once sync APIs
   return `Result`, errors are caught before becoming exceptions. A future phase can
   introduce `Task<Result<T>>` once the UI notification pattern stabilizes.

2. **Old constructor/throwException overloads are kept (not deleted).** This avoids
   forcing all existing 60+ throwException call sites to add `Error::Code` immediately.
   They default to `Generic` and can be updated incrementally.

3. **`LibraryImporter` validation helpers change return types** from `ValidatedImport`/`void`
   to `Result<ValidatedImport>`/`Result<>`. This is the right API contract per the doc:
   validation failures are recoverable (show error, let user fix YAML) and should not throw.

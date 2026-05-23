# Library Scan Naming Plan

## Context

The current code uses `Import` for two different operations:

1. Importing a library from YAML.
2. Adding or updating tracks by reading audio files from disk.

The second operation no longer matches the intended library-management model. A
music library should be owned by a single root folder. Track creation, technical
metadata refresh, missing-file detection, and changed-file handling should all
come from scanning that root folder. Creating a library should run an initial
scan automatically.

This plan narrows `Import` to external structured data, especially YAML
backup/restore, and moves folder-based track management under `Scan`.

## Target Vocabulary

| Concept | Target name | Notes |
| --- | --- | --- |
| YAML file -> library data | `Import Library Data` / `LibraryYamlImporter` | UI emphasizes "Import/Export Data", code uses `LibraryYamlImporter` to leave room for other formats. |
| Library root folder -> scan plan | `Scan Library` / `LibraryScanner` | This is the primary folder-management operation. (Consider `Update Library` for UI). |
| Apply scan plan to DB | `ScanPlanExecutor` | Replaces the misleading `ImportWorker` role. |
| Library creation | `Create Library` | Select root folder, initialize DB, scan once. |
| Manual audio-file add | Removed | Folder scan is the only track ingestion path. |
| Shared progress event | `LibraryTaskProgressUpdated` | Covers scan, apply scan plan, YAML import/export if needed. |

## Desired User Model

The UI should expose these operations:

- `Create Library...`
- `Open Library...`
- `Scan Library`
- `Import Library Data...`
- `Export Library Data...`

The UI should not expose `Import Files` or `Add Music Files`. Users manage files
in the library root folder and use `Scan Library` to reconcile the database.

## Current Code Hotspots

### YAML import/export

- `app/runtime/LibraryImporter.h`
- `app/runtime/LibraryImporter.cpp`
- `app/runtime/LibraryExporter.h`
- `app/runtime/LibraryExporter.cpp`
- `app/cli/LibCommand.cpp`
- `app/linux-gtk/portal/ImportExportCoordinator.*`
- `test/unit/runtime/LibraryExportImportTest.cpp`

### File/folder scan and track ingestion

- `include/ao/library/LibraryScanner.h`
- `lib/library/LibraryScanner.cpp`
- `include/ao/library/ImportWorker.h`
- `lib/library/ImportWorker.cpp`
- `app/runtime/LibraryMutationService.h`
- `app/runtime/LibraryMutationService.cpp`
- `app/linux-gtk/portal/ImportExportCoordinator.*`
- `app/linux-gtk/app/MenuController.cpp`
- `test/unit/library/LibraryScannerTest.cpp`
- `test/unit/library/ImportWorkerTest.cpp`
- `test/unit/runtime/LibraryMutationServiceTest.cpp`

### Progress UI

- `app/linux-gtk/portal/ImportProgressIndicator.*`
- `app/linux-gtk/portal/ImportProgressDialog.*`
- `app/linux-gtk/layout/components/StatusComponents.cpp`
- `app/runtime/LibraryMutationService.h`
- `app/runtime/LibraryMutationService.cpp`

## Phase 1: Remove Manual File Import From Product Surface

### 1a. Remove menu action

**File:** `app/linux-gtk/app/MenuController.cpp`

Remove:

- `Import Files` menu item.
- `win.import-files` action.
- The call to `ImportExportCoordinator::importFiles()`.

Keep `Scan Library`.

### 1b. Remove or hide file-import dialog entry points

**Files:**

- `app/linux-gtk/portal/ImportExportCoordinator.h`
- `app/linux-gtk/portal/ImportExportCoordinator.cpp`

Remove public file-import entry points from the GTK surface:

- `importFiles()`
- `importFilesFromPath()`
- `onImportFolderSelected()`
- `executeImportTask()` if it is only used by manual import

Do not remove scan-plan application yet. This phase is about product surface,
not internals.

### Verification Phase 1

- [ ] GTK build succeeds.
- [ ] File menu no longer shows `Import Files`.
- [ ] `Scan Library` still builds and applies a plan.
- [ ] YAML import/export actions still work.

## Phase 2: Make Library Creation Scan Once

### 2a. Identify library creation flow

Find the flow that creates or opens a new library root. The current GTK
coordinator has `openLibrary()` and `openMusicLibrary()`; if library creation is
handled elsewhere, wire the scan there instead of adding a second creation path.

### 2b. Run initial scan after creating a library

After a new library root is established:

```cpp
auto plan = co_await runtime.mutation().buildScanPlanAsync();
co_await runtime.mutation().applyScanPlanAsync(std::move(plan));
```

This should run only for new libraries, not every open of an existing library.

### 2c. User-facing progress and cancellation

Initial scan should use the same progress channel as manual `Scan Library`.
Avoid a separate import dialog unless the operation already has a reusable
progress component.

**Note:** Ensure the scan task supports cancellation (`std::stop_token` or `CancellationToken`) so users can safely abort long-running initial scans of huge directories.

### Verification Phase 2

- [ ] Creating a library scans the root folder once.
- [ ] Reopening an existing library does not rescan automatically.
- [ ] Empty folders create a valid empty library.
- [ ] Unsupported files do not abort library creation.

## Phase 3: Rename `ImportWorker` To `ScanPlanExecutor`

### 3a. Rename files and class

Rename:

| From | To |
| --- | --- |
| `include/ao/library/ImportWorker.h` | `include/ao/library/ScanPlanExecutor.h` |
| `lib/library/ImportWorker.cpp` | `lib/library/ScanPlanExecutor.cpp` |
| `test/unit/library/ImportWorkerTest.cpp` | `test/unit/library/ScanPlanExecutorTest.cpp` |
| `ao::library::ImportWorker` | `ao::library::ScanPlanExecutor` |
| `ImportResult` | `ScanApplyResult` |

Update CMake file lists accordingly.

### 3b. Drop direct file-list constructor

The old worker can build a scan plan from an arbitrary file list. Under the
folder-owned model, that constructor should disappear:

```cpp
ScanPlanExecutor(MusicLibrary& ml,
                 ScanPlan plan,
                 ProgressCallback progressCallback,
                 FinishedCallback finishedCallback); // Consider passing a CancellationToken here
```

Delete `buildPlanFromFiles()` after all callers use `LibraryScanner`.

### 3c. Rename behavior comments

Replace comments such as "Run import" with "Apply scan plan" or "Process scan
item".

### Verification Phase 3

- [ ] Build succeeds.
- [ ] `ScanPlanExecutorTest` covers new, changed, unchanged, missing, unsupported, and error items.
- [ ] No references to `ImportWorker` remain outside historical docs.

## Phase 4: Clean Up Runtime Service API

### 4a. Remove file-import APIs

**Files:**

- `app/runtime/LibraryMutationService.h`
- `app/runtime/LibraryMutationService.cpp`

Remove:

- `importFiles()`
- `importFilesAsync()`
- `ImportFilesReply` usage if it is now dead.
- `scanLibraryAsync(std::filesystem::path dir)` because it only collected files
  for manual import and is not the same as scanning the active library root.

Keep:

- `buildScanPlanAsync()`
- `applyScanPlanAsync(library::ScanPlan plan)`
- `importLibraryAsync(std::filesystem::path path)` for YAML.
- `exportLibraryAsync(std::filesystem::path path, rt::ExportMode mode)`.

### 4b. Update apply implementation

Replace `ao::library::ImportWorker` with `ao::library::ScanPlanExecutor`.

Progress text should be scan-specific:

- Planning: `Scanning: <name>`
- Applying: `Updating: <name>` or `Applying scan: <name>`

Avoid `Importing:` for folder scan work.

### Verification Phase 4

- [ ] Runtime tests compile without `importFiles*`.
- [ ] Scan plan application still emits track mutation events.
- [ ] Missing-file scan items update manifest status.
- [ ] Changed-file scan items update technical properties without overwriting curated metadata.

## Phase 5: Rename Shared Progress Types

### 5a. Runtime event rename

Rename:

| From | To |
| --- | --- |
| `ImportProgressUpdated` | `LibraryTaskProgressUpdated` |
| `onImportProgress()` | `onLibraryTaskProgress()` |
| `onImportCompleted()` | `onLibraryTaskCompleted()` or remove if only scan callers remain |
| `_importProgressSignal` | `_libraryTaskProgressSignal` |
| `_importCompletedSignal` | `_libraryTaskCompletedSignal` |

If completion semantics differ between scan, YAML import, and export, prefer a
single typed event later:

```cpp
enum class LibraryTaskKind { Scan, ApplyScanPlan, ImportYaml, ExportYaml };
```

Do not add this enum unless the UI needs different behavior per task.

### 5b. GTK progress rename

Rename:

| From | To |
| --- | --- |
| `ImportProgressIndicator` | `LibraryTaskProgressIndicator` |
| `ImportProgressDialog` | `LibraryTaskProgressDialog` or remove |
| `status.importProgress` | `status.libraryTaskProgress` |
| Display name `Import Progress` | `Library Task Progress` or `Library Progress` |

Prefer `Library Progress` for visible UI text.

### Verification Phase 5

- [ ] Layout component registry accepts the new status component type.
- [ ] Existing default layout uses the new component type.
- [ ] Old saved layouts either migrate or fail with a clear compatibility rule.

## Phase 6: Clarify YAML Import Names

This phase is lower priority because YAML import is a legitimate import.

### 6a. Rename class only

Rename:

| From | To |
| --- | --- |
| `LibraryImporter` | `LibraryYamlImporter` |

Keep `importFromYaml()` unless a broader API cleanup is already happening.

### 6b. Clarify UI labels

Use intention-based names for the UI, hiding the technical format (YAML) but remaining versatile enough for non-backup use cases:

- `Import Library Data...`
- `Export Library Data...`

In file dialogs, describe the file type as `Aobus Library Data (*.yaml)` to leave room for future formats (like JSON) without polluting the high-level UI actions.

### Verification Phase 6

- [ ] YAML import/export tests still pass.
- [ ] CLI `lib import` remains compatible or has an intentional migration note.
- [ ] No code path confuses YAML import with folder scan.

## Suggested Final API Shape

```cpp
// YAML.
auto importer = rt::LibraryYamlImporter{library};
auto result = importer.importFromYaml(path, rt::ImportMode::Restore);

// Folder scan.
auto scanner = library::LibraryScanner{library};
auto plan = scanner.buildPlan(progress);

auto executor = library::ScanPlanExecutor{library, std::move(plan), progress, finished};
executor.run();
auto result = executor.result();
```

Runtime facade:

```cpp
async::Task<library::ScanPlan> buildScanPlanAsync();
async::Task<void> applyScanPlanAsync(library::ScanPlan plan);
async::Task<void> importLibraryAsync(std::filesystem::path path);
async::Task<void> exportLibraryAsync(std::filesystem::path path, rt::ExportMode mode);
```

## Migration Rules

1. `Import` means YAML or another external structured library format.
2. `Scan` means reconciling the active library root folder with the database.
3. Track creation from audio files happens only through scan-plan application.
4. Avoid user-facing "add files" language.
5. Keep each phase independently buildable.
6. Avoid changing YAML import behavior while removing manual file import.

## Future Considerations

- **Missing Files Purge:** When a file is physically removed, the scan marks it as `Missing` rather than deleting it outright from the database. In the future, consider adding a `Purge Missing Tracks` or `Clean Up Library` operation so users can permanently remove ghost entries.
- **Format-Agnostic API:** Keeping `importLibraryAsync(path)` agnostic of YAML allows future extensions (e.g., `LibraryJsonImporter`) to be routed automatically based on file extensions.

## Final Verification

- [ ] `rg -n "Import Files|importFiles|ImportWorker|Importing:" app include lib test`
  returns no active-code references.
- [ ] `rg -n "LibraryImporter" app include lib test` returns no active-code
  references if Phase 6 is completed.
- [ ] New library creation scans once.
- [ ] Manual `Scan Library` detects new, changed, missing, unchanged,
  unsupported, and error items.
- [ ] YAML import/export still works end to end.
- [ ] Existing tests pass, with renamed test files reflected in CMake.

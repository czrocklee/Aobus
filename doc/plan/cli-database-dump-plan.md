# Plan: Distributed `dump` Subcommands for Database Debugging

## Context

Debugging the Dictionary ID crash required manually inspecting LMDB state. Currently there's no way to see raw database contents. Instead of a centralized `lib dump`, distribute `dump` subcommands to each existing domain command so `--help` is clear and each dump can have domain-specific options (e.g., `track dump --id 42`).

## Architecture

```
aobus track dump              # all tracks (hot + cold, resolved)
aobus track dump --id 42      # single track
aobus track dump --raw        # hex dump raw bytes
aobus track dump --yaml       # machine output

aobus list dump               # all lists
aobus list dump --raw
aobus list dump --yaml

aobus lib dump                # meta + dict + manifest + resources
aobus lib dump --dict         # dictionary only
aobus lib dump --manifest     # manifest only
aobus lib dump --raw
aobus lib dump --yaml
```

## Changes Summary

| File | Change |
|------|--------|
| `include/ao/library/DictionaryStore.h` | Add `size()` — 1 line, header-only |
| `include/ao/library/ListView.h` | Add `rawData()` — 1 line, header-only |
| `app/cli/DumpUtils.h` | **New file** — shared `hexDump()` + `resolveDict()` helpers |
| `app/cli/LibCommand.cpp` | Add `lib dump` (meta, dict, manifest, resources) |
| `app/cli/TrackCommand.cpp` | Add `track dump` (all + `--id N`), migrate `show --json` to `--yaml` |
| `app/cli/ListCommand.cpp` | Add `list dump` |
| `app/cli/CMakeLists.txt` | Add `DumpUtils.h` (header-only, no build change needed) |

No `.cpp` additions. `DumpUtils.h` is inline-only.

## Step 0: Migrate Existing Options

Currently, `track show` has a `--json` option. For consistency with the rest of the Aobus repository (which uses `ryml` and YAML for config/export), we will migrate existing `--json` options in the CLI to `--yaml`. 
- Update `TrackCommand.cpp`: `showCmd->add_flag("-y,--yaml", "output as YAML");`
- Refactor the printing logic from JSON generation to YAML generation.

## Step 1: API Additions

### DictionaryStore::size() (`include/ao/library/DictionaryStore.h`)

```cpp
std::size_t size() const noexcept { return _idToStringStorage.size(); }
```

CLI is a separate process — everything is loaded from LMDB, no reserved entries. Simple.

### ListView::rawData() (`include/ao/library/ListView.h`)

```cpp
std::span<std::byte const> rawData() const noexcept { return _payload; }
```

For `--raw` hex dump of list payloads.

## Step 2: Shared Utilities (`app/cli/DumpUtils.h`)

```cpp
#pragma once

// hexDump(span<byte const>, ostream&)
//   Standard 16-byte-per-line format: "  offset  hex...  |ascii|"
//
// resolveDict(dict, id) -> string_view
//   Safe resolution: returns "" for id==0, dict.getOrDefault(id, "<INVALID>") otherwise
```

Both trivial, both `inline`.

## Step 3: `track dump` (`TrackCommand.cpp`)

Register under existing `track` command alongside `show`/`create`/`delete`.

```
track dump [--id N] [--raw] [--yaml]
```

- Default: iterate all tracks, for each print:
  - `Track ID: N`
  - Hot header fields (title, artistId→name, albumId→name, etc.) — each showing `"resolved" (ID: N)`
  - Tag Bloom (hex), Tags: each `"resolved" (ID: N)`
  - Cold header fields (duration, sampleRate, URI, etc.)
  - Custom KV entries
- `--id N`: single track lookup via `reader.get(TrackId{N}, Both)`
- `--raw`: hex dump hot header (36B), hot payload, cold header (36B), cold payload
- Guards: `isHotValid()` before hot fields, `isColdValid()` before cold fields
- `--yaml`: Output as structured YAML (e.g. `tracks:` followed by list of items)

## Step 4: `list dump` (`ListCommand.cpp`)

Register under existing `list` command.

```
list dump [--raw] [--yaml]
```

- For each list: ID, name, description, type (smart/manual), parentId, filter (if smart), track IDs (if manual)
- `--raw`: hex dump via `view.rawData()`
- `--yaml`: Output as structured YAML (e.g. `lists:` followed by list of items)

## Step 5: `lib dump` (`LibCommand.cpp`)

Register under existing `lib` command. Covers the 4 "infrastructure" databases.

```
lib dump [--dict] [--manifest] [--meta] [--resources] [--raw] [--yaml]
```

Default (no filter flags) = dump all 4.

### dumpMeta
- magic (hex), libraryVersion, flags (hex), createdAt (formatted), libraryId (UUID formatted)
- `--raw`: 40-byte header hex
- Uses existing `formatUuid()` / `formatTimestamp()` helpers

### dumpDictionary
- `for (i = 1; i <= dict.size(); ++i)` → `ID → "string"`
- Header line: total count

### dumpManifest
- Per entry: URI, trackId, fileSize, mtime, status (decoded to Available/Missing/Error)
- `--raw`: hex via `reader.databaseReader()` (already public)

### dumpResources
- Header: count + total bytes
- Per entry: resource ID, size, hex preview (first 64 bytes)
- `--raw`: full hex dump

## Step 6: Verification

```bash
./build.sh debug
aobus track dump                # populated library
aobus track dump --id 1 --raw   # single track with hex
aobus list dump
aobus lib dump                  # meta + dict + manifest + resources
aobus lib dump --dict           # dict only
```

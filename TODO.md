# RockStudio TODO (User-Centric)

Goal: build a tag-first music manager with maximum flexibility for personal use.

## P0: Core Tag Workflow (Highest Priority)

1. As a user, I want to add, remove, and rename tags for one track quickly.
2. As a user, I want bulk tag editing for selected tracks (add/remove/replace tags).
3. As a user, I want auto-complete suggestions from existing tags when editing.
4. As a user, I want to search tracks by tag with include/exclude logic (`#jazz and not #live`).
5. As a user, I want to normalize tags (case-insensitive, trim spaces, alias mapping).
6. As a user, I want a dedicated tag management screen to merge duplicates (`hip-hop` + `hiphop`).
7. As a user, I want to pin favorite tags for fast filtering.

## P0: Smart Lists / Rule Engine

1. As a user, I want live preview while typing list expressions.
2. As a user, I want expression validation with clear error location and fix hints.
3. As a user, I want expression snippets/templates (recently added, favorites, top rated).
4. As a user, I want nested list rules and saved reusable rule fragments.
5. As a user, I want list sorting rules in expressions (e.g., newest first, random, score desc).
6. As a user, I want date and numeric functions (`days_since_added`, `play_count > 10`).

## P0: Track Metadata and File Safety

1. As a user, I want side-by-side view of file tags vs RockStudio tags before syncing.
2. As a user, I want to write selected metadata/tag changes back to files safely.
3. As a user, I want dry-run mode for metadata writes with a clear diff preview.
4. As a user, I want conflict handling when file tags changed outside RockStudio.
5. As a user, I want reliable support for MP3/FLAC/M4A across malformed edge-case files.

## P1: Library Quality of Life

1. As a user, I want duplicate detection (same audio fingerprint or artist/album/title heuristic).
2. As a user, I want missing-tag dashboards (tracks with empty artist/genre/year, etc.).
3. As a user, I want quick actions like "tag all selected as workout".
4. As a user, I want undo/redo for tag edits and list edits.
5. As a user, I want auto-reload when files in the music folder are added/removed/changed.
6. As a user, I want import profiles (strict, fast, metadata-first, cover-art-heavy).

## P1: Discovery and Navigation

1. As a user, I want faceted filtering (artist, album, year, tags) with instant counts.
2. As a user, I want fuzzy search across title/artist/album/tags.
3. As a user, I want custom columns and column presets.
4. As a user, I want keyboard-first workflows (command palette, hotkeys for tagging/filtering).
5. As a user, I want "related tracks" suggestions based on tag overlap.

## P1: Playlist and Export

1. As a user, I want one-click export of any filtered view to `.m3u`.
2. As a user, I want export presets (relative paths, absolute paths, UTF-8, per-device).
3. As a user, I want sync-friendly export folders for mobile players.
4. As a user, I want deterministic export order with tie-break rules.

## P1: CLI Power Features

1. As a user, I want complete parity between GUI and CLI for tagging and list operations.
2. As a user, I want structured output modes (`json`, `csv`) for scripting.
3. As a user, I want batch commands (`track tag add --filter "#todo" --tag "reviewed"`).
4. As a user, I want import and verify commands with summary reports.

## P2: Data Portability and Backup

1. As a user, I want backup/restore of library DB and settings in one command.
2. As a user, I want import/export of tags and list definitions as JSON/YAML.
3. As a user, I want migration tools when schema changes.
4. As a user, I want optional sync to Git folder for versioning list/tag rules.

## P2: Advanced Personalization

1. As a user, I want user-defined computed fields (e.g., score from tags and play stats).
2. As a user, I want plugin hooks for custom parsers and custom expression functions.
3. As a user, I want weighted tag scoring (`#focus:0.8`, `#sleep:0.2`) for smart playlists.
4. As a user, I want rule-based auto-tagging during import.

## P2: Playback-Adjacent (Optional)

1. As a user, I want basic preview playback from the selected track.
2. As a user, I want play history, skip history, and simple ratings.
3. As a user, I want smart lists based on behavior (`skipped < 2`, `played this week`).

## P3: Nice-to-Have UX Polish

1. As a user, I want customizable themes and denser table modes for large libraries.
2. As a user, I want startup performance metrics and import performance stats.
3. As a user, I want in-app diagnostics page (DB size, broken files, parse failures).

## Implementation Order Proposal

1. Bulk tag editing + tag normalization + tag search operators.
2. List expression live validation + preview + better errors.
3. Undo/redo + conflict-safe metadata writeback + dry-run diff.
4. GUI/CLI parity for all tag and list operations.
5. Import/watcher robustness + duplicate detection + missing-tag dashboard.

# RockStudio Versioning

RockStudio tracks two independent versions.

## App Version

- `AppVersion` is the RockStudio release version.
- It is defined in `CMakeLists.txt` and generated into `rs/AppVersion.h`.
- Use semantic versioning for releases.
- Changing the app version does not require a library format change.

## Library Version

- `LibraryVersion` is the on-disk version of the music library format.
- It is stored in `LibraryMetaHeader` as `libraryVersion`.
- The library version changes whenever persisted library data would be interpreted differently by a newer build.

## When To Bump LibraryVersion

- Any incompatible change to persisted track, list, dictionary, resource, or metadata storage.
- Any change to stored smart-list expression syntax or semantics that could change how existing saved filters parse or evaluate.
- Any change that requires a migration step when opening an existing library.

## When Not To Bump LibraryVersion

- UI-only changes.
- Logging, diagnostics, or performance improvements.
- Additive expression editor improvements that do not change the meaning of existing stored expressions.
- Bug fixes that do not change persisted data interpretation.

## Current Implementation Rules

- `MusicLibrary` accepts only the current `LibraryVersion`.
- A newer library version is rejected.
- An older library version is treated as requiring migration.
- Pre-release development does not preserve backward compatibility automatically; if persisted behavior changes, bump `LibraryVersion`.

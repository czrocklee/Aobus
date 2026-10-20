---
id: development.library-format-changes
---
# Library format changes

## Scope

This guide is the contributor workflow for changing the library database or library YAML.
A later release must bring every released version of either format forward without losing curation, so each change has to decide its version before it lands.
Application-managed payloads follow the [managed-state schema guide](managed-state-schemas.md) instead.

[Decision 0020](../decision/0020-versioning-and-upgrade-policy.md) records the policy and its reasons.
The [database reference](../reference/library/storage/database.md#compatibility-and-versioning) and the [YAML reference](../reference/library/format/yaml.md#compatibility-and-versioning) own which changes are incompatible.

## Versions

Each format keeps its current version beside the last released one:

| Format | Current | Last released |
|---|---|---|
| Library database | `kLibraryVersion` in [`MetadataLayout.h`](../../include/ao/library/MetadataLayout.h) | `kLastReleasedLibraryVersion` |
| Library YAML | `kYamlFormatVersion` in [`LibraryTransfer.h`](../../app/include/ao/rt/library/LibraryTransfer.h) | `kLastReleasedYamlFormatVersion` |

The current version equals the last released one until the first incompatible change of the next cycle increments it by one.
Every later change in that cycle amends the unreleased version and keeps its number.
A [format-fixture test](../../test/unit/runtime/library/LibraryFormatFixtureTest.cpp) enforces both rules at compile time.

A predicate-language change that alters the outcome of filter text the current language accepts is incompatible for both formats, as the [predicate language reference](../reference/query/predicate-language.md#compatibility-and-versioning) explains.

## Amend an unreleased version

A version is unreleased while it is one past its last released version.
An unreleased version has no data that a release must bring forward, so amending it needs no upgrade step.

1. Change the layout or schema together with its validation.
2. Update the owning reference so that it still describes exactly what the build reads and writes.
3. Regenerate the unreleased version's fixtures when the change alters what they hold. The [format-fixture test](../../test/unit/runtime/library/LibraryFormatFixtureTest.cpp) describes how each platform's library and `v1.yaml` were made, and each platform's library is made on that platform.

## Increment a version

The first incompatible change after a release increments the version.
Open and import refuse to build at that point: [`MusicLibrary.cpp`](../../lib/library/MusicLibrary.cpp) and [`LibraryYamlImporter.cpp`](../../app/runtime/library/LibraryYamlImporter.cpp) each assert that the current version is 1, and the change replaces that assertion with the path for the older version.

Following decision 0020, the change brings:

- for the database, an upgrade step from the previous version with its own frozen decoder; the first such change also builds the step runner, the pre-upgrade copy, rollback, and the confirmations;
- for YAML, a tree step from the previous version that converts the document without opening an audio file;
- fixtures of the new version made on every platform, beside the released fixtures, which are never regenerated;
- for each platform, a frozen dump of every named database's keys and values, which the step must reproduce when it upgrades that platform's fixture of the previous version.

Upgrade and rollback hold the open lock exclusively for their whole run, so they cannot then call `MusicLibrary::open`: it takes the same lock shared, and a holder in the same process conflicts with it as one in another process does.
The first database step therefore also gives them a way to open the environment under the lock they already hold.

No change moves the database directory or either of the [lock files beside it](../reference/library/storage/database.md#environment).

## Release

The release commit sets each last released version to its current version, so that the next incompatible change increments it again.
A format that did not change in the cycle already has equal versions.
From that commit, the fixtures of every released version are frozen.

## Validation

The format-fixture test reads the libraries made on each platform, so it runs on every host; follow [validation and review](test/validation-and-review.md) for the platforms a change needs.

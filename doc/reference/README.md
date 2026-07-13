---
id: reference.index
type: index
status: current
domain: documentation
summary: Routes exhaustive formats, languages, commands, protocols, and configuration reference.
---
# Reference documentation

Reference documents are authoritative lookup surfaces for exact names, fields, types, grammar, values, defaults, compatibility rules, and serialized shapes.

Expected areas include formats, languages, CLI surfaces, protocols, and configuration.
Reference should be exhaustive and mechanically organized; it should not carry architectural rationale or procedural contributor guidance.

Prefer generation from code or machine-readable schemas where practical, and lock hand-written reference against implementation tests when generation is not practical.
Use the [reference template](../template/reference.md).

## Persistence

- [Managed file locations](persistence/location.md) enumerate default Linux product paths and frontend overrides.
- [Application managed-state surface](persistence/application-config.md) enumerates managed YAML documents, registered groups, payload authorities, codecs, and version markers.

## Failure and reporting

- [Error value](failure/error.md) enumerates the shared recoverable error fields, codes, result wrapper, and helper.
- [Notification model](reporting/notification.md) enumerates runtime reporting identities, fields, enums, defaults, commands, and signals.

## Library

- [Library reference](library/README.md) routes its model, storage, and format surfaces by code boundary.

## Media

- [Supported audio files](media/audio-file.md) enumerates recognized extensions, visitor fields, codec and source mappings, cover roles, and encoded payload ranges.

## Resource

- [Resource blob](resource/blob.md) enumerates ids, content-derived creation, collision probing, raw store operations, scoped reads, and owned runtime reads.

## Query

- [Predicate expression language](query/predicate-language.md) enumerates grammar, variables, aliases, operators, literals, and units.
- [Format expression language](query/format-language.md) enumerates the string-producing subset and scalar fields.

## Playback

- [Audio quality surface](playback/quality-surface.md) enumerates quality levels, findings, fields, labels, verdict categories, and style tokens.
- [Decoder errors](playback/decoder-error.md) enumerate factory routing, operation code families, end-of-stream, and private translation behavior.
- [Playback session state](playback/session-state.md) enumerates the exact version 3 restorable listening-intent payload and compatibility gate.

## Presentation

- [Activity-status surface](presentation/activity-status.md) enumerates shared UIModel state, kinds, helpers, defaults, and commands.
- [Track presentation presets](presentation/track-preset.md) enumerates built-in ids, labels, menu order, and intent.

## Workspace

- [Workspace session state](workspace/session-state.md) enumerates the unversioned `workspace` group, nested view and presentation fields, defaults, and compatibility limits.

## Application shell

- [Shell layout document](shell/layout-document.md) enumerates the version 1 document, node, value, template, and tooltip surface.
- [Shell layout component state](shell/layout-state.md) enumerates the version 1 per-preset state document, stateful types, guards, and promoted fields.
- [Layout catalog and actions](shell/layout-catalog.md) enumerate descriptor vocabulary plus registered component and action ids.
- [Keyboard map](shell/keymap.md) enumerates neutral chord syntax, defaults, eligibility, and the override shape.

## Linux GTK

- [GTK MPRIS surface](linux-gtk/mpris.md) enumerates D-Bus identity, interfaces, members, mappings, metadata, and signals.

## CLI

- [CLI command surface](cli/command.md) enumerates options, commands, arguments, structured output fields, streams, and exits.

## TUI

- [TUI command surface](tui/command.md) enumerates startup options, commands, aliases, keys, overlays, mouse targets, and default paths.

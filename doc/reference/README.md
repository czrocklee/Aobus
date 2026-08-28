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

- [Managed file locations](persistence/location.md) enumerate default Linux and Windows product paths and frontend overrides.
- [Application managed-state surface](persistence/application-config.md) enumerates managed YAML documents, registered groups, payload authorities, schemas, and version markers.

## Failure and reporting

- [Error value](failure/error.md) enumerates the shared recoverable error fields, codes, result wrapper, and helper.
- [Fatal facility](failure/fatal.md) enumerates AO fatal categories, macros, diagnostics, sink registration, and abort behavior.
- [Exception carriers](failure/exception-carriers.md) enumerate the exact project carriers, throw regions, catch owners, and foreign exception boundaries.
- [Notification model](reporting/notification.md) enumerates runtime reporting identities, fields, enums, defaults, commands, and canonical feed updates.

## Library

- [Library reference](library/README.md) routes its model, storage, and format surfaces by code boundary.

## Media

- [Supported audio files](media/audio-file.md) enumerates recognized extensions, visitor fields, codec and source mappings, cover roles, and encoded payload ranges.

## Resource

- [Resource descriptors](resource/blob.md) enumerates ids, digest-derived creation, collision probing, descriptor store operations, and the runtime materialization surface.

## Query

- [Predicate expression language](query/predicate-language.md) enumerates grammar, variables, aliases, operators, literals, and units.
- [Format expression language](query/format-language.md) enumerates the string-producing subset and scalar fields.

## Playback

- [Playback application boundary](playback/application-boundary.md) enumerates the `PlaybackService` snapshot, event, and command surface.
- [PCM format surface](playback/pcm-format.md) enumerates logical signal fields, concrete encodings, byte layout, and lossless output candidates.
- [Audio quality surface](playback/quality-surface.md) enumerates quality levels, findings, fields, labels, verdict categories, and style tokens.
- [Decoder errors](playback/decoder-error.md) enumerate factory routing, operation code families, end-of-stream, and private translation behavior.
- [Playback session state](playback/session-state.md) enumerates the exact version 4 restorable listening-intent payload and compatibility gate.

## Presentation

- [Presentation text catalog](presentation/text-catalog.md) enumerates shared UIModel semantic inputs, exact English output, and open-id fallbacks.
- [Activity-status surface](presentation/activity-status.md) enumerates shared UIModel state, kinds, helpers, defaults, and commands.
- [Persisted presentation state](presentation/persisted-state.md) enumerates versioned GTK presentation documents, stable token authorities, validation, and compatibility behavior.
- [Track presentation presets](presentation/track-preset.md) enumerate built-in structural ids, menu order, and intent.

## Workspace

- [Workspace session state](workspace/session-state.md) enumerates the strict `workspace` group, versioned presentation vocabulary, exact fields, and remaining compatibility limits.

## Application shell

- [Shell layout document](shell/layout-document.md) enumerates the version 1 document, node, value, template, and tooltip surface.
- [Shell layout component state](shell/layout-state.md) enumerates the version 1 per-preset state document, stateful types, guards, and promoted fields.
- [Shared component vocabulary](shell/component-vocabulary.md) enumerates the component types more than one shell presents and the properties whose authored value means the same thing in each.
- [GTK layout schema and actions](shell/layout-schema.md) enumerate shared schema vocabulary plus registered GTK component and action ids.
- [Keyboard map](shell/keymap.md) enumerates neutral chord syntax, defaults, eligibility, and the override shape.
- [Library scan report](shell/library-scan-report.md) enumerates the verdicts a finished scan reduces to and the sentence every shell reports it with.

## Application

- [Desktop successor protocol](application/desktop-successor-protocol.md) enumerates the private GTK and WinUI marker, root, scan, validation, and remainder rules.

## Linux GTK

- [GTK MPRIS surface](linux-gtk/mpris.md) enumerates D-Bus identity, interfaces, members, mappings, metadata, and signals.

## CLI

- [CLI command surface](cli/command.md) enumerates options, commands, arguments, structured output fields, streams, and exits.

## TUI

- [TUI command surface](tui/command.md) enumerates startup options, commands, aliases, keys, overlays, mouse targets, and default paths.

## Windows

- [Windows desktop state](windows/desktop-state.md) enumerates native desktop settings and semantic theme files, fields, defaults, validation, and versioning.
- [Windows layout schema](windows/layout-schema.md) enumerates Windows shell component and action ids, accepted layout fields, native element mapping, style-key resolution, and the built-in preset documents.
